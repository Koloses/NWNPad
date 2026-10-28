// NWNPad - abstract run of a compiled script: every value carries where it came from (tags looked up,
// strings it may be, a journal state it reads, the journal tests it implies), every path is followed, and
// the journal writes are recorded with the tests on their way.
// Strings, sets and test lists are interned (small ids), so the values copied along the paths are plain data.
#include "ncsflow.h"
#include "nwsig.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>

namespace NcsFlow {
namespace {

std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// routines (nwscript.nss numbers)
enum : uint16_t {
    R_ExecuteScript = 8, R_GetItemPossessedBy = 30, R_CreateItemOnObject = 31, R_GetLocalInt = 51, R_SetLocalString = 57,
    R_GetStringUpperCase = 60, R_GetStringLowerCase = 61, R_IntToString = 92, R_GetTag = 168, R_ActionJumpToObject = 196,
    R_GetWaypointByTag = 197, R_GetObjectByTag = 200, R_ActionStartConversation = 204, R_ActionJumpToLocation = 214,
    R_GetNearestObjectByTag = 229, R_CreateObject = 243, R_BeginConversation = 255, R_JumpToLocation = 313,
    R_AddJournalQuestEntry = 367, R_JumpToObject = 385, R_GetIsObjectValid = 42, R_GetIsDead = 140, R_SetLocalInt = 55,
    R_GetMaster = 319, R_GetModule = 242, R_GetHenchman = 354, R_GetAssociateType = 748,
};

struct G { int q; char op; int n; bool operator==(const G& o) const { return q == o.q && op == o.op && n == o.n; } bool operator<(const G& o) const { return std::tie(q, op, n) < std::tie(o.q, o.op, o.n); } };
// tests: journal / local int (= ! < > l g against n), has item (h) / has not (x), object exists (o) / not (O)
G Not(G g) {
    switch (g.op) {
        case '=': g.op = '!'; break; case '!': g.op = '='; break;
        case '<': g.op = 'g'; break; case 'g': g.op = '<'; break;
        case '>': g.op = 'l'; break; case 'l': g.op = '>'; break;
        case 'h': g.op = 'x'; break; case 'x': g.op = 'h'; break;
        case 'o': g.op = 'O'; break; case 'O': g.op = 'o'; break;
    }
    return g;
}
char Flip(char op) { return op == '<' ? '>' : op == '>' ? '<' : op == 'l' ? 'g' : op == 'g' ? 'l' : op; }

uint64_t K2(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }

// interned sorted lists: id 0 = empty
template <class T> struct Lists {
    std::vector<std::vector<T>> v{std::vector<T>()};
    std::map<std::vector<T>, int> ids{{std::vector<T>(), 0}};
    std::unordered_map<uint64_t, int> uni, inter;
    int Id(std::vector<T> x) {
        std::sort(x.begin(), x.end()); x.erase(std::unique(x.begin(), x.end()), x.end());
        auto it = ids.find(x);
        if (it != ids.end()) return it->second;
        int id = (int)v.size(); v.push_back(x); ids.emplace(std::move(x), id); return id;
    }
    int One(const T& t) { return Id({t}); }
    int Union(int a, int b) {
        if (a == b || !b) return a;
        if (!a) return b;
        if (a > b) std::swap(a, b);
        auto it = uni.find(K2(a, b));
        if (it != uni.end()) return it->second;
        std::vector<T> x = v[a]; x.insert(x.end(), v[b].begin(), v[b].end());
        int r = Id(std::move(x)); uni[K2(a, b)] = r; return r;
    }
    int Inter(int a, int b) {
        if (a == b) return a;
        if (!a || !b) return 0;
        if (a > b) std::swap(a, b);
        auto it = inter.find(K2(a, b));
        if (it != inter.end()) return it->second;
        std::vector<T> x;
        std::set_intersection(v[a].begin(), v[a].end(), v[b].begin(), v[b].end(), std::back_inserter(x));
        int r = Id(std::move(x)); inter[K2(a, b)] = r; return r;
    }
    size_t Size(int a) const { return v[a].size(); }
};

struct Val {
    int taint = 0;          // tags it depends on (set of string ids)
    int str = -1;           // the strings it may be (set of string ids), -1 unknown
    bool isInt = false, self = false, invalid = false, mod = false, pc = false;   // a constant int, OBJECT_SELF, OBJECT_INVALID, GetModule(), the player
    int iv = 0;
    int jq = -1;            // the journal state of this quest (string id; "\x04name": a local int)
    int cond = 0, neg = 0;  // tests that hold when it is true / false (guard list ids)
    int any = 0;            // when true, one of these items is had (guard list of 'h' tests: an OR)
};
const size_t MaxStr = 8;

struct Ctl { int pc; int side; Val v; };     // a test passed on the way: true (1) / false (2) side, both (3)
struct St {
    bool live = false;
    std::vector<Val> stk;
    int bp = 0;
    std::vector<int> bps;
    std::vector<Ctl> ctl;                     // sorted by pc
};

struct Key {
    int pc; std::vector<int> ret;
    // deeper calls first (a call returns before the code after it runs), then in code order
    bool operator<(const Key& o) const { return ret.size() != o.ret.size() ? ret.size() > o.ret.size() : pc != o.pc ? pc < o.pc : ret < o.ret; }
};

struct Run {
    const uint8_t* d; size_t n;
    Info info;
    std::vector<std::string> S;               // strings
    std::unordered_map<std::string, int> Sid;
    Lists<int> sets;
    Lists<G> gl;
    std::vector<char> target;                 // where paths meet: jump targets, returns, deferred code
    // functions (JSR targets): those that matter (they, or what they call, write the journal, create, look up
    // tags ...) are followed; the others are stepped over (their net effect on the stack)
    std::vector<int> funcs;                   // entry points, sorted
    std::map<int, bool> relevant;
    std::map<int, int> delta;                 // net stack slots of a call (args popped: negative)
    bool all = false;                         // follow every function (a condition script)
    std::map<Key, St> states;
    std::map<Key, int> visits;
    std::set<Key> work;
    struct CallRec { int guards = 0; bool first = true, over = false; };
    std::map<std::tuple<int, int, int>, CallRec> calls;          // (pc, quest, entry)
    struct CrRec { int newTag = 0, at = 0, guards = 0; bool first = true; };
    std::map<std::pair<uint16_t, int>, CrRec> creates;   // (routine, template) -> new tag (+1, 0 none), location tags, guards
    long steps = 0;
    bool retSlot = false;                     // stack slot 0 is the result (int StartingConditional)
    std::set<int> pass;                       // guard lists of the TRUE results
    bool rec = false;                         // the last pass: every path from its final state, recording

    std::unordered_map<int, std::string> orig;  // lower-cased string -> as written
    // a local int: "\x04" + "m:" (the module's), "p:" (the player's) or "o:" (someone else's) + its name as written
    std::string LocalKey(const Val& obj, int name) {
        auto o = orig.find(name);
        return std::string("\x04") + (obj.mod ? "m:" : obj.pc ? "p:" : obj.self ? "s:" : "o:") + (o != orig.end() ? o->second : S[name]);
    }
    int Sx(const std::string& s) { auto it = Sid.find(s); if (it != Sid.end()) return it->second; S.push_back(s); Sid[s] = (int)S.size() - 1; return (int)S.size() - 1; }

    // the items one of which is had when v is true
    int AnyEff(const Val& v) {
        std::vector<G> h;
        for (auto& g : gl.v[v.cond]) if (g.op == 'h') h.push_back(g);
        return h.empty() ? v.any : gl.Union(v.any, gl.Id(h));
    }
    bool Merge(Val& a, const Val& b) {
        Val o = a;
        a.taint = sets.Union(a.taint, b.taint);
        if (a.str >= 0) {
            if (b.str < 0) a.str = -1;
            else { int u = sets.Union(a.str, b.str); a.str = sets.Size(u) > MaxStr ? -1 : u; }
        }
        // a constant FALSE is never true: what holds when the merged value is true comes from the other one
        bool aF = o.isInt && o.iv == 0, bF = b.isInt && b.iv == 0, aT = o.isInt && o.iv != 0, bT = b.isInt && b.iv != 0;
        a.cond = aF ? b.cond : bF ? o.cond : gl.Inter(o.cond, b.cond);
        a.neg = aT ? b.neg : bT ? o.neg : gl.Inter(o.neg, b.neg);
        { int ea = AnyEff(o), eb = AnyEff(b); a.any = aF ? eb : bF ? ea : (ea && eb ? gl.Union(ea, eb) : 0); }
        if (a.isInt && (!b.isInt || b.iv != a.iv)) a.isInt = false;
        if (a.self && !b.self) a.self = false;
        if (a.invalid && !b.invalid) a.invalid = false;
        if (a.mod && !b.mod) a.mod = false;
        if (a.pc && !b.pc) a.pc = false;
        if (a.jq != b.jq) a.jq = -1;
        return o.taint != a.taint || o.str != a.str || o.isInt != a.isInt || o.self != a.self || o.invalid != a.invalid || o.mod != a.mod || o.pc != a.pc || o.jq != a.jq || o.cond != a.cond || o.neg != a.neg || o.any != a.any;
    }
    bool MergeSt(St& a, const St& b) {
        if (!a.live) { a = b; a.live = true; return true; }
        bool ch = false;
        size_t m = std::min(a.stk.size(), b.stk.size());
        if (a.stk.size() != m) { a.stk.resize(m); ch = true; }
        for (size_t i = 0; i < m; i++) ch |= Merge(a.stk[i], b.stk[i]);
        for (auto& c : b.ctl) {
            auto it = std::lower_bound(a.ctl.begin(), a.ctl.end(), c.pc, [](const Ctl& x, int pc) { return x.pc < pc; });
            if (it == a.ctl.end() || it->pc != c.pc) { a.ctl.insert(it, c); ch = true; continue; }
            int s = it->side | c.side;
            if (s != it->side) { it->side = s; ch = true; }
            ch |= Merge(it->v, c.v);
        }
        return ch;
    }

    int be16(size_t o) const { return (d[o] << 8) | d[o + 1]; }
    int be32(size_t o) const { return (int)(((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3]); }

    // instruction length, 0 if unknown
    int Len(size_t pc) const {
        if (pc + 2 > n) return 0;
        uint8_t op = d[pc], ty = d[pc + 1];
        switch (op) {
            case 0x01: case 0x03: case 0x26: case 0x27: case 0x21: return 8;
            case 0x02: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a: case 0x0d: case 0x0e: case 0x0f: case 0x10:
            case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1a:
            case 0x1c: case 0x20: case 0x22: case 0x2a: case 0x2b: case 0x2d: return 2;
            case 0x0b: case 0x0c: return ty == 0x24 ? 4 : 2;
            case 0x04:
                if (ty == 5) return pc + 4 <= n ? 4 + be16(pc + 2) : 0;
                if (ty == 3 || ty == 4 || ty == 6) return 6;
                return 0;
            case 0x05: return 5;
            case 0x1b: case 0x1d: case 0x1e: case 0x1f: case 0x23: case 0x24: case 0x25: case 0x28: case 0x29: return 6;
            case 0x2c: return 10;
        }
        return 0;
    }

    // what a script does that matters (sideFx), and the lookups whose results its tests use
    static bool SideFx(int r) {
        switch (r) {
            case R_AddJournalQuestEntry: case R_CreateObject: case R_CreateItemOnObject: case R_ActionJumpToLocation:
            case R_JumpToLocation: case R_ActionJumpToObject: case R_JumpToObject: case R_ActionStartConversation:
            case R_BeginConversation: case R_ExecuteScript: return true;
        }
        return false;
    }
    static bool Interesting(int r) {
        if (SideFx(r)) return true;
        switch (r) {
            case R_SetLocalString: case R_SetLocalInt: case R_GetItemPossessedBy: case R_GetObjectByTag: case R_GetNearestObjectByTag: case R_GetWaypointByTag: return true;
        }
        return false;
    }
    bool anyFx = false;
    int Owner(int pc) const {                 // the function a pc is in (-1: before any)
        auto it = std::upper_bound(funcs.begin(), funcs.end(), pc);
        return it == funcs.begin() ? -1 : *(it - 1);
    }
    bool Decode() {
        target.assign(n + 1, 0);
        auto mark = [&](long t) { if (t >= 0 && t <= (long)n) target[t] = 1; };
        std::vector<std::pair<int, int>> jsr;                     // (pc, target)
        std::vector<int> acts;                                    // pcs of interesting actions
        std::set<int> fs;
        for (size_t pc = 13; pc < n;) {
            int l = Len(pc);
            if (!l || pc + l > n) return false;
            uint8_t op = d[pc];
            if (op == 0x1d || op == 0x1e || op == 0x1f || op == 0x25) mark((long)pc + be32(pc + 2));
            if (op == 0x1e) { mark((long)pc + 6); jsr.push_back({(int)pc, (int)pc + be32(pc + 2)}); fs.insert((int)pc + be32(pc + 2)); }
            if (op == 0x2c) mark((long)pc + 16);
            if (op == 0x05 && Interesting(be16(pc + 2))) acts.push_back((int)pc);
            if (op == 0x05 && SideFx(be16(pc + 2))) anyFx = true;
            pc += l;
        }
        funcs.assign(fs.begin(), fs.end());
        for (int f : funcs) relevant[f] = false;
        for (int a : acts) { int o = Owner(a); if (o >= 0) relevant[o] = true; }
        for (bool ch = true; ch;) {
            ch = false;
            for (auto& j : jsr) { int o = Owner(j.first); if (o >= 0 && !relevant[o] && relevant[j.second]) { relevant[o] = true; ch = true; } }
        }
        return true;
    }
    // net stack effect of calling the function at f (follows its paths counting slots only)
    int Delta(int f, int depth = 0) {
        auto it = delta.find(f);
        if (it != delta.end()) return it->second;
        if (depth > 16) return 0;
        delta[f] = 0;                                             // (recursion)
        std::map<int, int> seen;
        std::vector<std::pair<int, int>> q{{f, 0}};
        int result = 0; bool got = false;
        while (!q.empty() && !got) {
            auto [pc, dd] = q.back(); q.pop_back();
            while (true) {
                if (seen.count(pc)) break;
                seen[pc] = dd;
                int l = Len(pc);
                if (!l) break;
                uint8_t op = d[pc], ty = d[pc + 1];
                int next = pc + l;
                switch (op) {
                    case 0x02: case 0x04: case 0x2a: dd++; break;
                    case 0x03: case 0x27: dd += be16(pc + 6) / 4; break;
                    case 0x05: {
                        int r = be16(pc + 2), argc = d[pc + 4];
                        if (r >= NWSIG_N) break;
                        const char* sig = NWSIG[r];
                        for (int i = 0; i < argc && sig[1 + i]; i++) dd -= sig[1 + i] - '0';
                        dd += sig[0] - '0';
                        break;
                    }
                    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a: case 0x0d: case 0x0e: case 0x0f: case 0x10: dd--; break;
                    case 0x0b: case 0x0c: dd += ty == 0x24 ? 1 - 2 * (be16(pc + 2) / 4) : -1; break;
                    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
                        dd -= ty == 0x3a ? 3 : 1; break;
                    case 0x1b: dd += be32(pc + 2) / 4; break;
                    case 0x21: dd -= (be16(pc + 2) - be16(pc + 6)) / 4; break;
                    case 0x2b: dd--; break;
                    case 0x1d: next = pc + be32(pc + 2); break;
                    case 0x1e: dd += Delta(pc + be32(pc + 2), depth + 1); break;
                    case 0x1f: case 0x25: dd--; q.push_back({pc + be32(pc + 2), dd}); break;
                    case 0x20: result = dd; got = true; break;
                }
                if (got || op == 0x20) break;
                pc = next;
            }
        }
        delta[f] = result;
        return result;
    }

    void Push(const Key& k, const St& s) {
        if (rec) return;
        St& t = states[k];
        if (MergeSt(t, s) && visits[k] < 6) work.insert(k);
    }

    std::string JE(int q) { return "nw_journal_entry" + S[q]; }
    // a local int that has to have been set (to something else than 0)
    bool SetLocal(const G& g) {
        if (S[g.q][0] != '\x04' || S[g.q].compare(1, 2, "s:") == 0) return false;   // (the runner's own: not a goal)
        switch (g.op) { case '!': return g.n == 0; case '=': return g.n != 0; case '>': return g.n >= 0; case 'g': return g.n >= 1; }
        return false;
    }
    bool IsJournal(const G& g) { return strchr("=!<>lg", g.op) && S[g.q][0] != '\x04'; }
    // what the tests passed on the way to here require
    int PathGuards(const St& s) {
        int g = 0;
        for (auto& c : s.ctl) if (c.side != 3) g = gl.Union(g, c.side == 1 ? c.v.cond : c.v.neg);
        return g;
    }
    void Tested(const Val& v) {
        if (!rec) return;
        for (int t : sets.v[v.taint]) if (!S[t].empty()) info.cond.insert(S[t]);
        if (v.jq >= 0 && S[v.jq][0] != '\x04') info.cond.insert(JE(v.jq));
        for (auto& g : gl.v[v.cond]) if (IsJournal(g)) info.cond.insert(JE(g.q));
    }

    static Val Pop(St& s) { if (s.stk.empty()) return Val(); Val v = s.stk.back(); s.stk.pop_back(); return v; }
    const std::vector<int>& Strs(const Val& v) { static const std::vector<int> none; return v.str >= 0 ? sets.v[v.str] : none; }
    int One(const Val& v) { return v.str >= 0 && sets.Size(v.str) == 1 ? sets.v[v.str][0] : -1; }

    void Action(St& s, int pc, uint16_t r, int argc) {
        if (r >= NWSIG_N) { s.live = false; return; }
        const char* sig = NWSIG[r];
        int ret = sig[0] - '0';
        Val a[8];
        int na = 0;
        for (int i = 0; i < argc && sig[1 + i]; i++) {
            int k = sig[1 + i] - '0';
            Val v;
            for (int j = 0; j < k; j++) { Val x = Pop(s); if (j == 0) v = x; else v.taint = sets.Union(v.taint, x.taint); }
            if (na < 8) a[na++] = v;
        }
        Val res;
        for (int i = 0; i < na; i++) res.taint = sets.Union(res.taint, a[i].taint);
        auto look = [&](const Val& v) { for (int t : Strs(v)) if (!S[t].empty()) res.taint = sets.Union(res.taint, sets.One(t)); };
        // found by a tag: true when it is there ("has the item" / "the object exists")
        auto found = [&](const Val& v, char op) {
            int t = One(v);
            if (t < 0 || S[t].empty()) return;
            G g{t, op, 0};
            res.cond = gl.One(g); res.neg = gl.One(Not(g));
        };
        switch (r) {
            case R_GetItemPossessedBy: look(a[1]); found(a[1], 'h'); break;
            case R_GetObjectByTag: case R_GetNearestObjectByTag: look(a[0]); found(a[0], 'o'); break;
            case R_GetWaypointByTag: look(a[0]); break;
            case R_GetIsObjectValid: res.cond = a[0].cond; res.neg = a[0].neg; break;
            case R_GetModule: res.mod = true; break;
            case 238: case 548: case 549: case 25: case 707: case 330: case 376: case 326: case 437: case 440: case 506: case 562: case 254:
                res.pc = true; break;                              // the player (speaker, entering, user, killer ...)
            case R_GetIsDead: {                                    // dead: the object is gone, as far as the quest goes
                std::vector<G> c;
                for (auto g : gl.v[a[0].cond]) if (g.op == 'o') { g.op = 'O'; c.push_back(g); }
                res.cond = gl.Id(c); res.neg = a[0].cond;
                break;
            }
            case R_GetMaster: case R_GetHenchman: case R_GetAssociateType:   // someone's associate (henchmen's scripts)
                if (a[0].self || na == 0) { G g{Sx("\x05master"), 'o', 0}; res.cond = gl.One(g); res.neg = gl.One(Not(g)); }
                break;
            case R_GetTag: if (a[0].self) res.str = sets.One(Sx("\x01")); break;
            case R_IntToString: res.str = sets.One(Sx(a[0].isInt ? std::to_string(a[0].iv) : std::string("\x02"))); break;
            case R_GetStringLowerCase: case R_GetStringUpperCase: res.str = a[0].str; break;
            case R_GetLocalInt: {                                  // the journal state, or a local int not of the runner
                int t = One(a[1]);
                if (t >= 0 && S[t].compare(0, 16, "nw_journal_entry") == 0) res.jq = Sx(S[t].substr(16));
                else if (t >= 0 && !S[t].empty()) res.jq = Sx(LocalKey(a[0], t));
                if (res.jq >= 0) { G g{res.jq, '!', 0}; res.cond = gl.One(g); res.neg = gl.One(Not(g)); }
                break;
            }
        }
        if (rec) switch (r) {
            case R_AddJournalQuestEntry: {
                int guards = PathGuards(s);
                for (int q : Strs(a[0])) {
                    if (S[q].empty()) continue;
                    auto& rc = calls[std::make_tuple(pc, q, a[1].isInt ? a[1].iv : -1)];
                    rc.guards = rc.first ? guards : gl.Inter(rc.guards, guards);
                    rc.first = false;
                    if (a[5].isInt && a[5].iv) rc.over = true;
                }
                break;
            }
            case R_SetLocalInt: {
                int t = One(a[1]);
                if (t >= 0 && !S[t].empty() && !a[0].self) info.setsInt.insert(LocalKey(a[0], t).substr(1));
                break;
            }
            case R_CreateObject:
                for (int t : Strs(a[1])) { auto& c = creates[{r, t}]; if (!c.newTag) c.newTag = One(a[4]) + 1; c.at = sets.Union(c.at, a[2].taint); int pg = PathGuards(s); c.guards = c.first ? pg : gl.Inter(c.guards, pg); c.first = false; }
                break;
            case R_CreateItemOnObject:
                for (int t : Strs(a[0])) { auto& c = creates[{r, t}]; if (!c.newTag) c.newTag = One(a[3]) + 1; int pg = PathGuards(s); c.guards = c.first ? pg : gl.Inter(c.guards, pg); c.first = false; }
                break;
            case R_ActionJumpToLocation: case R_JumpToLocation: case R_ActionJumpToObject: case R_JumpToObject:
                for (int t : sets.v[a[0].taint]) info.jumps.insert(S[t]);
                break;
            case R_ActionStartConversation: for (int t : Strs(a[1])) if (!S[t].empty()) info.convs.insert(S[t]); break;
            case R_BeginConversation: for (int t : Strs(a[0])) if (!S[t].empty()) info.convs.insert(S[t]); break;
            case R_ExecuteScript: for (int t : Strs(a[0])) if (!S[t].empty()) info.execs.insert(S[t]); break;
            case R_SetLocalString: for (int t : Strs(a[2])) if (!S[t].empty()) info.locals.insert(S[t]); break;
        }
        for (int i = 0; i < ret; i++) s.stk.push_back(res);
    }

    // one path from k until it meets another (or ends)
    void Path(const Key& k) {
        St s = states[k];
        if (!rec) visits[k]++;
        int pc = k.pc;
        std::vector<int> ret = k.ret;
        bool first = true;
        long budget = 400000;
        int prev = -1;
        while (s.live) {
            if (--budget < 0 || (!rec && ++steps > 400000)) return;
            if (!first && target[pc]) { Push({pc, ret}, s); return; }
            first = false;
            int l = Len(pc);
            if (!l) return;
            uint8_t op = d[pc], ty = d[pc + 1];
            int prevPc = prev; prev = pc;
            int next = pc + l;
            long sz = (long)s.stk.size();
            switch (op) {
                case 0x01: case 0x26: {                                       // CPDOWNSP / CPDOWNBP
                    int off = be32(pc + 2), cnt = be16(pc + 6) / 4;
                    long dst = (op == 0x01 ? sz : (long)s.bp) + off / 4;
                    for (int i = 0; i < cnt; i++) {
                        long si = sz - cnt + i, di = dst + i;
                        if (si >= 0 && di >= 0 && di < sz && si != di) {
                            s.stk[di] = s.stk[si]; Tested(s.stk[si]);
                            Val& w = s.stk[di];
                            if (w.isInt && cnt == 1) {           // a constant TRUE / FALSE: what holds on the way here
                                int pg = PathGuards(s);
                                if (w.iv) w.cond = gl.Union(w.cond, pg); else w.neg = gl.Union(w.neg, pg);
                            }
                            if (rec && di == 0 && retSlot && !(s.stk[si].isInt && s.stk[si].iv == 0)) {
                                int g = s.stk[si].cond;
                                for (auto& c : s.ctl) if (c.side != 3) g = gl.Union(g, c.side == 1 ? c.v.cond : c.v.neg);
                                if (s.stk[si].any && gl.Size(s.stk[si].any) > 1) {        // one of several items: a group
                                    std::string k = "\x06";
                                    std::vector<std::string> names;
                                    for (auto& x : gl.v[s.stk[si].any]) names.push_back(S[x.q]);
                                    std::sort(names.begin(), names.end());
                                    for (auto& nme : names) { if (k.size() > 1) k += '|'; k += nme; }
                                    g = gl.Union(g, gl.One(G{Sx(k), 'a', 0}));
                                }
                                pass.insert(g);
                            }
                        }
                    }
                    break;
                }
                case 0x03: case 0x27: {                                       // CPTOPSP / CPTOPBP
                    int off = be32(pc + 2), cnt = be16(pc + 6) / 4;
                    long src = (op == 0x03 ? sz : (long)s.bp) + off / 4;
                    for (int i = 0; i < cnt; i++) {
                        long si = src + i;
                        s.stk.push_back(si >= 0 && si < (long)s.stk.size() ? s.stk[si] : Val());
                    }
                    break;
                }
                case 0x02: s.stk.push_back(Val()); break;                     // RSADD
                case 0x04: {                                                   // CONST
                    Val v;
                    if (ty == 3) { v.isInt = true; v.iv = be32(pc + 2); }
                    else if (ty == 5) {
                        std::string raw((const char*)d + pc + 4, be16(pc + 2));
                        int id = Sx(Lower(raw));
                        orig.emplace(id, raw);                                   // (local variable names keep their case)
                        v.str = sets.One(id);
                    }
                    else if (ty == 6) { v.self = be32(pc + 2) == 0; v.invalid = (uint32_t)be32(pc + 2) == 0x7f000000u; }
                    s.stk.push_back(v);
                    break;
                }
                case 0x05: Action(s, pc, (uint16_t)be16(pc + 2), d[pc + 4]); break;
                case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x10: {   // comparisons
                    Val res;
                    if (op <= 0x0c && ty == 0x24) {
                        int cnt = 2 * (be16(pc + 2) / 4);
                        for (int i = 0; i < cnt; i++) res.taint = sets.Union(res.taint, Pop(s).taint);
                    } else {
                        Val b = Pop(s), a = Pop(s);
                        res.taint = sets.Union(a.taint, b.taint);
                        if (ty == 0x23) {
                            for (int t : Strs(a)) if (!S[t].empty()) res.taint = sets.Union(res.taint, sets.One(t));
                            for (int t : Strs(b)) if (!S[t].empty()) res.taint = sets.Union(res.taint, sets.One(t));
                            // the tag of something (the acquired item ...) against a constant: it is that item
                            int t = a.str < 0 ? One(b) : b.str < 0 ? One(a) : -1;
                            if (t >= 0 && !S[t].empty() && op <= 0x0c) {
                                G g{t, 'h', 0};
                                res.cond = gl.One(op == 0x0b ? g : Not(g)); res.neg = gl.One(op == 0x0b ? Not(g) : g);
                            }
                        }
                        char o = op == 0x0b ? '=' : op == 0x0c ? '!' : op == 0x0d ? 'g' : op == 0x0e ? '>' : op == 0x0f ? '<' : 'l';
                        if (ty == 0x20) {
                            G g{}; bool ok = false;
                            if (a.jq >= 0 && b.isInt) { g = {a.jq, o, b.iv}; ok = true; }
                            else if (b.jq >= 0 && a.isInt) { g = {b.jq, Flip(o), a.iv}; ok = true; }
                            if (ok) { res.cond = gl.One(g); res.neg = gl.One(Not(g)); }
                            else if (op <= 0x0c && (a.isInt || b.isInt)) {   // a test compared with TRUE / FALSE
                                const Val& v = a.isInt ? b : a; int k = a.isInt ? a.iv : b.iv;
                                if (k == 0 || k == 1) {
                                    bool same = (k == 1) == (op == 0x0b);
                                    res.cond = same ? v.cond : v.neg; res.neg = same ? v.neg : v.cond;
                                    if (same) res.any = v.any;
                                }
                            }
                            if (rec && a.jq >= 0 && S[a.jq][0] != '\x04') info.cond.insert(JE(a.jq));
                            if (rec && b.jq >= 0 && S[b.jq][0] != '\x04') info.cond.insert(JE(b.jq));
                        } else if (ty == 0x22 && op <= 0x0c && (a.invalid || b.invalid)) {   // against OBJECT_INVALID
                            const Val& v = a.invalid ? b : a;
                            res.cond = op == 0x0c ? v.cond : v.neg; res.neg = op == 0x0c ? v.neg : v.cond;
                        }
                    }
                    Tested(res);
                    s.stk.push_back(res);
                    break;
                }
                case 0x06: case 0x07: {                                        // LOGAND / LOGOR
                    Val b = Pop(s), a = Pop(s), res;
                    res.taint = sets.Union(a.taint, b.taint);
                    if (op == 0x06) { res.cond = gl.Union(a.cond, b.cond); res.any = a.any ? a.any : b.any; }
                    else {
                        res.neg = gl.Union(a.neg, b.neg);
                        int ea = AnyEff(a), eb = AnyEff(b);
                        if (ea && eb) res.any = gl.Union(ea, eb);
                    }
                    s.stk.push_back(res);
                    break;
                }
                case 0x22: {                                                   // NOTI
                    Val v = Pop(s), r;
                    r.taint = v.taint; r.cond = v.neg; r.neg = v.cond;
                    s.stk.push_back(r);
                    break;
                }
                case 0x08: case 0x09: case 0x0a: case 0x11: case 0x12: case 0x13:
                case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: {       // arithmetic
                    int popn = ty == 0x3a ? 6 : (ty == 0x3b || ty == 0x3c) ? 4 : 2, pushn = popn > 2 ? 3 : 1;
                    Val res;
                    if (popn == 2) {
                        Val b = Pop(s), a = Pop(s);
                        res.taint = sets.Union(a.taint, b.taint);
                        if (op == 0x14 && ty == 0x23 && a.str >= 0 && b.str >= 0) {      // string + string
                            std::vector<int> v;
                            for (int x : Strs(a)) for (int y : Strs(b)) if (v.size() < MaxStr) v.push_back(Sx(S[x] + S[y]));
                            res.str = sets.Id(v);
                        }
                        if ((op == 0x14 || op == 0x15) && ty == 0x20 && a.isInt && b.isInt) { res.isInt = true; res.iv = op == 0x14 ? a.iv + b.iv : a.iv - b.iv; }
                    } else for (int i = 0; i < popn; i++) res.taint = sets.Union(res.taint, Pop(s).taint);
                    for (int i = 0; i < pushn; i++) s.stk.push_back(res);
                    break;
                }
                case 0x19: case 0x1a: {                                        // NEG / COMP
                    Val v = Pop(s), r; r.taint = v.taint;
                    if (op == 0x19 && v.isInt) { r.isInt = true; r.iv = -v.iv; }
                    s.stk.push_back(r);
                    break;
                }
                case 0x1b: { int c = -be32(pc + 2) / 4; while (c-- > 0 && !s.stk.empty()) s.stk.pop_back(); break; }   // MOVSP
                case 0x23: case 0x24: case 0x28: case 0x29: {                  // DEC/INC ISP/IBP
                    long i = (op <= 0x24 ? sz : (long)s.bp) + be32(pc + 2) / 4;
                    if (i >= 0 && i < sz) s.stk[i].isInt = false;
                    break;
                }
                case 0x2a: s.bps.push_back(s.bp); s.stk.push_back(Val()); s.bp = (int)s.stk.size(); break;     // SAVEBP
                case 0x2b: if (!s.bps.empty()) { s.bp = s.bps.back(); s.bps.pop_back(); } if (!s.stk.empty()) s.stk.pop_back(); break;
                case 0x21: {                                                   // DESTRUCT
                    int size = be16(pc + 2) / 4, off = be16(pc + 4) / 4, keep = be16(pc + 6) / 4;
                    if (size > (int)s.stk.size()) size = (int)s.stk.size();
                    std::vector<Val> top(s.stk.end() - size, s.stk.end());
                    s.stk.resize(s.stk.size() - size);
                    for (int i = off; i < off + keep && i < (int)top.size(); i++) s.stk.push_back(top[i]);
                    break;
                }
                case 0x1d: next = pc + be32(pc + 2); break;                    // JMP
                case 0x1e: {                                                   // JSR
                    int f = pc + be32(pc + 2);
                    // nothing for us in there, or deep in helpers (the contexts would multiply): step over
                    if ((!all && !relevant[f]) || ret.size() >= 4) {
                        int dd = Delta(f);
                        for (; dd < 0 && !s.stk.empty(); dd++) s.stk.pop_back();
                        for (; dd > 0; dd--) s.stk.push_back(Val());
                        break;
                    }
                    ret.push_back(pc + 6);
                    Push({f, ret}, s);
                    return;
                }
                case 0x1f: case 0x25: {                                        // JZ / JNZ
                    Val v = Pop(s);
                    Tested(v);
                    int fall = op == 0x1f ? 1 : 2, jump = 3 - fall;
                    auto mark = [&](St& st, int side) {
                        auto it = std::lower_bound(st.ctl.begin(), st.ctl.end(), pc, [](const Ctl& x, int p) { return x.pc < p; });
                        if (it == st.ctl.end() || it->pc != pc) st.ctl.insert(it, Ctl{pc, side, v});
                        else { it->side |= side; Merge(it->v, v); }
                    };
                    // a && / || short cut tests a copy of the value left on the stack: on the jump it is known
                    bool copy = prevPc >= 0 && d[prevPc] == 0x03 && be32(prevPc + 2) == -4 && be16(prevPc + 6) == 4 && !s.stk.empty();
                    if (!rec) {
                        St t = s; mark(t, jump);
                        if (copy) { Val& k = t.stk.back(); k.isInt = true; k.iv = op == 0x1f ? 0 : 1; if (op == 0x1f) k.cond = 0; else k.neg = 0; }
                        Push({pc + be32(pc + 2), ret}, t);
                    }
                    mark(s, fall);
                    break;
                }
                case 0x20:                                                     // RETN
                    if (ret.empty() || ret.back() < 0) return;
                    next = ret.back(); ret.pop_back();
                    Push({next, ret}, s);
                    return;
                case 0x2c: {                                                   // STORE_STATE: deferred code follows the JMP
                    std::vector<int> r2 = ret; r2.push_back(-1);
                    Push({pc + 16, r2}, s);
                    break;
                }
                default: break;
            }
            pc = next;
        }
    }

    void Go() {
        if (n < 13 || memcmp(d, "NCS V1.0", 8) != 0 || !Decode()) return;
        info.ok = true;
        retSlot = d[13] == 0x02 && d[14] == 0x03;
        all = retSlot || n < 2048;                           // condition scripts and small ones: every function
        if (!all && !anyFx) return;                          // (the AI scripts: nothing for the quests)
        St s0; s0.live = true;
        states[{13, {}}] = s0;
        work.insert({13, {}});
        while (!work.empty() && steps <= 400000) {
            Key k = *work.begin();
            work.erase(work.begin());
            Path(k);
        }
        rec = true;
        for (auto& st : states) if (st.second.live) Path(st.first);
        for (auto& c : calls) {
            Call o;
            o.quest = S[std::get<1>(c.first)]; o.entry = std::get<2>(c.first); o.override_ = c.second.over;
            for (auto& g : gl.v[c.second.guards]) {
                o.guards.push_back({S[g.q], g.op, g.n});
                if (g.op == 'h' || g.op == 'O') o.tags.insert(S[g.q]);          // has the item / the object is gone
                if (SetLocal(g)) o.tags.insert(S[g.q]);                             // a local int set
            }
            info.calls.push_back(std::move(o));
        }
        if (pass.size() <= 16)
            for (int g : pass) {
                std::vector<Guard> v;
                for (auto& x : gl.v[g]) v.push_back({S[x.q], x.op, x.n});
                info.pass.push_back(std::move(v));
            }
        // what every way of returning TRUE requires: items had, objects gone, local ints; and when each way
        // needs one of several items ("has any of the tomes"), that group ("\x06a|b|c")
        bool firstAlt = true, eachOne = !pass.empty();
        std::set<std::string> any;
        for (int g : pass) {
            std::set<std::string> t; bool item = false;
            for (auto& x : gl.v[g]) {
                if (x.op == 'h' || x.op == 'O' || SetLocal(x)) t.insert(S[x.q]);
                if (x.op == 'h' || x.op == 'O') { any.insert(S[x.q]); item = true; }
            }
            for (auto& x : gl.v[g]) if (x.op == 'a') { t.insert(S[x.q]); item = true; }
            if (!item) eachOne = false;
            if (firstAlt) { info.req = t; firstAlt = false; }
            else { std::set<std::string> r; for (auto& y : info.req) if (t.count(y)) r.insert(y); info.req.swap(r); }
        }
        if (eachOne && any.size() > 1) {
            bool covered = false;
            for (auto& y : any) if (info.req.count(y)) covered = true;
            if (!covered) { std::string k = "\x06"; for (auto& y : any) { if (k.size() > 1) k += '|'; k += y; } info.req.insert(k); }
        }
        for (auto& c : creates) {
            Create o{c.first.first, S[c.first.second], c.second.newTag > 0 ? S[c.second.newTag - 1] : std::string(), {}, {}};
            for (int t : sets.v[c.second.at]) o.at.insert(S[t]);
            for (auto& g : gl.v[c.second.guards]) o.guards.push_back({S[g.q], g.op, g.n});
            info.creates.push_back(std::move(o));
        }
    }
};
}  // namespace

Info Analyze(const uint8_t* d, size_t n) {
    Run r{d, n};
    r.Go();
    return std::move(r.info);
}

bool Pass(const Guard& g, int st) {
    switch (g.op) {
        case '=': return st == g.n; case '!': return st != g.n;
        case '<': return st < g.n; case '>': return st > g.n;
        case 'l': return st <= g.n; case 'g': return st >= g.n;
    }
    return true;
}
}  // namespace NcsFlow
