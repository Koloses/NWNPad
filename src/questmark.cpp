// NWNPad - quest marker on the minimap (single player).
//
// The game has no quest markers, but the module knows which conversations move a quest on: dialogue
// nodes carry the journal tag and entry they set (DLG "Quest" / "QuestEntry"). With the server in this
// process we read the player's journal (tag + current entry), look through every creature, placeable
// and door of every area of the module for a conversation with a node that sets a later entry of an
// active quest, and point at the nearest one. In another area the marker points at the door / area
// transition that leads there (shortest route over the area transitions). Quests the journal marks as
// finished (JRL "End") are skipped. The tracked quest is the one updated last, or the one chosen with X
// on the hub Journal page.
//
// The objective itself comes first when the module's scripts reveal it. Compiled scripts (NCS) are read
// for AddJournalQuestEntry(quest, entry, ...) calls; for each call the string constants that decide it
// (the tests of the if-blocks around the call, and the strings since the previous journal call) are the
// tags it depends on - "if the acquired item is HEAD_M2Q2ALENT" in the module's OnAcquireItem script, or
// "has the item" in the condition of the conversation line that hands the quest in. Scripts come from the
// module's events, the scripts of every area / creature / placeable / door / trigger, and the actions and
// conditions of conversation lines. Objects with those tags (creatures alive, placeables, doors,
// triggers, and items - pointed at where they lie or at whoever carries them, unless you or your party
// carry them already) and objects whose own scripts write a later entry (a creature's death, a trigger's
// enter) are the targets; the nearest one is followed. Without any, the marker points at whoever's
// conversation writes the next entry, as before.
#include "questmark.h"
#include "game.h"
#include "res.h"
#include "log.h"
#include "text.h"
#include "pad.h"
#include "gamedata.h"
#include <algorithm>
#include <windows.h>
#include <cstring>
#include <cmath>
#include <map>
#include <set>
#include <deque>
#include <string>
#include <vector>

using namespace G;

namespace {
// ---- minimal GFF reader ----
struct Gff {
    std::vector<uint8_t> d;
    uint32_t so = 0, sc = 0, fo = 0, fc = 0, lo = 0, lc = 0, fdo = 0, fio = 0, lio = 0;
    bool ok = false;
    bool Load(std::vector<uint8_t>&& v) {
        d = std::move(v);
        if (d.size() < 56) return false;
        const uint32_t* h = (const uint32_t*)(d.data() + 8);
        so = h[0]; sc = h[1]; fo = h[2]; fc = h[3]; lo = h[4]; lc = h[5]; fdo = h[6]; fio = h[8]; lio = h[10];
        ok = so < d.size() && fo < d.size() && lo <= d.size();
        return ok;
    }
    uint32_t U(size_t off) const { return off + 4 <= d.size() ? *(const uint32_t*)(d.data() + off) : 0; }
    // fields of a struct: (label, type, data)
    struct Field { std::string label; uint32_t type, data; };
    std::vector<Field> Fields(uint32_t s) const {
        std::vector<Field> r;
        if (s >= sc) return r;
        size_t sp = so + (size_t)s * 12;
        uint32_t data = U(sp + 4), n = U(sp + 8);
        auto one = [&](uint32_t fi) {
            if (fi >= fc) return;
            size_t fp = fo + (size_t)fi * 12;
            uint32_t li = U(fp + 4);
            Field f;
            if (li < lc && lo + li * 16 + 16 <= d.size()) f.label.assign((const char*)d.data() + lo + li * 16, strnlen((const char*)d.data() + lo + li * 16, 16));
            f.type = U(fp); f.data = U(fp + 8);
            r.push_back(f);
        };
        if (n == 1) one(data);
        else for (uint32_t i = 0; i < n && i < 4096; i++) one(U(fio + data + i * 4));
        return r;
    }
    std::string Str(const Field& f) const {          // CExoString
        size_t p = fdo + f.data;
        uint32_t len = U(p);
        if (p + 4 + len > d.size() || len > 4096) return std::string();
        return std::string((const char*)d.data() + p + 4, len);
    }
    std::string Ref(const Field& f) const {          // CResRef
        size_t p = fdo + f.data;
        if (p >= d.size()) return std::string();
        uint8_t len = d[p];
        if (p + 1 + len > d.size() || len > 32) return std::string();
        return std::string((const char*)d.data() + p + 1, len);
    }
    std::string Loc(const Field& f) const {          // CExoLocString: the string for the game language, else the TLK entry
        size_t p = fdo + f.data;
        uint32_t strref = U(p + 4), n = U(p + 8);
        size_t q = p + 12;
        std::string first;
        for (uint32_t i = 0; i < n && i < 16; i++) {
            uint32_t len = U(q + 4);
            if (q + 8 + len > d.size()) break;
            std::string s((const char*)d.data() + q + 8, len);
            if (first.empty()) first = s;
            q += 8 + len;
        }
        if (strref != 0xffffffffu) { std::string t = Tlk(strref); if (!t.empty()) return t; }
        return G2UClean(first.c_str(), (int)first.size());
    }
    std::vector<uint32_t> List(const Field& f) const {
        std::vector<uint32_t> r;
        size_t p = lio + f.data;
        uint32_t n = U(p);
        for (uint32_t i = 0; i < n && i < 100000; i++) r.push_back(U(p + 4 + i * 4));
        return r;
    }
};
std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// ---- module data (cached) ----
// ---- compiled scripts: AddJournalQuestEntry calls and the tags they depend on ----
struct JCall { std::string quest; int entry = -1; std::vector<std::string> tags; };   // lower case
struct NcsInfo { bool exists = false; std::vector<JCall> calls; std::vector<std::string> strings; };
std::map<std::string, NcsInfo> g_ncs;
const uint16_t RT_AddJournalQuestEntry = 367;              // nwscript.nss routine number
const NcsInfo& Script(const std::string& resref) {
    auto it = g_ncs.find(resref);
    if (it != g_ncs.end()) return it->second;
    NcsInfo info;
    std::vector<uint8_t> d;
    if (!resref.empty() && Res_Load(resref.c_str(), 2010, d) && d.size() > 13 && memcmp(d.data(), "NCS V1.0", 8) == 0) {
        info.exists = true;
        struct Op { char k; int pos; int v; std::string s; };    // S string, I int, A action, Z conditional jump (v = target)
        std::vector<Op> ops;
        size_t i = 13;
        auto be16 = [&](size_t o) { return (int)((d[o] << 8) | d[o + 1]); };
        auto be32 = [&](size_t o) { return (int)(((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3]); };
        while (i + 2 <= d.size()) {
            int p = (int)i; uint8_t op = d[i], ty = d[i + 1]; i += 2;
            if (i > d.size()) break;
            if (op == 0x04) {                                   // CONST
                if (ty == 5) { if (i + 2 > d.size()) break; int n = be16(i); if (i + 2 + n > d.size()) break; ops.push_back({'S', p, 0, std::string((const char*)&d[i + 2], n)}); i += 2 + n; }
                else { if (i + 4 > d.size()) break; if (ty == 3) ops.push_back({'I', p, be32(i), {}}); i += 4; }
            } else if (op == 0x05) { if (i + 3 > d.size()) break; ops.push_back({'A', p, be16(i), {}}); i += 3; }
            else if (op == 0x1f || op == 0x25) { if (i + 4 > d.size()) break; ops.push_back({'Z', p, p + be32(i), {}}); i += 4; }
            else if (op == 0x01 || op == 0x03 || op == 0x26 || op == 0x27 || op == 0x21) i += 6;
            else if (op == 0x1b || op == 0x1d || op == 0x1e || op == 0x23 || op == 0x24 || op == 0x28 || op == 0x29) i += 4;
            else if (op == 0x2c) i += 8;
            else if ((op == 0x0b || op == 0x0c) && ty == 0x24) i += 2;
        }
        size_t last = 0;
        for (size_t k = 0; k < ops.size(); k++) {
            if (ops[k].k == 'S') info.strings.push_back(Lower(ops[k].s));
            if (ops[k].k != 'A' || ops[k].v != RT_AddJournalQuestEntry) continue;
            JCall c;
            // arguments are pushed last to first: the quest tag is the last string, the entry the int before it
            size_t q = k;
            while (q > 0) { q--; if (ops[q].k == 'S') { c.quest = Lower(ops[q].s); break; } }
            if (q > 0 && ops[q - 1].k == 'I' && ops[q - 1].pos + 6 == ops[q].pos) c.entry = ops[q - 1].v;   // a constant entry
            std::set<std::string> t;
            for (size_t x = last; x < k; x++) if (ops[x].k == 'S') t.insert(Lower(ops[x].s));
            for (size_t z = 0; z < k; z++)                      // the tests of the blocks around the call
                if (ops[z].k == 'Z' && ops[z].pos < ops[k].pos && ops[k].pos < ops[z].v)
                    for (size_t x = z >= 8 ? z - 8 : 0; x < z; x++) if (ops[x].k == 'S') t.insert(Lower(ops[x].s));
            for (auto& w : t) if (w != c.quest && w.compare(0, 16, "nw_journal_entry") != 0) c.tags.push_back(w);
            if (!c.quest.empty()) info.calls.push_back(c);
            last = k + 1;
        }
    }
    return g_ncs.emplace(resref, std::move(info)).first->second;
}

// a conversation: the journal entries its lines write (directly, or by their action script) and the tags
// the conditions leading to those lines test
struct DlgSet { std::string quest; uint32_t entry; std::vector<std::string> tags; };
struct DlgInfo { std::vector<DlgSet> sets; };
std::map<std::string, DlgInfo> g_dlg;
std::map<std::string, std::set<uint32_t>> g_end;       // quest tag -> entries that end it
std::map<std::string, std::string> g_title;            // quest tag -> title
bool g_jrlLoaded = false;
void* g_jrlModule = nullptr;
bool g_debugTargets = false;           // log the targets of every quest on the next scan

const DlgInfo& Dialog(const std::string& resref) {
    auto it = g_dlg.find(resref);
    if (it != g_dlg.end()) return it->second;
    DlgInfo info;
    std::vector<uint8_t> v;
    Gff g;
    if (Res_Load(resref.c_str(), 2029, v) && g.Load(std::move(v))) {
        // conditions of the links to each entry (0) / reply (1)
        std::map<uint32_t, std::set<std::string>> cond[2];
        std::vector<uint32_t> nodes[2];
        for (auto& top : g.Fields(0)) {
            if (top.type != 15) continue;
            int kind = top.label == "EntryList" ? 0 : top.label == "ReplyList" ? 1 : top.label == "StartingList" ? 2 : -1;
            if (kind < 0) continue;
            if (kind < 2) nodes[kind] = g.List(top);
            auto link = [&](uint32_t ls, int to) {
                uint32_t idx = 0; std::string act;
                for (auto& f : g.Fields(ls)) { if (f.label == "Index") idx = f.data; if (f.label == "Active" && f.type == 11) act = Lower(g.Ref(f)); }
                if (!act.empty()) cond[to][idx].insert(act);
            };
            if (kind == 2) { for (uint32_t ls : g.List(top)) link(ls, 0); continue; }
            for (uint32_t s : nodes[kind])
                for (auto& f : g.Fields(s))
                    if (f.type == 15 && (f.label == "RepliesList" || f.label == "EntriesList"))
                        for (uint32_t ls : g.List(f)) link(ls, f.label == "RepliesList" ? 1 : 0);
        }
        for (int kind = 0; kind < 2; kind++)
            for (uint32_t ni = 0; ni < nodes[kind].size(); ni++) {
                std::string q, act; uint32_t e = 0; bool hasE = false;
                for (auto& f : g.Fields(nodes[kind][ni])) {
                    if (f.label == "Quest" && f.type == 10) q = Lower(g.Str(f));
                    else if (f.label == "QuestEntry" && f.type == 4) { e = f.data; hasE = true; }
                    else if (f.label == "Script" && f.type == 11) act = Lower(g.Ref(f));
                }
                std::vector<std::string> ct;
                auto c = cond[kind].find(ni);
                if (c != cond[kind].end()) for (auto& cs : c->second) for (auto& w : Script(cs).strings) ct.push_back(w);
                if (!q.empty() && hasE) info.sets.push_back({q, e, ct});
                if (!act.empty())
                    for (auto& jc : Script(act).calls) {
                        DlgSet ds{jc.quest, (uint32_t)(jc.entry < 0 ? 0x7fffffff : jc.entry), ct};
                        ds.tags.insert(ds.tags.end(), jc.tags.begin(), jc.tags.end());
                        info.sets.push_back(ds);
                    }
            }
    }
    return g_dlg.emplace(resref, info).first->second;
}

void LoadJournalDefs() {
    g_end.clear(); g_title.clear();
    std::vector<uint8_t> v;
    Gff g;
    if (!Res_Load("module", 2056, v) || !g.Load(std::move(v))) { Log("quest: no module.jrl"); return; }
    for (auto& top : g.Fields(0)) {
        if (top.label != "Categories" || top.type != 15) continue;
        for (uint32_t c : g.List(top)) {
            std::string tag, title; std::set<uint32_t> ends;
            for (auto& f : g.Fields(c)) {
                if (f.label == "Tag" && f.type == 10) tag = Lower(g.Str(f));
                if (f.label == "Name" && f.type == 12) title = g.Loc(f);
                if (f.label == "EntryList" && f.type == 15)
                    for (uint32_t e : g.List(f)) {
                        uint32_t id = 0; bool end = false;
                        for (auto& ef : g.Fields(e)) {
                            if (ef.label == "ID") id = ef.data;
                            if (ef.label == "End") end = (ef.data & 0xffff) != 0;
                        }
                        if (end) ends.insert(id);
                    }
            }
            if (!tag.empty()) { g_end[tag] = ends; g_title[tag] = title; }
        }
    }
    Log("quest: module.jrl has %d quests", (int)g_end.size());
}

// ---- server (in-process in single player) ----
typedef void* (*GetSrvObj_t)(void* srv, uint32_t id);
GetSrvObj_t fSrvGameObject = nullptr, fSrvCreature = nullptr, fSrvArea = nullptr;
typedef void* (*Thiscall0_t)(void*);
const uintptr_t VA_SrvGetModule = 0x1405599a0, VA_GetJournal = 0x14038fb10, VA_LookupTarget = 0x1404fc930;
const uintptr_t VA_SrvAreaById = 0x1405595a0;
void* Srv() {
    static void** am = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?g_pAppManager@@3PEAVCAppManager@@EA");
    void* s = am && *am ? F<void*>(*am, 8) : nullptr;
    return s && F<void*>(s, 8) ? s : nullptr;
}
void* SrvObject(uint32_t id) {
    void* s = Srv();
    if (!s || !fSrvGameObject) return nullptr;
    void* g = fSrvGameObject(s, id & 0x7fffffffu);
    return g ? VCall<void* (*)(void*)>(g, 0x20)(g) : nullptr;
}
void* SrvArea(uint32_t id) { void* s = Srv(); return s ? ((GetSrvObj_t)A(VA_SrvAreaById))(s, id) : nullptr; }
Vec3 SrvPos(void* o) { return F<Vec3>(o, 0xa4); }
uint32_t SrvAreaId(void* o) { return F<uint32_t>(o, 0xa0); }

struct Target { uint32_t id; uint32_t area; Vec3 pos; bool isArea = false; };
struct Mark {
    std::string tag;             // quest (lower case)
    bool found = false;          // something to point at
    bool here = false;           // target in the player's area
    Vec3 point;                  // where the marker points (target or exit)
    uint32_t object = 0;         // client id of what is pointed at
    uint64_t when = 0;           // journal entry time (latest = followed by default)
    bool objective = false;      // the quest's objective, not the one who gives / takes it
};
struct State {
    std::string tag;             // followed quest (lower case)
    std::vector<Mark> marks;     // every active quest
    double lastScan = -100;
} S;
std::string g_trackTag;          // chosen on the journal / map page ("" = the latest updated quest)
bool g_trackNone = false;        // unfollowed: no marker until a quest moves on
uint64_t g_noneWhen = 0;         // newest journal entry when it was unfollowed

struct Active { std::string tag; uint32_t state; uint64_t when; };
std::vector<Active> ActiveQuests(void* srvPlayer) {
    std::vector<Active> r;
    void* j = ((Thiscall0_t)A(VA_GetJournal))(srvPlayer);
    if (!j) return r;
    char* base = F<char*>(j, 0);
    int n = F<int>(j, 8);
    for (int i = 0; base && i < n && i < 2000; i++) {
        char* e = base + (size_t)i * 0x50;
        if (!Readable(e, 0x50)) break;
        const char* t = F<const char*>(e, 0x28);
        uint32_t len = F<uint32_t>(e, 0x30);
        if (!t || len == 0 || len > 64) continue;
        Active a; a.tag = Lower(std::string(t, len)); a.state = F<uint32_t>(e, 0x38);
        a.when = ((uint64_t)F<uint32_t>(e, 0x20) << 32) | F<uint32_t>(e, 0x24);
        auto end = g_end.find(a.tag);
        if (end != g_end.end() && end->second.count(a.state)) continue;      // finished
        r.push_back(a);
    }
    return r;
}

// the conversation of an object (creature / placeable / door), "" if none
const DlgInfo* ObjDialog(void* o, uint8_t type) {
    const char* src = nullptr;
    if (type == 5) { void* st = F<void*>(o, 0xad0); if (st && Readable((char*)st + 0x68, 16)) src = (char*)st + 0x68; }
    else if (type == 9) src = (char*)o + 0x354;
    else if (type == 10) src = (char*)o + 0x400;
    if (!src) return nullptr;
    char ref[17] = {};
    memcpy(ref, src, 16);
    if (!ref[0]) return nullptr;
    const DlgInfo& d = Dialog(Lower(ref));
    return d.sets.empty() ? nullptr : &d;
}

// scripts of a server object that write journal entries: the event scripts are CExoStrings in a block of
// the object (module, area, creature, placeable, door, trigger); every string there naming a compiled
// script is checked. Cached per object.
std::vector<std::string> ObjScripts(void* o, int type) {
    static std::map<void*, std::pair<uint32_t, std::vector<std::string>>> cache;
    uint32_t id = type == 3 || type == 4 ? 0 : F<uint32_t>(o, 8);
    auto it = cache.find(o);
    if (it != cache.end() && it->second.first == id) return it->second.second;
    size_t from = 0, to = 0;
    switch (type) {
        case 3: from = 0x1b0; to = 0x2a0; break;      // module
        case 4: from = 0x1d0; to = 0x220; break;      // area
        case 5: from = 0x330; to = 0x420; break;      // creature
        case 7: from = 0x330; to = 0x3a0; break;      // trigger
        case 9: from = 0x3f0; to = 0x4f0; break;      // placeable
        case 10: from = 0x380; to = 0x480; break;     // door
    }
    std::vector<std::string> r;
    for (size_t off = from; off + 12 <= to; off += 8) {
        const char* p = F<const char*>(o, off); uint32_t n = F<uint32_t>(o, off + 8);
        if (!p || n == 0 || n > 16 || !Readable(p, n)) continue;
        std::string name(p, n);
        bool ok = true;
        for (char c : name) if ((unsigned char)c <= 32 || (unsigned char)c > 126) ok = false;
        if (!ok) continue;
        name = Lower(name);
        if (!Script(name).calls.empty() && std::find(r.begin(), r.end(), name) == r.end()) r.push_back(name);
    }
    cache[o] = {id, r};
    return r;
}
std::string SrvTag(void* g) {
    const char* p = F<const char*>(g, 0x20); uint32_t n = F<uint32_t>(g, 0x28);
    return p && n && n < 64 && Readable(p, n) ? Lower(std::string(p, n)) : std::string();
}

void Scan() {
    S.marks.clear();
    void* pl = Player();
    void* srv = Srv();
    if (!pl || !srv || !fSrvCreature) return;
    void* sp = fSrvCreature(srv, Id(pl) & 0x7fffffffu);
    if (!sp) return;
    void* mod = ((Thiscall0_t)A(VA_SrvGetModule))(srv);
    if (!mod) return;
    if (mod != g_jrlModule) { g_jrlModule = mod; g_dlg.clear(); g_ncs.clear(); LoadJournalDefs(); }
    auto act = ActiveQuests(sp);
    if (act.empty()) { S.tag.clear(); return; }
    std::map<std::string, uint32_t> state;
    for (auto& a : act) state[a.tag] = a.state;
    auto later = [&](const std::string& q, int64_t entry) { auto it = state.find(q); return it != state.end() && (entry < 0 || entry > (int64_t)it->second); };
    uint32_t me = Id(pl) & 0x7fffffffu;
    auto mine = [&](uint32_t id) {                           // the hero or one of his associates
        id &= 0x7fffffffu;
        if (id == me) return true;
        void* c = GetObj(id | 0x80000000u);
        return c && (GD::MasterId(c) & 0x7fffffffu) == me;
    };
    static auto fDead = (int (*)(void*))GetProcAddress(GetModuleHandleA(nullptr), "?GetDead@CNWSObject@@QEAAHXZ");
    // what each quest waits for: tags (from the scripts' tests) and objects whose own scripts move it on
    std::map<std::string, std::set<std::string>> goal;
    std::map<std::string, std::vector<Target>> objectives, givers;
    auto addCalls = [&](const std::vector<std::string>& scripts, const Target* self) {
        for (auto& sc : scripts)
            for (auto& c : Script(sc).calls) {
                if (!later(c.quest, c.entry)) continue;
                goal[c.quest].insert(c.tags.begin(), c.tags.end());
                if (self) objectives[c.quest].push_back(*self);
            }
    };
    addCalls(ObjScripts(mod, 3), nullptr);
    // one pass over every object of every area: tags, own scripts, conversations, transitions
    struct Edge { uint32_t from, to; Vec3 at; uint32_t obj; };
    std::map<std::string, std::vector<Target>> byTag;
    std::vector<Edge> edges;
    uint32_t* areas = F<uint32_t*>(mod, 0x50);
    int na = F<int>(mod, 0x58);
    for (int ai = 0; areas && ai < na && ai < 1000; ai++) {
        void* ar = SrvArea(areas[ai]);
        if (!ar) continue;
        Target at{areas[ai], areas[ai], Vec3{}, true};
        addCalls(ObjScripts(ar, 4), &at);
        uint32_t* ids = F<uint32_t*>(ar, 0x228);
        int n = F<int>(ar, 0x230);
        for (int i = 0; ids && i < n && i < 20000; i++) {
            void* o = SrvObject(ids[i]);
            if (!o) continue;
            uint8_t t = F<uint8_t>(o, 0xc);
            bool thing = t == 5 || t == 7 || t == 9 || t == 10;
            if (thing && !(t == 5 && (mine(ids[i]) || (fDead && fDead(o))))) {
                Target self{ids[i], areas[ai], SrvPos(o)};
                std::string tag = SrvTag(o);
                if (!tag.empty()) byTag[tag].push_back(self);
                addCalls(ObjScripts(o, t), &self);
                if (t != 7)
                    if (const DlgInfo* d = ObjDialog(o, t)) {
                        std::set<std::string> added;
                        for (auto& st : d->sets) {
                            if (!later(st.quest, st.entry == 0x7fffffff ? -1 : (int64_t)st.entry)) continue;
                            goal[st.quest].insert(st.tags.begin(), st.tags.end());
                            if (added.insert(st.quest).second) givers[st.quest].push_back(self);
                        }
                    }
            }
            if (t == 10 || t == 7) {
                void* dst = ((void* (*)(void*))A(VA_LookupTarget))((char*)o + 0x288);
                if (dst) {
                    uint32_t da = SrvAreaId(dst);
                    if (da != areas[ai] && da != 0x7f000000) edges.push_back({areas[ai], da, SrvPos(o), ids[i]});
                }
            }
        }
    }
    // the tags: objects in the areas, and items wherever they are (on the ground, in a container, carried)
    std::map<std::string, std::vector<std::string>> questsOf;            // tag -> quests waiting for it
    for (auto& g : goal) for (auto& t : g.second) questsOf[t].push_back(g.first);
    for (auto& q : questsOf) {
        auto bt = byTag.find(q.first);
        if (bt != byTag.end()) for (auto& qq : q.second) objectives[qq].insert(objectives[qq].end(), bt->second.begin(), bt->second.end());
    }
    static auto fObjArray = (void* (*)(void*))GetProcAddress(GetModuleHandleA(nullptr), "?GetObjectArray@CServerExoApp@@QEAAPEAVCGameObjectArray@@XZ");
    void* oa = fObjArray && !questsOf.empty() ? fObjArray(srv) : nullptr;
    void** buckets = oa ? F<void**>(oa, 0) : nullptr;
    uint32_t nb = oa ? F<uint32_t>(oa, 0x1c) : 0;
    for (uint32_t b = 0; buckets && b < nb && b < (1u << 20); b++)
        for (char* node = (char*)buckets[b]; node && Readable(node, 0x18); node = F<char*>(node, 0x10)) {
            void* g = F<void*>(node, 8);
            if (!g || !Readable(g, 0x30) || F<uint8_t>(g, 0xc) != 6) continue;
            auto qs = questsOf.find(SrvTag(g));
            if (qs == questsOf.end()) continue;
            // the outermost holder: a container item's holder, a creature, a placeable; or the ground
            uint32_t owner = 0x7f000000; void* cur = g;
            for (int depth = 0; depth < 4; depth++) {
                void* item = VCall<void* (*)(void*)>(cur, 0x70)(cur);
                uint32_t pid = item ? F<uint32_t>(item, 0x3c8) : 0x7f000000;
                if (pid == 0x7f000000 || pid == 0xffffffffu) break;
                void* pg = fSrvGameObject(srv, pid & 0x7fffffffu);
                if (!pg) break;
                owner = pid;
                if (F<uint8_t>(pg, 0xc) != 6) break;
                cur = pg;
            }
            Target tg;
            if (owner == 0x7f000000) {                          // lying somewhere
                void* o = VCall<void* (*)(void*)>(g, 0x20)(g);
                if (!o) continue;
                tg = {F<uint32_t>(node, 0), SrvAreaId(o), SrvPos(o)};
            } else {
                if (mine(owner)) continue;                      // already yours
                void* og = fSrvGameObject(srv, owner & 0x7fffffffu);
                void* o = og ? VCall<void* (*)(void*)>(og, 0x20)(og) : nullptr;
                uint8_t ot = o ? F<uint8_t>(o, 0xc) : 0;
                if (!o || (ot != 5 && ot != 9)) continue;      // stores, the dead's carried things ...
                if (ot == 5 && fDead && fDead(o)) continue;
                tg = {owner, SrvAreaId(o), SrvPos(o)};
            }
            if (tg.area == 0x7f000000 || !tg.area) continue;
            for (auto& q : qs->second) objectives[q].push_back(tg);
        }
    uint32_t myArea = SrvAreaId(sp);
    Vec3 mp = SrvPos(sp);
    auto dist = [&](Vec3 a, Vec3 b) { float dx = a.x - b.x, dy = a.y - b.y; return sqrtf(dx * dx + dy * dy); };
    // first exit of the shortest route (fewest transitions) from the player's area to every other area
    std::map<uint32_t, int> firstEdge, hops;
    {
        std::deque<uint32_t> q;
        std::set<uint32_t> seen{myArea};
        for (size_t i = 0; i < edges.size(); i++)
            if (edges[i].from == myArea && !seen.count(edges[i].to)) { seen.insert(edges[i].to); firstEdge[edges[i].to] = (int)i; hops[edges[i].to] = 1; q.push_back(edges[i].to); }
        while (!q.empty()) {
            uint32_t a = q.front(); q.pop_front();
            for (auto& e : edges)
                if (e.from == a && !seen.count(e.to)) { seen.insert(e.to); firstEdge[e.to] = firstEdge[a]; hops[e.to] = hops[a] + 1; q.push_back(e.to); }
        }
    }
    for (auto& a : act) {
        Mark m; m.tag = a.tag; m.when = a.when;
        // the objective (nearest) when known and reachable, else whoever's conversation moves the quest on
        auto pick = [&](const std::vector<Target>& tv) {
            const Target* best = nullptr; float bd = 1e9f;
            for (auto& t : tv) if (t.area == myArea && !t.isArea && dist(t.pos, mp) < bd) { bd = dist(t.pos, mp); best = &t; }
            if (best) { m.found = true; m.here = true; m.point = best->pos; m.object = best->id | 0x80000000u; return true; }
            // another area: fewest transitions away; among those, the exit nearest to you
            int bestHops = 1 << 30; const Edge* via = nullptr;
            for (auto& t : tv) {
                auto fe = firstEdge.find(t.area);
                if (fe == firstEdge.end()) continue;
                const Edge* e = &edges[fe->second];
                if (!via || hops[t.area] < bestHops || (hops[t.area] == bestHops && dist(e->at, mp) < dist(via->at, mp))) { bestHops = hops[t.area]; via = e; }
            }
            if (via) { m.found = true; m.here = false; m.point = via->at; m.object = via->obj | 0x80000000u; return true; }
            return false;
        };
        std::vector<Target> tv;
        for (auto& t : objectives[a.tag]) if (!(t.isArea && t.area == myArea)) tv.push_back(t);
        m.objective = pick(tv);
        if (!m.objective) { tv = givers[a.tag]; pick(tv); }
        if (g_debugTargets) {
            std::string d;
            for (auto& t : tv) {
                void* g = fSrvGameObject(srv, t.id & 0x7fffffffu);
                char b2[96]; snprintf(b2, sizeof b2, " %s@%08x", t.isArea ? "(area)" : g ? SrvTag(g).c_str() : "?", t.area);
                if (d.size() < 600) d += b2;
            }
            std::string gt; for (auto& x : goal[a.tag]) if (gt.size() < 300) gt += " " + x;
            Log("quest:   %s %s:%s | tags:%s", a.tag.c_str(), m.objective ? "objective" : "giver", d.c_str(), gt.c_str());
        }
        S.marks.push_back(m);
    }
    g_debugTargets = false;
    // followed quest: the chosen one, else the one updated last
    const Mark* tr = nullptr;
    for (auto& m : S.marks) if (!g_trackTag.empty() && m.tag == g_trackTag) tr = &m;
    if (!tr) for (auto& m : S.marks) if (!tr || m.when > tr->when) tr = &m;
    if (g_trackNone) {
        if (tr && tr->when > g_noneWhen) { g_trackNone = false; Log("quest: '%s' moved on, following the latest again", tr->tag.c_str()); }
        else tr = nullptr;
    }
    S.tag = tr ? tr->tag : std::string();
    {
        static std::string last;
        std::string sum;
        for (auto& m : S.marks) { char b[120]; snprintf(b, sizeof b, " %s:%s%s", m.tag.c_str(), m.found ? (m.here ? "here" : "exit") : "-", m.objective ? "*" : ""); sum += b; }
        if (sum != last) { last = sum; g_debugTargets = true; Log("quest: %d active, following '%s';%s", (int)S.marks.size(), S.tag.c_str(), sum.c_str()); }
    }
}

const Mark* Followed() {
    for (auto& m : S.marks) if (m.tag == S.tag) return &m;
    return nullptr;
}

void Update() {
    if (G::Loading()) { S.lastScan = g_pad.now; return; }
    if (g_pad.now - S.lastScan <= 2.0) return;
    S.lastScan = g_pad.now;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    Scan();
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    static int slow = 0;
    if (ms > 15 && slow++ < 5) Log("quest: scan took %.1f ms", ms);
}

Vec3 LivePoint(const Mark& m) {
    // the client object (doors / creatures move) when we can see it
    if (void* o = GetObj(m.object)) { Vec3 p = Pos(o); return Vec3{p.x, p.y, p.z}; }
    return m.point;
}
}  // namespace

void Quest_Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    fSrvGameObject = (GetSrvObj_t)GetProcAddress(exe, "?GetGameObject@CServerExoApp@@QEAAPEAVCGameObject@@I@Z");
    fSrvCreature = (GetSrvObj_t)GetProcAddress(exe, "?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z");
}

void Quest_Track(const std::string& tag) {
    g_trackTag = Lower(tag);
    g_trackNone = false;
    for (auto& m : S.marks) if (m.tag == g_trackTag) S.tag = m.tag;   // shown at once; the rescan confirms
    S.lastScan = -100;
    Log("quest: following '%s'", g_trackTag.empty() ? "(latest)" : g_trackTag.c_str());
}
void Quest_Untrack() {
    g_trackTag.clear();
    g_trackNone = true;
    g_noneWhen = 0;
    for (auto& m : S.marks) if (m.when > g_noneWhen) g_noneWhen = m.when;
    S.tag.clear();
    S.lastScan = -100;
    Log("quest: following nothing");
}
std::string Quest_Tracked() { Update(); return S.tag; }
bool Quest_FollowChosen() { return !g_trackTag.empty(); }
std::string Quest_Title(const std::string& tag) { auto it = g_title.find(Lower(tag)); return it != g_title.end() && !it->second.empty() ? it->second : tag; }
std::string Quest_Name() { return S.tag.empty() ? std::string() : Quest_Title(S.tag); }

bool Quest_Marker(Vec3& point, bool& here) {
    Update();
    const Mark* m = Followed();
    if (!m || !m->found) return false;
    point = LivePoint(*m);
    here = m->here;
    return true;
}

std::vector<QuestMark> Quest_All() {
    Update();
    std::vector<QuestMark> r;
    for (auto& m : S.marks) {
        QuestMark q;
        q.tag = m.tag; q.title = Quest_Title(m.tag); q.found = m.found; q.here = m.here;
        q.point = m.found ? LivePoint(m) : m.point;
        q.followed = m.tag == S.tag;
        r.push_back(q);
    }
    return r;
}
