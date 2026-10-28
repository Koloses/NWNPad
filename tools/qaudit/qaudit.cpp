// Offline audit of the quest marker: runs questplan (the game's own planning code) over a module's files
// for every quest and every journal entry, and prints what the marker would point at.
//   qaudit <module.nwm> <nwn_base.key> <dialog.tlk> [quest tags...]
#include "../../src/questplan.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

static std::vector<uint8_t> ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static uint32_t U32(const std::vector<uint8_t>& d, size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; }
static uint16_t U16(const std::vector<uint8_t>& d, size_t o) { uint16_t v = 0; if (o + 2 <= d.size()) memcpy(&v, &d[o], 2); return v; }

struct Res { std::string file; uint32_t off, size; };
static std::map<std::pair<std::string, uint16_t>, Res> g_mod;
static std::map<std::string, std::vector<uint8_t>> g_files;
static std::vector<uint8_t> g_tlk;

static void LoadErf(const std::string& p) {
    auto& d = g_files[p] = ReadFile(p);
    uint32_t ec = U32(d, 16), ok = U32(d, 24), orr = U32(d, 28);
    bool v11 = memcmp(&d[4], "V1.1", 4) == 0;
    size_t rl = v11 ? 32 : 16, ks = rl + 8;
    for (uint32_t i = 0; i < ec; i++) {
        size_t k = ok + i * ks;
        std::string name((const char*)&d[k], strnlen((const char*)&d[k], rl));
        uint32_t rid = U32(d, k + rl); uint16_t rt = U16(d, k + rl + 4);
        g_mod[{QD_Lower(name), rt}] = {p, U32(d, orr + rid * 8), U32(d, orr + rid * 8 + 4)};
    }
}
static std::vector<uint8_t> ReadAt(const std::string& p, uint32_t off, uint32_t size) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> d(size);
    f.seekg(off); f.read((char*)d.data(), size);
    if ((uint32_t)f.gcount() != size) d.resize((size_t)f.gcount());
    return d;
}
static std::vector<std::string> g_bifs;
static std::map<std::pair<std::string, uint16_t>, uint32_t> g_key;          // -> resource id
static std::map<std::string, std::map<uint32_t, std::pair<uint32_t, uint32_t>>> g_bifIdx;
static void LoadKey(const std::string& keyp) {
    auto k = ReadFile(keyp);
    std::string base = keyp.substr(0, keyp.rfind('/')); base = base.substr(0, base.rfind('/'));
    uint32_t nb = U32(k, 8), nk = U32(k, 12), offf = U32(k, 16), offk = U32(k, 20);
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t noff = U32(k, offf + i * 12 + 4); uint16_t nsz = U16(k, offf + i * 12 + 8);
        std::string n((const char*)&k[noff], strnlen((const char*)&k[noff], nsz));
        for (auto& c : n) if (c == '\\') c = '/';
        g_bifs.push_back(base + "/" + n);
    }
    for (uint32_t i = 0; i < nk; i++) {
        size_t o = offk + i * 22;
        std::string name((const char*)&k[o], strnlen((const char*)&k[o], 16));
        g_key[{QD_Lower(name), U16(k, o + 16)}] = U32(k, o + 18);
    }
}
static bool Load(const char* resref, uint16_t type, std::vector<uint8_t>& out) {
    std::pair<std::string, uint16_t> k{QD_Lower(resref), type};
    auto it = g_mod.find(k);
    if (it != g_mod.end()) {
        auto& d = g_files[it->second.file];
        if (it->second.off + it->second.size > d.size()) return false;
        out.assign(d.begin() + it->second.off, d.begin() + it->second.off + it->second.size);
        return true;
    }
    auto kk = g_key.find(k);
    if (kk == g_key.end()) return false;
    const std::string& bf = g_bifs[kk->second >> 20];
    auto bi = g_bifIdx.find(bf);
    if (bi == g_bifIdx.end()) {
        auto h = ReadAt(bf, 0, 20);
        uint32_t nv = U32(h, 8), voff = U32(h, 16);
        auto t = ReadAt(bf, voff, nv * 16);
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> m;
        for (uint32_t j = 0; j < nv; j++) m[U32(t, j * 16) & 0xfffff] = {U32(t, j * 16 + 4), U32(t, j * 16 + 8)};
        bi = g_bifIdx.emplace(bf, std::move(m)).first;
    }
    auto r = bi->second.find(kk->second & 0xfffff);
    if (r == bi->second.end()) return false;
    out = ReadAt(bf, r->second.first, r->second.second);
    return true;
}
static std::string Tlk(uint32_t i) {
    auto& T = g_tlk;
    uint32_t n = U32(T, 12), so = U32(T, 16);
    if (i >= n) return std::string();
    size_t e = 20 + (size_t)i * 40;
    uint32_t off = U32(T, e + 28), ln = U32(T, e + 32);
    return std::string((const char*)&T[so + off], ln);
}

static std::string P(const std::string& x) { std::string r; for (char c : x) { if (c == 1) r += "<self>"; else if (c == 2) r += "<n>"; else if (c == 4) r += "local:"; else if (c == 5) r += "<assoc>"; else if (c == 6) r += "anyof:"; else r += c; } return r; }
struct Info { std::string kind, name, tag, area; };
static std::map<uint32_t, Info> g_info;
static std::map<uint32_t, std::string> g_areaName;

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "qaudit module.nwm base.key dialog.tlk [quests]\n"); return 1; }
    LoadErf(argv[1]); LoadKey(argv[2]); g_tlk = ReadFile(argv[3]);
    QD_Load = Load; QD_Tlk = Tlk;
    std::set<std::string> only;
    for (int i = 4; i < argc; i++) only.insert(QD_Lower(argv[i]));
    QD_LoadJournal();
    static std::map<std::string, std::set<std::string>> g_from;     // quest + tag -> sources
    QP::Trace = [](const std::string& q, const std::string& t, uint32_t e, const std::string& from) { g_from[q + " " + t].insert(from + "@" + std::to_string(e)); };
    bool why = only.count("--why") > 0;
    only.erase("--why");
    if (only.count("--game")) {                          // as in game at the start of a stage: no local int set yet
        only.erase("--game");
        QP::LocalInt = [](const std::string& k, int& v) { if (k[0] != 'm' && k[0] != 'p') return false; v = 0; return true; };
    }
    for (auto& o : only) if (o.compare(0, 6, "--ncs=") == 0) {      // one script's analysis
        std::vector<uint8_t> d; Load(o.substr(6).c_str(), 2010, d);
        auto in = NcsFlow::Analyze(d.data(), d.size());
        printf("ok=%d\n", in.ok);
        for (auto& c : in.calls) {
            printf("call %s %d%s tags:", c.quest.c_str(), c.entry, c.override_ ? " (override)" : "");
            for (auto& t : c.tags) printf(" %s", P(t).c_str());
            printf(" guards:");
            for (auto& g : c.guards) printf(" %s%c%d", P(g.quest).c_str(), g.op, g.n);
            printf("\n");
        }
        for (auto& a : in.pass) { printf("pass:"); for (auto& g : a) printf(" %s%c%d", P(g.quest).c_str(), g.op, g.n); printf("\n"); }
        printf("req:"); for (auto& t : in.req) printf(" %s", P(t).c_str()); printf("\n");
        printf("cond:"); for (auto& t : in.cond) printf(" %s", P(t).c_str()); printf("\n");
        printf("setsInt:"); for (auto& t : in.setsInt) printf(" %s", t.c_str()); printf("\n");
        for (auto& c : in.creates) printf("create %d %s %s\n", c.routine, P(c.tmpl).c_str(), c.newTag.c_str());
        printf("jumps:"); for (auto& t : in.jumps) printf(" %s", P(t).c_str()); printf("\n");
        return 0;
    }
    if (only.count("--scripts")) {                       // time the analysis of every script of the module
        for (auto& r : g_mod) {
            if (r.first.second != 2010) continue;
            std::vector<uint8_t> d; Load(r.first.first.c_str(), 2010, d);
            auto t0 = std::chrono::steady_clock::now();
            auto in = NcsFlow::Analyze(d.data(), d.size());
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 2 || !in.ok) printf("%-16s %6zu bytes %8.1f ms ok=%d calls=%zu\n", r.first.first.c_str(), d.size(), ms, in.ok, in.calls.size());
            fflush(stdout);
        }
        return 0;
    }

    // ---- the module as it starts ----
    std::vector<uint8_t> v; Gff ifo;
    Load("module", 2014, v); ifo.Load(std::move(v));
    std::vector<std::string> modScripts, areas;
    for (auto& f : ifo.Fields(0)) {
        if (f.type == 11 && f.label.compare(0, 6, "Mod_On") == 0) modScripts.push_back(QD_Lower(ifo.Ref(f)));
        if (f.label == "Mod_Area_list") for (uint32_t s : ifo.List(f)) for (auto& af : ifo.Fields(s)) if (af.label == "Area_Name") areas.push_back(QD_Lower(ifo.Ref(af)));
    }
    std::vector<QP::Obj> objs;
    std::vector<std::pair<uint32_t, std::vector<std::string>>> areaScripts;
    std::vector<std::pair<std::string, QP::Target>> items;
    struct Link { uint32_t obj, area; QP::P3 at; std::string to; };
    std::vector<Link> links;
    std::map<std::string, uint32_t> areaOfTag;
    uint32_t nextId = 1;
    for (size_t ai = 0; ai < areas.size(); ai++) {
        uint32_t aid = 0x100000 + (uint32_t)ai;
        Gff are, git;
        if (!Load(areas[ai].c_str(), 2012, v)) continue;
        are.Load(std::move(v));
        if (!Load(areas[ai].c_str(), 2023, v)) continue;
        git.Load(std::move(v));
        std::vector<std::string> asc;
        for (auto& f : are.Fields(0)) {
            if (f.label == "Name" && f.type == 12) g_areaName[aid] = are.Loc(f);
            if (f.type == 11 && f.label.compare(0, 2, "On") == 0) asc.push_back(QD_Lower(are.Ref(f)));
        }
        areaScripts.push_back({aid, asc});
        g_info[aid] = {"area", g_areaName[aid], areas[ai], g_areaName[aid]};
        for (auto& top : git.Fields(0)) {
            if (top.type != 15) continue;
            uint8_t type = top.label == "Creature List" ? 5 : top.label == "TriggerList" ? 7 : top.label == "Placeable List" ? 9 :
                           top.label == "Door List" ? 10 : top.label == "WaypointList" ? 12 : top.label == "List" ? 6 : 0;
            if (!type) continue;
            for (uint32_t s : git.List(top)) {
                auto fs = git.Fields(s);
                QP::Obj o; o.id = nextId++; o.area = aid; o.type = type;
                std::string name, last, linked;
                std::vector<std::string> inv;
                for (auto& f : fs) {
                    if (f.label == "Tag" && f.type == 10) o.tag = QD_Lower(git.Str(f));
                    else if ((f.label == "XPosition" || f.label == "X") && f.type == 8) memcpy(&o.pos.x, &f.data, 4);
                    else if ((f.label == "YPosition" || f.label == "Y") && f.type == 8) memcpy(&o.pos.y, &f.data, 4);
                    else if ((f.label == "ZPosition" || f.label == "Z") && f.type == 8) memcpy(&o.pos.z, &f.data, 4);
                    else if (f.label == "Conversation" && f.type == 11) o.dialog = QD_Lower(git.Ref(f));
                    else if (f.type == 11 && (f.label.compare(0, 6, "Script") == 0 || f.label.compare(0, 2, "On") == 0)) { std::string sc = QD_Lower(git.Ref(f)); if (!sc.empty()) o.scripts.push_back(sc); }
                    else if ((f.label == "FirstName" || f.label == "LocName" || f.label == "LocalizedName" || f.label == "Description") && f.type == 12 && name.empty()) name = git.Loc(f);
                    else if (f.label == "LastName" && f.type == 12) last = git.Loc(f);
                    else if (f.label == "LinkedTo" && f.type == 10) linked = QD_Lower(git.Str(f));
                    else if (f.type == 15 && (f.label == "ItemList" || f.label == "Equip_ItemList"))
                        for (uint32_t is : git.List(f)) for (auto& itf : git.Fields(is)) if (itf.label == "Tag" && itf.type == 10) inv.push_back(QD_Lower(git.Str(itf)));
                }
                if (!last.empty()) name += " " + last;
                const char* kinds[] = {"", "", "", "", "", "cre", "item", "trg", "", "plc", "door", "", "wp"};
                g_info[o.id] = {kinds[type], name, o.tag, g_areaName[aid]};
                QP::Target here; here.id = o.id; here.area = aid; here.pos = o.pos;
                if (type == 6) { items.push_back({o.tag, here}); continue; }
                for (auto& it : inv) items.push_back({it, here});
                if ((type == 10 || type == 12) && !o.tag.empty()) areaOfTag.emplace(o.tag, aid);
                if ((type == 10 || type == 7) && !linked.empty()) links.push_back({o.id, aid, o.pos, linked});
                objs.push_back(std::move(o));
            }
        }
    }
    std::vector<QP::Edge> transitions;
    for (auto& l : links) { auto a = areaOfTag.find(l.to); if (a != areaOfTag.end() && a->second != l.area) transitions.push_back({l.area, a->second, l.at, l.obj}); }

    auto describe = [&](const QP::Target& t) {
        auto& i = g_info[t.id];
        char b[400];
        if (t.isArea) snprintf(b, sizeof b, "area \"%s\"", i.name.c_str());
        else snprintf(b, sizeof b, "%s \"%s\" [%s] in %s  <%s %s>", i.kind.c_str(), i.name.c_str(), i.tag.c_str(), i.area.c_str(), t.why, t.giver ? "" : P(t.via).c_str());
        return std::string(b);
    };

    for (auto& qd : QD_quests) {
        const std::string& quest = qd.first;
        if (!only.empty() && !only.count(quest)) continue;
        printf("====================================================================================================\n");
        printf("QUEST %s  \"%s\"\n", quest.c_str(), qd.second.title.c_str());
        // its entries (ids) in order
        std::vector<uint8_t> jv; Gff jrl; Load("module", 2056, jv); jrl.Load(std::move(jv));
        std::map<uint32_t, std::string> texts;
        for (auto& top : jrl.Fields(0)) if (top.label == "Categories")
            for (uint32_t c : jrl.List(top)) {
                auto cf = jrl.Fields(c); auto* tg = jrl.Find(cf, "Tag");
                if (!tg || QD_Lower(jrl.Str(*tg)) != quest) continue;
                auto* el = jrl.Find(cf, "EntryList");
                if (el) for (uint32_t e : jrl.List(*el)) {
                    uint32_t id = 0; std::string tx;
                    for (auto& ef : jrl.Fields(e)) { if (ef.label == "ID") id = ef.data; if (ef.label == "Text") tx = jrl.Loc(ef); }
                    texts[id] = tx;
                }
            }
        for (auto& te : texts) {
            if (qd.second.ends.count(te.first)) continue;
            QP::Round r;
            g_from.clear();
            r.state[quest] = te.first;
            r.known.insert(quest);
            r.Module(modScripts);
            for (auto& a : areaScripts) r.Area(a.first, a.second);
            for (auto& o : objs) r.Object(o);
            bool done = false;
            while (!done) r.Spawned(done);
            r.Match();
            for (auto& it : items) r.Item(it.first, it.second);
            std::string tx = te.second; for (auto& c : tx) if (c == '\n' || c == '\r') c = ' ';
            if (tx.size() > 260) tx = tx.substr(0, 260) + "...";
            printf("-- at entry %u: %s\n", te.first, tx.c_str());
            std::vector<const QP::Target*> all;
            for (auto& t : r.objectives[quest]) all.push_back(&t);
            for (auto& t : r.givers[quest]) all.push_back(&t);
            if (all.empty()) { printf("   NOTHING\n"); continue; }
            // by entry, objectives first; duplicates once
            std::map<std::pair<uint32_t, int>, std::set<std::string>> by;
            for (auto* t : all) by[{t->entry, t->giver ? 1 : 0}].insert(describe(*t));
            auto best = r.Best(quest, all);
            if (best.empty()) { printf("   NOTHING (party)\n"); continue; }
            std::set<std::string> bs; for (auto* t : best) bs.insert(describe(*t));
            printf("   MARKER (-> %u%s):\n", best[0]->entry, best[0]->giver ? ", giver" : "");
            for (auto& s : bs) printf("     * %s\n", s.c_str());
            int shown = 0;
            for (auto& b : by) {
                bool allShown = true;
                for (auto& x : b.second) if (!bs.count(x)) allShown = false;
                if (allShown) continue;
                if (++shown > 4) { printf("   ...\n"); break; }
                printf("   then -> %u%s: ", b.first.first, b.first.second ? " (giver)" : "");
                int k = 0;
                for (auto& s : b.second) { if (k++ >= 5) { printf(" ..."); break; } printf("%s%s", k > 1 ? "; " : "", s.c_str()); }
                printf("\n");
            }
            if (!r.goal[quest].empty()) {
                printf("   tags:");
                int k = 0;
                for (auto& g : r.goal[quest]) { if (k++ >= 20) break; std::string t = P(g.first); printf(" %s(%u)", t.c_str(), g.second); }
                printf("\n");
                if (why) for (auto& g : r.goal[quest]) {
                    printf("      %s <-", P(g.first).c_str());
                    for (auto& f : g_from[quest + " " + g.first]) printf(" %s", f.c_str());
                    printf("\n");
                }
            }
        }
    }
    (void)transitions;
    return 0;
}
