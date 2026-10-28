// NWNPad - module resources for the quest marker (see questdata.h)
#include "questdata.h"
#include <cstring>
#include <atomic>
#include <deque>
#include <memory>

bool (*QD_Load)(const char*, uint16_t, std::vector<uint8_t>&) = nullptr;
std::string (*QD_Tlk)(uint32_t) = nullptr;
std::string (*QD_Text)(const char*, int) = nullptr;
bool (*QD_CanLoad)() = nullptr;
void (*QD_RunAsync)(void (*)(void*), void*) = nullptr;
int QD_loads = 0;
bool QD_pending = false;
std::map<std::string, QuestDef> QD_quests;

std::string QD_Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// ---- GFF ----
bool Gff::Load(std::vector<uint8_t>&& v) {
    d = std::move(v);
    if (d.size() < 56) return false;
    const uint32_t* h = (const uint32_t*)(d.data() + 8);
    so = h[0]; sc = h[1]; fo = h[2]; fc = h[3]; lo = h[4]; lc = h[5]; fdo = h[6]; fio = h[8]; lio = h[10];
    ok = so < d.size() && fo < d.size() && lo <= d.size();
    return ok;
}
uint32_t Gff::U(size_t off) const { uint32_t v = 0; if (off + 4 <= d.size()) memcpy(&v, d.data() + off, 4); return v; }
std::vector<Gff::Field> Gff::Fields(uint32_t s) const {
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
std::string Gff::Str(const Field& f) const {
    size_t p = fdo + f.data;
    uint32_t len = U(p);
    if (p + 4 + len > d.size() || len > 4096) return std::string();
    return std::string((const char*)d.data() + p + 4, len);
}
std::string Gff::Ref(const Field& f) const {
    size_t p = fdo + f.data;
    if (p >= d.size()) return std::string();
    uint8_t len = d[p];
    if (p + 1 + len > d.size() || len > 32) return std::string();
    return std::string((const char*)d.data() + p + 1, len);
}
std::string Gff::Loc(const Field& f) const {
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
    if (strref != 0xffffffffu && QD_Tlk) { std::string t = QD_Tlk(strref); if (!t.empty()) return t; }
    return QD_Text ? QD_Text(first.c_str(), (int)first.size()) : first;
}
std::vector<uint32_t> Gff::List(const Field& f) const {
    std::vector<uint32_t> r;
    size_t p = lio + f.data;
    uint32_t n = U(p);
    for (uint32_t i = 0; i < n && i < 100000; i++) r.push_back(U(p + 4 + i * 4));
    return r;
}
const Gff::Field* Gff::Find(const std::vector<Field>& fs, const char* label) const {
    for (auto& f : fs) if (f.label == label) return &f;
    return nullptr;
}

namespace {
std::map<std::string, NcsInfo> g_ncs;
std::map<std::string, DlgInfo> g_dlg;
std::map<std::string, Blueprint> g_bp;
// another resource may be read now (the first of a slice always may)
bool MayLoad() {
    if (QD_loads > 0 && QD_CanLoad && !QD_CanLoad()) { QD_pending = true; return false; }
    return true;
}
}  // namespace

// a script being worked out on the other thread
struct NcsJob { std::vector<uint8_t> d; NcsFlow::Info info; std::atomic<int> done{0}; };
std::map<std::string, std::shared_ptr<NcsJob>> g_jobs;
void RunJob(void* p) {
    auto* holder = (std::shared_ptr<NcsJob>*)p;
    NcsJob& j = **holder;
    j.info = NcsFlow::Analyze(j.d.data(), j.d.size());
    j.done.store(1);
    delete holder;
}

const NcsInfo& QD_Script(const std::string& resref) {
    static const NcsInfo none;
    auto it = g_ncs.find(resref);
    if (it != g_ncs.end()) return it->second;
    if (resref.empty()) return none;
    auto jb = g_jobs.find(resref);
    if (jb != g_jobs.end()) {
        if (!jb->second->done.load()) { QD_pending = true; return none; }
        NcsInfo info;
        static_cast<NcsFlow::Info&>(info) = std::move(jb->second->info);
        info.exists = info.ok;
        g_jobs.erase(jb);
        return g_ncs.emplace(resref, std::move(info)).first->second;
    }
    if (!MayLoad()) return none;
    NcsInfo info;
    std::vector<uint8_t> d;
    if (QD_Load && QD_Load(resref.c_str(), 2010, d)) {
        QD_loads++;
        if (QD_RunAsync) {
            auto j = std::make_shared<NcsJob>();
            j->d = std::move(d);
            g_jobs[resref] = j;
            QD_RunAsync(RunJob, new std::shared_ptr<NcsJob>(j));
            QD_pending = true;
            return none;
        }
        static_cast<NcsFlow::Info&>(info) = NcsFlow::Analyze(d.data(), d.size());
        info.exists = info.ok;
    }
    return g_ncs.emplace(resref, std::move(info)).first->second;
}

// a conversation: the journal entries its lines write (directly, or by their action scripts), the tests on
// the way to them (every condition on the links leading there from the start), what the actions create
struct NodeQ { std::string quest, act; uint32_t entry = 0; bool has = false; };
struct DlgRaw {                                   // a conversation read, its scripts not all worked out yet
    DlgInfo info;
    std::vector<NodeQ> nq;
};
std::map<std::string, DlgRaw> g_raw;

bool ParseDialog(const std::string& resref, DlgRaw& raw) {
    std::vector<uint8_t> v;
    Gff g;
    if (!QD_Load || !QD_Load(resref.c_str(), 2029, v) || !g.Load(std::move(v))) return false;
    DlgInfo& info = raw.info;
    std::vector<uint32_t> nodes[2], startLinks;
    for (auto& top : g.Fields(0)) {
        if (top.type != 15) continue;
        if (top.label == "EntryList") nodes[0] = g.List(top);
        else if (top.label == "ReplyList") nodes[1] = g.List(top);
        else if (top.label == "StartingList") startLinks = g.List(top);
    }
    info.entries = (int)nodes[0].size();
    int total = info.entries + (int)nodes[1].size();
    info.out.resize(total);
    std::map<std::string, int> condIdx;
    auto link = [&](uint32_t ls, int toKind) {
        DlgLink l{-1, -1};
        uint32_t idx = 0; std::string act;
        for (auto& f : g.Fields(ls)) { if (f.label == "Index") idx = f.data; if (f.label == "Active" && f.type == 11) act = QD_Lower(g.Ref(f)); }
        l.to = (toKind == 0 ? 0 : info.entries) + (int)idx;
        if (l.to >= total) l.to = -1;
        if (!act.empty()) {
            auto c = condIdx.find(act);
            if (c == condIdx.end()) { c = condIdx.emplace(act, (int)info.conds.size()).first; info.conds.push_back(act); }
            l.cond = c->second;
        }
        return l;
    };
    for (uint32_t ls : startLinks) { DlgLink l = link(ls, 0); if (l.to >= 0) info.start.push_back(l); }
    raw.nq.resize(total);
    for (int kind = 0; kind < 2; kind++)
        for (uint32_t ni = 0; ni < nodes[kind].size(); ni++) {
            int id = (kind ? info.entries : 0) + (int)ni;
            NodeQ& q = raw.nq[id];
            for (auto& f : g.Fields(nodes[kind][ni])) {
                if (f.label == "Quest" && f.type == 10) q.quest = QD_Lower(g.Str(f));
                else if (f.label == "QuestEntry" && f.type == 4) { q.entry = f.data; q.has = true; }
                else if (f.label == "Script" && f.type == 11) q.act = QD_Lower(g.Ref(f));
                else if (f.type == 15 && (f.label == "RepliesList" || f.label == "EntriesList"))
                    for (uint32_t ls : g.List(f)) { DlgLink l = link(ls, f.label == "RepliesList" ? 1 : 0); if (l.to >= 0) info.out[id].push_back(l); }
            }
        }
    return true;
}

// a conversation: the journal entries its lines write (directly, or by their action scripts), the tests on
// the way to them, what the actions create
const DlgInfo& QD_Dialog(const std::string& resref) {
    static const DlgInfo none;
    auto it = g_dlg.find(resref);
    if (it != g_dlg.end()) return it->second;
    auto rw = g_raw.find(resref);
    if (rw == g_raw.end()) {
        DlgRaw raw;
        if (!ParseDialog(resref, raw)) return g_dlg.emplace(resref, DlgInfo()).first->second;
        rw = g_raw.emplace(resref, std::move(raw)).first;
    }
    DlgRaw& raw = rw->second;
    DlgInfo& info = raw.info;
    int total = (int)raw.nq.size();
    bool was = QD_pending;
    QD_pending = false;
    // every script it needs, at once (worked out meanwhile): the actions, then the conditions if it writes
    for (auto& q : raw.nq) if (!q.act.empty()) QD_Script(q.act);
    if (QD_pending) { QD_pending = true; return none; }
    bool anyWrite = false;
    for (auto& q : raw.nq) if ((!q.quest.empty() && q.has) || (!q.act.empty() && !QD_Script(q.act).calls.empty())) anyWrite = true;
    if (anyWrite) for (auto& c : info.conds) QD_Script(c);
    if (QD_pending) return none;
    QD_pending = was;
    // the tests on the way to a node: what every way there from the start needs (a test on only one of
    // several ways there is not what the line waits for; two ways testing the same thing still count). The
    // NPC says the first of his lines whose condition passes: a line also means the ones before it failed.
    std::vector<std::set<std::string>> need(total);
    if (anyWrite) {
        struct In { int from; std::set<std::string> t; };
        std::vector<std::vector<In>> in(total);
        auto links = [&](int from, const std::vector<DlgLink>& ls, bool ordered) {
            std::set<std::string> before;                       // what the earlier lines failing implies
            for (auto& l : ls) {
                In x{from, before};
                const NcsInfo* ci = l.cond >= 0 ? &QD_Script(info.conds[l.cond]) : nullptr;
                if (ci) x.t.insert(ci->req.begin(), ci->req.end());
                in[l.to].push_back(std::move(x));
                if (ordered && ci && ci->pass.size() == 1 && ci->pass[0].size() == 1) {
                    const NcsFlow::Guard& g = ci->pass[0][0];
                    if (g.op == 'x' || g.op == 'o') before.insert(g.quest);   // "has not X" failed: has X ...
                }
            }
        };
        links(-1, info.start, true);
        for (int i = 0; i < total; i++) links(i, info.out[i], i >= info.entries);
        std::vector<char> set_(total, 0);
        for (int pass = 0, ch = 1; ch && pass < 50; pass++) {
            ch = 0;
            // the items a set of tests needs (one of a group counts as its members)
            auto items = [](const std::set<std::string>& c, std::set<std::string>& out) {
                for (auto& t : c) {
                    if (t.empty() || t[0] == '\x04' || t.compare(0, 16, "nw_journal_entry") == 0) continue;
                    if (t[0] == '\x06') { for (size_t a = 1, b; a <= t.size(); a = b + 1) { b = t.find('|', a); if (b == std::string::npos) b = t.size(); if (b > a) out.insert(t.substr(a, b - a)); } }
                    else out.insert(t);
                }
            };
            for (int x = 0; x < total; x++) {
                bool any = false, eachItem = true; std::set<std::string> acc, uni;
                for (auto& p : in[x]) {
                    if (p.from >= 0 && !set_[p.from]) continue;       // not reached yet: no constraint
                    std::set<std::string> c = p.from >= 0 ? need[p.from] : std::set<std::string>();
                    c.insert(p.t.begin(), p.t.end());
                    std::set<std::string> it; items(c, it);
                    if (it.empty()) eachItem = false;
                    uni.insert(it.begin(), it.end());
                    if (!any) { acc.swap(c); any = true; }
                    else { std::set<std::string> r; for (auto& v : acc) if (c.count(v)) r.insert(v); acc.swap(r); }
                }
                if (!any) continue;
                // every way there needs some item, not the same one: it needs one of them
                std::set<std::string> have; items(acc, have);
                if (eachItem && have.empty() && uni.size() > 1) {
                    std::string k = "\x06";
                    for (auto& y : uni) { if (k.size() > 1) k += '|'; k += y; }
                    acc.insert(k);
                }
                if (!set_[x] || acc != need[x]) { need[x].swap(acc); set_[x] = 1; ch = 1; }
            }
        }
    }
    std::set<std::string> crSeen;
    for (int id = 0; id < total; id++) {
        NodeQ& q = raw.nq[id];
        const NcsInfo* as = q.act.empty() ? nullptr : &QD_Script(q.act);
        if (as) {
            for (auto& c : as->creates) if (crSeen.insert(std::to_string(id) + ":" + std::to_string(c.routine) + c.tmpl).second) info.creates.push_back({id, c});
            for (auto& j : as->jumps) info.jumps.push_back({id, j});
            info.convs.insert(as->convs.begin(), as->convs.end());
            info.setsInt.insert(as->setsInt.begin(), as->setsInt.end());
        }
        bool sets = !q.quest.empty() && q.has;
        if (!sets && !(as && !as->calls.empty())) continue;
        std::vector<std::string> pt(need[id].begin(), need[id].end());
        if (sets) { info.writes.push_back({id, q.quest, q.entry, pt, {}}); info.quests.insert(q.quest); }
        if (as)
            for (auto& c : as->calls) {
                DlgWrite w{id, c.quest, (uint32_t)(c.entry < 0 ? 0x7fffffff : c.entry), pt, c.guards};
                w.tags.insert(w.tags.end(), c.tags.begin(), c.tags.end());
                w.status = c.override_;
                for (auto& g : c.guards) if (g.quest == c.quest) w.status = false;
                info.writes.push_back(std::move(w));
                info.quests.insert(c.quest);
            }
    }
    info.ok = true;
    DlgInfo done = std::move(info);
    g_raw.erase(rw);
    return g_dlg.emplace(resref, std::move(done)).first->second;
}

const Blueprint& QD_Blueprint(uint16_t routine, const std::string& resref) {
    static const Blueprint none;
    std::string key = (routine == 31 ? "i:" : "o:") + resref;
    auto it = g_bp.find(key);
    if (it != g_bp.end()) return it->second;
    if (!MayLoad()) return none;
    bool was = QD_pending;
    QD_pending = false;
    Blueprint b;
    // CreateObject: a creature, placeable, ... (the type is a variable in many scripts: try the kinds that talk)
    const uint16_t objTypes[] = {2027, 2044}, itemTypes[] = {2025};
    const uint16_t* types = routine == 31 ? itemTypes : objTypes;
    int nt = routine == 31 ? 1 : 2;
    for (int ti = 0; ti < nt; ti++) {
        std::vector<uint8_t> v; Gff g;
        if (!QD_Load || !QD_Load(resref.c_str(), types[ti], v) || !g.Load(std::move(v))) continue;
        QD_loads++;
        b.ok = true;
        for (auto& f : g.Fields(0)) {
            if (f.label == "Tag" && f.type == 10) b.tag = QD_Lower(g.Str(f));
            else if (f.label == "Conversation" && f.type == 11) {
                std::string c = QD_Lower(g.Ref(f));
                if (!c.empty()) { const DlgInfo& d = QD_Dialog(c); if (!d.writes.empty() || !d.creates.empty()) b.dlg = &d; }
            } else if (f.type == 11 && (f.label.compare(0, 6, "Script") == 0 || f.label.compare(0, 2, "On") == 0)) {
                std::string sc = QD_Lower(g.Ref(f));
                if (!sc.empty() && QD_Script(sc).Relevant()) b.scripts.push_back(sc);
            } else if (f.type == 15 && (f.label == "ItemList" || f.label == "Equip_ItemList")) {
                for (uint32_t s : g.List(f))
                    for (auto& ff : g.Fields(s))
                        if (ff.type == 11 && (ff.label == "InventoryRes" || ff.label == "EquippedRes")) {
                            std::string r = QD_Lower(g.Ref(ff));
                            if (!r.empty()) b.items.push_back(r);
                        }
            }
        }
        break;
    }
    if (QD_pending) return none;
    QD_pending = was;
    return g_bp.emplace(key, std::move(b)).first->second;
}

void QD_LoadJournal() {
    QD_quests.clear();
    std::vector<uint8_t> v;
    Gff g;
    if (!QD_Load || !QD_Load("module", 2056, v) || !g.Load(std::move(v))) return;
    for (auto& top : g.Fields(0)) {
        if (top.label != "Categories" || top.type != 15) continue;
        for (uint32_t c : g.List(top)) {
            std::string tag; QuestDef q;
            for (auto& f : g.Fields(c)) {
                if (f.label == "Tag" && f.type == 10) tag = QD_Lower(g.Str(f));
                if (f.label == "Name" && f.type == 12) q.title = g.Loc(f);
                if (f.label == "EntryList" && f.type == 15)
                    for (uint32_t e : g.List(f)) {
                        uint32_t id = 0; bool end = false;
                        for (auto& ef : g.Fields(e)) {
                            if (ef.label == "ID") id = ef.data;
                            if (ef.label == "End") end = (ef.data & 0xffff) != 0;
                        }
                        if (end) q.ends.insert(id);
                        if (id < q.first) q.first = id;
                    }
            }
            if (!tag.empty()) QD_quests[tag] = q;
        }
    }
}

void QD_Reset() { g_raw.clear(); g_jobs.clear(); g_ncs.clear(); g_dlg.clear(); g_bp.clear(); QD_quests.clear(); }
