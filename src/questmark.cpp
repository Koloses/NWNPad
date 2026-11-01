// NWNPad - quest marker on the minimap (single player).
//
// The game has no quest markers; the module's own data says what moves each quest on. With the server in
// this process we read the player's journal (tag + current entry) and go through every object of every
// area: questplan.cpp works out, from the objects' scripts and conversations (questdata.cpp, ncsflow.cpp),
// who and what leads to a later journal entry and to which one - the objective (a creature to kill, an item
// to fetch and whoever carries it, a place to visit), or whoever's conversation takes the quest on. The
// marker points at the candidate leading to the lowest entry, the nearest one; in another area at the door
// / transition leading there (shortest route over the area transitions and the teleports the module's
// scripts and conversations make). Quests the journal marks as finished (JRL "End") are skipped. The
// tracked quest is the one updated last, or the one chosen on the hub Journal page.
#include "questmark.h"
#include "questplan.h"
#include "game.h"
#include "res.h"
#include "log.h"
#include "text.h"
#include "pad.h"
#include "gamedata.h"
#include "talklog.h"
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
std::string Lower(std::string s) { return QD_Lower(std::move(s)); }
void* g_jrlModule = nullptr;
bool g_debugTargets = false;           // log the targets of every quest on the next scan

// ---- server (in-process in single player) ----
typedef void* (*GetSrvObj_t)(void* srv, uint32_t id);
GetSrvObj_t fSrvGameObject = nullptr, fSrvCreature = nullptr;
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
QP::P3 SrvPos(void* o) { Vec3 v = F<Vec3>(o, 0xa4); return QP::P3{v.x, v.y, v.z}; }
Vec3 V(const QP::P3& p) { return Vec3{p.x, p.y, p.z}; }
uint32_t SrvAreaId(void* o) { return F<uint32_t>(o, 0xa0); }

struct Mark {
    std::string tag;             // quest (lower case)
    bool found = false;          // something to point at
    bool here = false;           // target in the player's area
    Vec3 point;                  // where the marker points (target or exit)
    uint32_t object = 0;         // client id of what is pointed at
    uint64_t when = 0;           // journal entry time (latest = followed by default)
    bool objective = false;      // the quest's objective, not the one who gives / takes it
};
struct Offer { uint32_t id, area; Vec3 pos; bool maybe; std::vector<std::string> quests; };
struct State {
    std::string tag;             // followed quest (lower case)
    std::vector<Mark> marks;     // every active quest
    std::vector<Offer> offers;   // who can start a quest you don't have (every area)
    uint32_t area = 0;           // the player's (server) area
    double lastScan = -100;
} S;
std::string g_trackTag;          // chosen on the journal / map page ("" = the latest updated quest)
bool g_trackNone = false;        // unfollowed: no marker until a quest moves on
uint64_t g_noneWhen = 0;         // newest journal entry when it was unfollowed

struct Active { std::string tag; uint32_t state; uint64_t when; };
std::vector<Active> ActiveQuests(void* srvPlayer, std::set<std::string>& known) {
    std::vector<Active> r;
    known.clear();
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
        known.insert(a.tag);
        auto def = QD_quests.find(a.tag);
        if (def != QD_quests.end() && def->second.ends.count(a.state)) continue;      // finished
        r.push_back(a);
    }
    return r;
}

// the conversation of an object (creature / placeable / door), "" if none
std::string ObjDialog(void* o, uint8_t type) {
    const char* src = nullptr;
    if (type == 5) { void* st = F<void*>(o, 0xad0); if (st && Readable((char*)st + 0x68, 16)) src = (char*)st + 0x68; }
    else if (type == 9) src = (char*)o + 0x354;
    else if (type == 10) src = (char*)o + 0x400;
    if (!src) return std::string();
    char ref[17] = {};
    memcpy(ref, src, 16);
    return Lower(ref);
}

// the event scripts of a server object: CExoStrings in a block of the object (module, area, creature,
// placeable, door, trigger); every string there that looks like a resref. Cached per object.
std::map<void*, std::pair<uint32_t, std::vector<std::string>>> g_objScripts;
const std::vector<std::string>& ObjScripts(void* o, int type) {
    uint32_t id = type == 3 || type == 4 ? 0 : F<uint32_t>(o, 8);
    auto it = g_objScripts.find(o);
    if (it != g_objScripts.end() && it->second.first == id) return it->second.second;
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
        if (std::find(r.begin(), r.end(), name) == r.end()) r.push_back(name);
    }
    auto& slot = g_objScripts[o];
    slot = {id, std::move(r)};
    return slot.second;
}
// a creature's name as the game shows it (CNWSCreatureStats::GetFullName)
std::string SrvName(uint32_t id) {
    static auto fFull = (void* (*)(void*, void*))GetProcAddress(GetModuleHandleA(nullptr), "?GetFullName@CNWSCreatureStats@@QEAA?AVCExoString@@XZ");
    static auto fFree = (void (*)(void*))GetProcAddress(GetModuleHandleA(nullptr), "??1CExoString@@QEAA@XZ");
    void* o = SrvObject(id);
    if (!o || F<uint8_t>(o, 0xc) != 5 || !fFull) return std::string();
    void* st = F<void*>(o, 0xad0);
    if (!st) return std::string();
    struct { char* p; uint32_t len, cap; } s{};
    fFull(st, &s);
    std::string r = s.p ? G2UClean(s.p, (int)s.len) : std::string();
    if (fFree) fFree(&s);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}
std::string SrvTag(void* g) {
    const char* p = F<const char*>(g, 0x20); uint32_t n = F<uint32_t>(g, 0x28);
    return p && n && n < 64 && Readable(p, n) ? Lower(std::string(p, n)) : std::string();
}

// The scan runs in slices of about a millisecond per frame (a whole module is thousands of objects, and
// every script and conversation is read once), then the result replaces the previous one at once; a new
// round starts 2 s after the last one ended.
struct Job {
    bool running = false;
    void* mod = nullptr;                                     // the module it scans (another one: start over)
    int phase = 0, ai = 0, i = 0;
    uint32_t b = 0, me = 0;
    std::vector<Active> act;
    QP::Round r;
    std::vector<QP::Edge> edges;                             // area transitions
    void* sp = nullptr;                                      // the player's server creature
    std::vector<std::pair<uint32_t, uint32_t>> later;        // (area, object) waiting for scripts being worked out
    size_t li = 0;
} J;

// local ints the module's conversations and scripts test: the module's own, or the player's
bool ReadLocalInt(const std::string& key, int& v) {
    static auto fGetInt = (int (*)(void*, void*))GetProcAddress(GetModuleHandleA(nullptr), "?GetInt@CNWSScriptVarTable@@QEAAHAEAVCExoString@@@Z");
    if (!fGetInt || key.size() < 3 || key[1] != ':') return false;
    void* tbl = nullptr;
    if (key[0] == 'm' && J.mod) tbl = (char*)J.mod + 0x170;           // CNWSModule::m_ScriptVars
    else if (key[0] == 'p' && J.sp) tbl = (char*)J.sp + 0x108;        // CNWSObject::m_ScriptVars
    if (!tbl) return false;
    std::string name = key.substr(2);
    struct { const char* p; uint32_t len, cap; } s{name.c_str(), (uint32_t)name.size(), (uint32_t)name.size() + 1};
    v = fGetInt(tbl, &s);
    return true;
}

// one worker thread works the scripts out (a big one can take milliseconds)
struct Work { void (*fn)(void*); void* arg; };
CRITICAL_SECTION g_wq;
HANDLE g_wev = nullptr;
std::deque<Work> g_work;
DWORD WINAPI Worker(void*) {
    for (;;) {
        WaitForSingleObject(g_wev, INFINITE);
        for (;;) {
            Work w{nullptr, nullptr};
            EnterCriticalSection(&g_wq);
            if (!g_work.empty()) { w = g_work.front(); g_work.pop_front(); }
            LeaveCriticalSection(&g_wq);
            if (!w.fn) break;
            w.fn(w.arg);
        }
    }
    return 0;
}
void RunAsync(void (*fn)(void*), void* arg) {
    EnterCriticalSection(&g_wq);
    g_work.push_back({fn, arg});
    LeaveCriticalSection(&g_wq);
    SetEvent(g_wev);
}

LARGE_INTEGER g_sliceStart, g_freq;
double g_budget = 1.0;
double SliceMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (t.QuadPart - g_sliceStart.QuadPart) * 1000.0 / g_freq.QuadPart; }
bool CanLoad() { return SliceMs() < g_budget; }

bool Mine(uint32_t id) {                                   // the hero or one of his associates
    id &= 0x7fffffffu;
    if (id == J.me) return true;
    void* c = GetObj(id | 0x80000000u);
    return c && (GD::MasterId(c) & 0x7fffffffu) == J.me;
}
int (*fDead)(void*) = nullptr;

// phase 0: the journal, the module's own scripts
bool Begin(void* sp, void* mod) {
    J = Job();
    J.running = true;
    J.mod = mod;
    J.sp = sp;
    QP::LocalInt = ReadLocalInt;
    if (mod != g_jrlModule) { g_jrlModule = mod; QD_Reset(); g_objScripts.clear(); QD_LoadJournal(); Log("quest: module.jrl has %d quests", (int)QD_quests.size()); }
    J.act = ActiveQuests(sp, J.r.known);
    for (auto& a : J.act) J.r.state[a.tag] = a.state;
    J.me = Id(Player()) & 0x7fffffffu;
    return true;
}

// phase 1: one object of an area (false: something not read yet, again next slice)
bool Object(uint32_t areaId, uint32_t id) {
    void* o = SrvObject(id);
    if (!o) return true;
    uint8_t t = F<uint8_t>(o, 0xc);
    if (t == 12) {                                         // waypoints: where teleports land
        QP::Obj w; w.id = id; w.area = areaId; w.pos = SrvPos(o); w.type = 12; w.tag = SrvTag(o);
        return J.r.Object(w);
    }
    bool thing = t == 5 || t == 7 || t == 9 || t == 10;
    bool mine = t == 5 && Mine(id);
    bool skip = t == 5 && fDead && fDead(o);
    if (mine && id != J.me && !skip) {                     // a henchman: only his conversation (his tale)
        QP::Obj ob; ob.id = id; ob.area = areaId; ob.pos = SrvPos(o); ob.type = t; ob.tag = SrvTag(o); ob.party = true;
        ob.dialog = ObjDialog(o, t);
        if (!J.r.Object(ob)) return false;
    } else if (thing && !skip && !mine) {
        QP::Obj ob; ob.id = id; ob.area = areaId; ob.pos = SrvPos(o); ob.type = t; ob.tag = SrvTag(o);
        ob.scripts = ObjScripts(o, t);
        if (t != 7) ob.dialog = ObjDialog(o, t);
        if (!J.r.Object(ob)) return false;
    }
    if (t == 10 || t == 7) {
        void* dst = ((void* (*)(void*))A(VA_LookupTarget))((char*)o + 0x288);
        if (dst) {
            uint32_t da = SrvAreaId(dst);
            if (da != areaId && da != 0x7f000000) J.edges.push_back({areaId, da, SrvPos(o), id});
        }
    }
    return true;
}

// phase 3: one bucket of the server's object table: items with a tag a quest waits for, where they are
void Bucket(void* srv, void* node0) {
    for (char* node = (char*)node0; node; node = F<char*>(node, 0x10)) {
        void* g = F<void*>(node, 8);
        if (!g || F<uint8_t>(g, 0xc) != 6) continue;
        std::string tag = SrvTag(g);
        if (tag.empty()) continue;
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
        QP::Target tg;
        if (owner == 0x7f000000) {                          // lying somewhere
            void* o = VCall<void* (*)(void*)>(g, 0x20)(g);
            if (!o) continue;
            tg.id = F<uint32_t>(node, 0); tg.area = SrvAreaId(o); tg.pos = SrvPos(o);
        } else {
            if (Mine(owner)) { J.r.Item(tag, tg, true); continue; }   // already yours
            void* og = fSrvGameObject(srv, owner & 0x7fffffffu);
            void* o = og ? VCall<void* (*)(void*)>(og, 0x20)(og) : nullptr;
            uint8_t ot = o ? F<uint8_t>(o, 0xc) : 0;
            if (!o || (ot != 5 && ot != 9)) continue;      // stores, the dead's carried things ...
            if (ot == 5 && fDead && fDead(o)) continue;
            tg.id = owner; tg.area = SrvAreaId(o); tg.pos = SrvPos(o);
        }
        if (tg.area == 0x7f000000 || !tg.area) continue;
        J.r.Item(tag, tg);
    }
}

// phase 4: routes, the marker of every active quest, the followed one
void Finish(void* srv, void* sp) {
    std::vector<Mark> marks;
    uint32_t myArea = SrvAreaId(sp);
    QP::P3 mp = SrvPos(sp);
    std::vector<QP::Edge> edges = J.edges;
    edges.insert(edges.end(), J.r.edges.begin(), J.r.edges.end());
    auto dist = [&](const QP::P3& a, const QP::P3& b) { float dx = a.x - b.x, dy = a.y - b.y; return sqrtf(dx * dx + dy * dy); };
    // first exit of the shortest route (fewest transitions) from the player's area to every other area
    std::map<uint32_t, int> firstEdge, hops;
    {
        std::map<uint32_t, std::vector<int>> out;
        for (size_t i = 0; i < edges.size(); i++) out[edges[i].from].push_back((int)i);
        std::deque<uint32_t> q;
        std::set<uint32_t> seen{myArea};
        for (int i : out[myArea])
            if (!seen.count(edges[i].to)) { seen.insert(edges[i].to); firstEdge[edges[i].to] = i; hops[edges[i].to] = 1; q.push_back(edges[i].to); }
        while (!q.empty()) {
            uint32_t a = q.front(); q.pop_front();
            for (int i : out[a])
                if (!seen.count(edges[i].to)) { seen.insert(edges[i].to); firstEdge[edges[i].to] = firstEdge[a]; hops[edges[i].to] = hops[a] + 1; q.push_back(edges[i].to); }
        }
    }
    for (auto& a : J.act) {
        Mark m; m.tag = a.tag; m.when = a.when;
        // the candidates you can get to: objectives, and whoever's conversation moves it on - leaving out
        // those you talked to when it moved on before (a druid already rescued still has the line, behind a
        // condition), unless nobody else is left
        std::vector<const QP::Target*> cand;
        auto reach = [&](const QP::Target& t) { return t.isArea ? t.area != myArea && firstEdge.count(t.area) : (t.area == myArea || firstEdge.count(t.area)); };
        for (auto& t : J.r.objectives[a.tag]) if (reach(t)) cand.push_back(&t);
        std::set<uint32_t> done = TalkLog_MovedBy(Quest_Title(a.tag));
        std::set<std::string> doneNames = TalkLog_MovedByName(Quest_Title(a.tag));   // older logs: by name
        std::vector<const QP::Target*> gv;
        for (auto& t : J.r.givers[a.tag]) {
            if (t.party || !reach(t)) continue;
            if (done.count(t.id & 0x7fffffffu)) continue;
            if (!doneNames.empty() && doneNames.count(Lower(SrvName(t.id)))) continue;
            gv.push_back(&t);
        }
        if (gv.empty()) for (auto& t : J.r.givers[a.tag]) if (!t.party && reach(t)) gv.push_back(&t);
        cand.insert(cand.end(), gv.begin(), gv.end());
        for (auto& t : J.r.givers[a.tag]) if (t.party) cand.push_back(&t);
        std::vector<const QP::Target*> best = J.r.Best(a.tag, cand);
        // the nearest: in this area, else fewest transitions away and the exit nearest to you
        const QP::Target* pick = nullptr; float bd = 1e9f;
        for (auto* t : best) if (!t->isArea && t->area == myArea && dist(t->pos, mp) < bd) { bd = dist(t->pos, mp); pick = t; }
        if (pick) { m.found = m.here = true; m.point = V(pick->pos); m.object = pick->id | 0x80000000u; }
        else {
            int bestHops = 1 << 30; const QP::Edge* via = nullptr;
            for (auto* t : best) {
                auto fe = firstEdge.find(t->area);
                if (fe == firstEdge.end()) continue;
                const QP::Edge* e = &edges[fe->second];
                if (!via || hops[t->area] < bestHops || (hops[t->area] == bestHops && dist(e->at, mp) < dist(via->at, mp))) { bestHops = hops[t->area]; via = e; pick = t; }
            }
            if (via) { m.found = true; m.here = false; m.point = V(via->at); m.object = via->obj | 0x80000000u; }
        }
        m.objective = pick && !pick->giver;
        if (g_debugTargets) {
            std::string d;
            for (auto* t : best) {
                void* g = fSrvGameObject(srv, t->id & 0x7fffffffu);
                char b2[160]; snprintf(b2, sizeof b2, " %s@%08x<%u %s %s>", t->isArea ? "(area)" : g ? SrvTag(g).c_str() : "?", t->area, t->entry, t->why, t->giver ? "" : t->via.c_str());
                if (d.size() < 800) d += b2;
            }
            std::string gt; for (auto& x : J.r.goal[a.tag]) if (gt.size() < 300) gt += " " + x.first;
            Log("quest:   %s@%u %s:%s | tags:%s", a.tag.c_str(), a.state, pick ? (pick->giver ? "giver" : "objective") : "-", d.c_str(), gt.c_str());
        }
        marks.push_back(m);
    }
    g_debugTargets = false;
    S.marks.swap(marks);
    S.offers.clear();
    for (auto& o : J.r.offers) S.offers.push_back({o.id | 0x80000000u, o.area, V(o.pos), o.maybe, o.quests});
    S.area = myArea;
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
        sum += " offers:";
        for (auto& o : S.offers) {
            void* g = fSrvGameObject(srv, o.id & 0x7fffffffu);
            char b[160]; snprintf(b, sizeof b, " %s%s(%s)", g ? SrvTag(g).c_str() : "?", o.maybe ? "?" : "", o.quests[0].c_str());
            if (sum.size() < 3000) sum += b;
        }
        if (sum != last) { last = sum; g_debugTargets = true; Log("quest: %d active, following '%s';%s", (int)S.marks.size(), S.tag.c_str(), sum.c_str()); }
    }
}

// one slice of the scan, at most ~budget ms
void Step(double budgetMs) {
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_sliceStart);
    g_budget = budgetMs;
    QD_loads = 0;
    QD_CanLoad = CanLoad;
    void* pl = Player();
    void* srv = Srv();
    void* sp = pl && srv && fSrvCreature ? fSrvCreature(srv, Id(pl) & 0x7fffffffu) : nullptr;
    void* mod = sp ? ((Thiscall0_t)A(VA_SrvGetModule))(srv) : nullptr;
    if (!mod) { J.running = false; S.marks.clear(); S.offers.clear(); return; }
    if (!fDead) fDead = (int (*)(void*))GetProcAddress(GetModuleHandleA(nullptr), "?GetDead@CNWSObject@@QEAAHXZ");
    if (J.running && J.mod != mod) J.running = false;
    if (!J.running) { Begin(sp, mod); J.phase = 0; }
    int k = 0;
    while (SliceMs() < budgetMs || J.phase == 4) {
        QD_pending = false;
        if (J.phase == 0) {                                   // the module's scripts
            if (!J.r.Module(ObjScripts(mod, 3))) return;
            J.phase = 1;
        } else if (J.phase == 1) {                            // every object of every area
            uint32_t* areas = F<uint32_t*>(mod, 0x50);
            int na = F<int>(mod, 0x58);
            if (!areas || J.ai >= na || J.ai >= 1000) { J.phase = 6; continue; }
            void* ar = SrvArea(areas[J.ai]);
            if (!ar) { J.ai++; J.i = 0; continue; }
            if (J.i == 0) { if (!J.r.Area(areas[J.ai], ObjScripts(ar, 4))) return; J.i = 1; }
            uint32_t* ids = F<uint32_t*>(ar, 0x228);
            int n = F<int>(ar, 0x230);
            for (; ids && J.i - 1 < n && J.i < 20001; ) {
                QD_pending = false;
                // a script / conversation still being worked out: that object again at the end
                if (!Object(areas[J.ai], ids[J.i - 1])) J.later.push_back({areas[J.ai], ids[J.i - 1]});
                J.i++;
                if (SliceMs() >= budgetMs) return;
            }
            J.ai++; J.i = 0;
        } else if (J.phase == 6) {                            // the objects that waited
            if (J.li >= J.later.size()) {
                if (J.later.empty()) { J.phase = 5; continue; }
                J.later.erase(std::remove_if(J.later.begin(), J.later.end(), [](const std::pair<uint32_t, uint32_t>& x) { return x.first == 0; }), J.later.end());
                J.li = 0;
                if (J.later.empty()) { J.phase = 5; continue; }
                return;                                       // the rest next slice (the worker is at it)
            }
            QD_pending = false;
            auto& x = J.later[J.li++];
            if (Object(x.first, x.second)) x.first = 0;       // done
            if (SliceMs() >= budgetMs) return;
        } else if (J.phase == 5) {                            // what scripts create
            bool done = false;
            J.r.Spawned(done);
            if (done) J.phase = 2;
        } else if (J.phase == 2) {                            // tags -> objects
            J.r.Match();
            J.phase = 3;
        } else if (J.phase == 3) {                            // items
            static auto fObjArray = (void* (*)(void*))GetProcAddress(GetModuleHandleA(nullptr), "?GetObjectArray@CServerExoApp@@QEAAPEAVCGameObjectArray@@XZ");
            void* oa = fObjArray ? fObjArray(srv) : nullptr;
            void** buckets = oa ? F<void**>(oa, 0) : nullptr;
            uint32_t nb = oa ? F<uint32_t>(oa, 0x1c) : 0;
            if (!buckets || J.b >= nb || J.b >= (1u << 20)) { J.phase = 4; continue; }
            Bucket(srv, buckets[J.b++]);
            if ((++k & 15) == 0 && SliceMs() >= budgetMs) return;
        } else {                                              // done
            Finish(srv, sp);
            J.running = false;
            S.lastScan = g_pad.now;
            return;
        }
    }
}

const Mark* Followed() {
    for (auto& m : S.marks) if (m.tag == S.tag) return &m;
    return nullptr;
}

void Update() {
    static double lastStep = -1;
    if (g_pad.now == lastStep) return;                       // once per frame
    lastStep = g_pad.now;
    if (G::Loading()) { J.running = false; S.lastScan = g_pad.now; return; }
    if (!J.running && g_pad.now - S.lastScan <= 2.0) return;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    Step(1.0);
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    static int slow = 0;
    if (ms > 15 && slow++ < 10) Log("quest: scan slice took %.1f ms (phase %d)", ms, J.phase);
}

Vec3 LivePoint(const Mark& m) {
    // the client object (doors / creatures move) when we can see it
    if (void* o = GetObj(m.object)) { Vec3 p = Pos(o); return Vec3{p.x, p.y, p.z}; }
    return m.point;
}
}  // namespace

void Quest_Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    QD_Load = [](const char* r, uint16_t t, std::vector<uint8_t>& out) { return Res_Load(r, t, out); };
    QD_Tlk = Tlk;
    QD_Text = [](const char* s, int n) { return G2UClean(s, n); };
    InitializeCriticalSection(&g_wq);
    g_wev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); QD_RunAsync = RunAsync; }
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
std::string Quest_Title(const std::string& tag) { auto it = QD_quests.find(Lower(tag)); return it != QD_quests.end() && !it->second.title.empty() ? it->second.title : tag; }
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

std::vector<QuestOffer> Quest_Offers() {
    Update();
    std::vector<QuestOffer> r;
    for (auto& o : S.offers) {
        if (o.area != S.area) continue;
        QuestOffer q;
        q.object = o.id; q.maybe = o.maybe;
        void* c = GetObj(o.id);
        if (c && GD::IsDead(c)) continue;
        q.pos = c ? Pos(c) : o.pos;
        for (auto& t : o.quests) q.titles.push_back(Quest_Title(t));
        r.push_back(q);
    }
    return r;
}

