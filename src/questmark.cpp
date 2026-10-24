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
#include "questmark.h"
#include "game.h"
#include "res.h"
#include "log.h"
#include "text.h"
#include "pad.h"
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
struct DlgInfo { std::vector<std::pair<std::string, uint32_t>> sets; };   // quest tag (lower) -> entry set
std::map<std::string, DlgInfo> g_dlg;
std::map<std::string, std::set<uint32_t>> g_end;       // quest tag -> entries that end it
std::map<std::string, std::string> g_title;            // quest tag -> title
bool g_jrlLoaded = false;
void* g_jrlModule = nullptr;

const DlgInfo& Dialog(const std::string& resref) {
    auto it = g_dlg.find(resref);
    if (it != g_dlg.end()) return it->second;
    DlgInfo info;
    std::vector<uint8_t> v;
    Gff g;
    if (Res_Load(resref.c_str(), 2029, v) && g.Load(std::move(v))) {
        for (auto& top : g.Fields(0)) {
            if (top.type != 15 || (top.label != "EntryList" && top.label != "ReplyList")) continue;
            for (uint32_t s : g.List(top)) {
                std::string q; uint32_t e = 0; bool hasE = false;
                for (auto& f : g.Fields(s)) {
                    if (f.label == "Quest" && f.type == 10) q = Lower(g.Str(f));
                    else if (f.label == "QuestEntry" && f.type == 4) { e = f.data; hasE = true; }
                }
                if (!q.empty() && hasE) info.sets.push_back({q, e});
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

struct Target { uint32_t id; uint32_t area; Vec3 pos; };
struct Mark {
    std::string tag;             // quest (lower case)
    bool found = false;          // something to point at
    bool here = false;           // target in the player's area
    Vec3 point;                  // where the marker points (target or exit)
    uint32_t object = 0;         // client id of what is pointed at
    uint64_t when = 0;           // journal entry time (latest = followed by default)
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

void Scan() {
    S.marks.clear();
    void* pl = Player();
    void* srv = Srv();
    if (!pl || !srv || !fSrvCreature) return;
    void* sp = fSrvCreature(srv, Id(pl) & 0x7fffffffu);
    if (!sp) return;
    void* mod = ((Thiscall0_t)A(VA_SrvGetModule))(srv);
    if (!mod) return;
    if (mod != g_jrlModule) { g_jrlModule = mod; g_dlg.clear(); LoadJournalDefs(); }
    auto act = ActiveQuests(sp);
    if (act.empty()) { S.tag.clear(); return; }
    std::map<std::string, uint32_t> state;
    for (auto& a : act) state[a.tag] = a.state;
    // one pass over every object of every area: whose conversation moves which quest on; transitions
    struct Edge { uint32_t from, to; Vec3 at; uint32_t obj; };
    std::map<std::string, std::vector<Target>> targets;
    std::vector<Edge> edges;
    uint32_t* areas = F<uint32_t*>(mod, 0x50);
    int na = F<int>(mod, 0x58);
    uint32_t me = Id(pl) & 0x7fffffffu;
    for (int ai = 0; areas && ai < na && ai < 1000; ai++) {
        void* ar = SrvArea(areas[ai]);
        if (!ar) continue;
        uint32_t* ids = F<uint32_t*>(ar, 0x228);
        int n = F<int>(ar, 0x230);
        for (int i = 0; ids && i < n && i < 20000; i++) {
            void* o = SrvObject(ids[i]);
            if (!o) continue;
            uint8_t t = F<uint8_t>(o, 0xc);
            if ((t == 5 || t == 9 || t == 10) && (ids[i] & 0x7fffffffu) != me) {
                if (const DlgInfo* d = ObjDialog(o, t)) {
                    std::set<std::string> added;
                    for (auto& st : d->sets) {
                        auto q = state.find(st.first);
                        if (q != state.end() && st.second > q->second && added.insert(st.first).second)
                            targets[st.first].push_back({ids[i], areas[ai], SrvPos(o)});
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
        auto& tv = targets[a.tag];
        const Target* best = nullptr; float bd = 1e9f;
        for (auto& t : tv) if (t.area == myArea && dist(t.pos, mp) < bd) { bd = dist(t.pos, mp); best = &t; }
        if (best) { m.found = true; m.here = true; m.point = best->pos; m.object = best->id | 0x80000000u; }
        else {
            // the nearest area (by transitions) that has a target
            int bestHops = 1 << 30; const Edge* via = nullptr;
            for (auto& t : tv) {
                auto fe = firstEdge.find(t.area);
                if (fe == firstEdge.end()) continue;
                if (!via || hops[t.area] < bestHops) { bestHops = hops[t.area]; via = &edges[fe->second]; }
            }
            if (via) { m.found = true; m.here = false; m.point = via->at; m.object = via->obj | 0x80000000u; }
        }
        S.marks.push_back(m);
    }
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
        for (auto& m : S.marks) { char b[120]; snprintf(b, sizeof b, " %s:%s", m.tag.c_str(), m.found ? (m.here ? "here" : "exit") : "-"); sum += b; }
        if (sum != last) { last = sum; Log("quest: %d active, following '%s';%s", (int)S.marks.size(), S.tag.c_str(), sum.c_str()); }
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
