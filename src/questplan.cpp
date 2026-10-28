// NWNPad - one round of the quest marker's planning (see questplan.h)
//
// Every object that can move an active quest on is a candidate, with the lowest journal entry it leads to:
//  - its own scripts write a later entry (a creature's death, a trigger's enter, a placeable used);
//  - it has a tag, or carries / holds an item with a tag, that the tests around a later entry's write look
//    up or compare (the acquired item in the module's OnAcquireItem, "has the item" in a conversation);
//  - its scripts or conversation create such a thing, or someone whose conversation or scripts move it on;
//  - its conversation writes a later entry (a giver).
// The marker goes to the lowest entry, objectives before givers, then the nearest.
#include "questplan.h"
#include <algorithm>
#include <cstring>
#include <deque>

namespace QP {

std::vector<std::string> Expand(const std::string& t, const std::string& owner) {
    std::vector<std::string> r;
    std::string s;
    for (char c : t) { if (c == '\x01') s += owner; else s += c; }
    if (t.find('\x01') != std::string::npos && owner.empty()) return r;
    size_t p = s.find('\x02');
    if (p == std::string::npos) { r.push_back(s); return r; }
    if (s.find('\x02', p + 1) != std::string::npos) return r;
    for (char dgt = '0'; dgt <= '9'; dgt++) r.push_back(s.substr(0, p) + dgt + s.substr(p + 1));
    return r;
}

std::vector<std::string> Scripts(const std::vector<std::string>& names, const std::string& tag) {
    std::vector<std::string> r;
    std::deque<std::pair<std::string, int>> q;
    for (auto& n : names) if (!n.empty()) q.push_back({n, 0});
    while (!q.empty()) {
        auto [n, depth] = q.front(); q.pop_front();
        if (std::find(r.begin(), r.end(), n) != r.end()) continue;
        const NcsInfo& ni = QD_Script(n);
        if (ni.Relevant()) r.push_back(n);
        if (depth < 2)
            for (auto& e : ni.execs)
                for (auto& x : Expand(e, tag)) if (x.size() <= 16) q.push_back({x, depth + 1});
    }
    return r;
}

bool Round::Later(const std::string& q, int64_t entry) const {
    auto it = state.find(q);
    return it != state.end() && (entry < 0 || entry >= 0x7fffffff || entry > (int64_t)it->second);
}
bool Round::GuardsOk(const std::vector<NcsFlow::Guard>& gs, bool locals) const {
    for (auto& g : gs) {
        if (!strchr("=!<>lg", g.op) || g.quest.empty() || g.quest[0] == '\x05') continue;    // items, objects: not known here
        if (g.quest[0] == '\x04') {                                         // a local int: when it can be read
            int v;
            if (locals && LocalInt && LocalInt(g.quest.substr(1), v) && !NcsFlow::Pass(g, v)) return false;
            continue;
        }
        auto it = state.find(g.quest);
        int st;
        if (it != state.end()) st = (int)it->second;
        else if (known.count(g.quest)) continue;                // finished: its entry is not known here
        else st = 0;
        if (!NcsFlow::Pass(g, st)) return false;
    }
    return true;
}

bool (*LocalInt)(const std::string&, int&) = nullptr;
void (*Trace)(const std::string&, const std::string&, uint32_t, const std::string&) = nullptr;

void Round::Goal(const std::string& q, const std::string& tag, const std::string& owner, uint32_t entry, const std::string& from) {
    if (tag.compare(0, 16, "nw_journal_entry") == 0) return;
    if (tag[0] == '\x06') {                              // one of several items: each is a goal
        for (size_t a = 1, b; a <= tag.size(); a = b + 1) { b = tag.find('|', a); if (b == std::string::npos) b = tag.size(); if (b > a) Goal(q, tag.substr(a, b - a), owner, entry, from); }
        return;
    }
    if (tag[0] == '\x04') { int v; if (LocalInt && LocalInt(tag.substr(1), v) && v != 0) return; }   // set already
    for (auto& t : Expand(tag, owner)) {
        if (t.empty()) continue;
        auto& g = goal[q];
        auto it = g.find(t);
        if (it == g.end() || entry < it->second) g[t] = entry;
        if (Trace) Trace(q, t, entry, from);
    }
}

// several lines write the same entry: what that entry needs is what all of them need (one of the items each
// needs, when they need different ones)
static void Meet(std::map<uint32_t, std::vector<std::string>>& m, uint32_t e, const std::vector<std::string>& tags) {
    auto it = m.find(e);
    if (it == m.end()) { m[e] = tags; return; }
    auto isItem = [](const std::string& t) { return !t.empty() && t[0] != '\x04' && t[0] != '\x06' && t.compare(0, 16, "nw_journal_entry") != 0; };
    std::vector<std::string> r, ia, ib;
    for (auto& t : it->second) { if (std::find(tags.begin(), tags.end(), t) != tags.end()) r.push_back(t); if (isItem(t)) ia.push_back(t); }
    for (auto& t : tags) if (isItem(t)) ib.push_back(t);
    bool kept = false;
    for (auto& t : r) if (isItem(t)) kept = true;
    if (!kept && !ia.empty() && !ib.empty()) {
        std::set<std::string> u(ia.begin(), ia.end()); u.insert(ib.begin(), ib.end());
        std::string k = "\x06";
        for (auto& y : u) { if (k.size() > 1) k += '|'; k += y; }
        r.push_back(k);
    }
    it->second.swap(r);
}

// a conversation in this round: the lines you can get to (the journal tests of the conditions on the way
// pass), the later entries of active quests they write, the quests they start
const Round::DlgRound& Round::Eval(const DlgInfo& d) {
    auto it = dlg.find(&d);
    if (it != dlg.end()) return it->second;
    DlgRound rd;
    int total = (int)d.out.size();
    // the lines you can get to; again without the local ints (whatever sets them may not be found: then
    // those lines are the fallback)
    std::vector<char> reach(total, 0), loose(total, 0);
    for (int mode = 0; mode < 2; mode++) {
        std::vector<char>& R = mode ? loose : reach;
        std::vector<char> can(d.conds.size(), -1);
        auto condOk = [&](int c) {
            if (c < 0) return true;
            if (can[c] < 0) {
                const NcsInfo& ni = QD_Script(d.conds[c]);
                bool ok = ni.pass.empty();
                for (auto& alt : ni.pass) if (GuardsOk(alt, mode == 0)) { ok = true; break; }
                can[c] = ok;
            }
            return can[c] != 0;
        };
        std::deque<int> q;
        // the NPC says the first of his lines whose condition passes: none after one without a condition
        auto walk = [&](const std::vector<DlgLink>& ls, bool ordered) {
            for (auto& l : ls) {
                if (!R[l.to] && condOk(l.cond)) { R[l.to] = 1; q.push_back(l.to); }
                if (ordered && l.cond < 0) break;
            }
        };
        walk(d.start, true);
        while (!q.empty()) {
            int x = q.front(); q.pop_front();
            walk(d.out[x], x >= d.entries);
        }
    }
    for (auto& w : d.writes) {
        if (!reach[w.node] && loose[w.node] && GuardsOk(w.guards, false) && !w.status && Later(w.quest, w.entry)) {
            Meet(rd.loose[w.quest], w.entry, w.tags);
            continue;
        }
        if (!reach[w.node] || !GuardsOk(w.guards) || w.status) continue;
        // the first entry of a quest you don't have: its owner offers it
        auto def = QD_quests.find(w.quest);
        if (!known.count(w.quest) && def != QD_quests.end() && w.entry == def->second.first && !def->second.ends.count(w.entry)) {
            bool maybe = false;            // its conditions test another quest you haven't got: maybe not yet
            for (auto& t : w.tags)
                if (t.compare(0, 16, "nw_journal_entry") == 0) {
                    std::string o = t.substr(16);
                    if (o != w.quest && !known.count(o) && QD_quests.count(o)) maybe = true;
                }
            if (!maybe) rd.maybe = false;
            if (std::find(rd.offers.begin(), rd.offers.end(), w.quest) == rd.offers.end()) rd.offers.push_back(w.quest);
        }
        if (!Later(w.quest, w.entry)) continue;
        auto& v = rd.moves[w.quest][w.entry];                 // (what any of them tests: the goals)
        v.insert(v.end(), w.tags.begin(), w.tags.end());
        Meet(rd.needs[w.quest], w.entry, w.tags);
    }
    rd.reach = loose;
    return dlg.emplace(&d, std::move(rd)).first->second;
}

// journal writes of scripts run by an object (self: the object, nullptr for the module): later entries make
// it an objective, the tags tested before them are what the quest waits for
void Round::Calls(const std::vector<std::string>& scripts, const Target* self, const std::string& tag, const std::set<std::string>& skip) {
    for (auto& sc : scripts) {
        const NcsInfo& ni = QD_Script(sc);
        for (auto& c : ni.calls) {
            if (!Later(c.quest, c.entry) || !GuardsOk(c.guards)) continue;
            bool status = c.override_, assoc = false;
            for (auto& g : c.guards) {
                if (g.quest == c.quest) status = false;           // written over whatever it was: a status label
                if (g.quest == "\x05master" && g.op == 'o') assoc = true;   // only while someone's associate
            }
            if (status) continue;
            uint32_t e = c.entry < 0 ? 0x7fffffff : (uint32_t)c.entry;
            for (auto& t : c.tags) Goal(c.quest, t, tag, e, sc);
            // (one who talks the quest on is no target of its own death / scripts)
            if (self && !skip.count(c.quest) && !assoc) {
                Target t = *self; t.entry = e; t.why = self->isArea ? "area script" : "own script"; t.via = sc;
                for (auto& x : c.tags) for (auto& y : Expand(x, tag)) t.needs += " " + y;
                objectives[c.quest].push_back(t);
            }
        }
    }
}

bool Round::Module(const std::vector<std::string>& names) {
    std::vector<std::string> sc = Scripts(names, std::string());
    if (QD_pending) return false;
    Calls(sc, nullptr, std::string(), {});
    return true;
}

bool Round::Area(uint32_t id, const std::vector<std::string>& names) {
    std::vector<std::string> sc = Scripts(names, std::string());
    if (QD_pending) return false;
    Target at; at.id = id; at.area = id; at.isArea = true;
    Calls(sc, &at, std::string(), {});
    return true;
}

bool Round::Object(const Obj& o) {
    if (o.type == 12) { if (!o.tag.empty()) { Target t; t.id = o.id; t.area = o.area; t.pos = o.pos; byTag[o.tag].push_back(t); } return true; }
    if (o.type != 5 && o.type != 7 && o.type != 9 && o.type != 10) return true;
    if (o.party) {                                     // one of yours: only what his conversation moves on
        if (o.dialog.empty()) return true;
        const DlgInfo& d = QD_Dialog(o.dialog);
        const DlgRound* rd = d.ok ? &Eval(d) : nullptr;
        if (QD_pending) return false;
        if (rd)
            for (auto& m : rd->moves) {
                Target g; g.id = o.id; g.area = o.area; g.pos = o.pos; g.giver = g.party = true; g.entry = m.second.begin()->first; g.why = "party";
                givers[m.first].push_back(g);
            }
        return true;
    }
    // everything first (a resource not read yet: the whole object again later)
    std::vector<std::string> sc = Scripts(o.scripts, o.tag);
    std::vector<const DlgInfo*> dlgs;
    std::set<std::string> convs;
    if (o.type != 7 && !o.dialog.empty()) convs.insert(o.dialog);
    for (auto& s : sc) { const NcsInfo& ni = QD_Script(s); convs.insert(ni.convs.begin(), ni.convs.end()); }
    for (auto& c : convs) { const DlgInfo& d = QD_Dialog(c); if (d.ok) dlgs.push_back(&d); }
    std::vector<const DlgRound*> rds;
    for (auto* d : dlgs) rds.push_back(&Eval(*d));
    if (QD_pending) return false;

    Target self; self.id = o.id; self.area = o.area; self.pos = o.pos;
    if (!o.tag.empty()) byTag[o.tag].push_back(self);
    std::set<std::string> talks;
    for (auto* rd : rds) {
        for (auto& m : rd->moves) {
            Target g = self; g.giver = true; g.entry = m.second.begin()->first; g.why = "conversation";
            for (auto& t : rd->needs.at(m.first).begin()->second) for (auto& x : Expand(t, o.tag)) g.needs += " " + x;
            givers[m.first].push_back(g);
            talks.insert(m.first);
            for (auto& e : m.second) for (auto& t : e.second) Goal(m.first, t, o.tag, e.first, "dialog of " + o.tag);
        }
        for (auto& m : rd->loose) {
            if (rd->moves.count(m.first)) continue;
            Target g = self; g.giver = g.loose = true; g.entry = m.second.begin()->first; g.why = "conversation?";
            givers[m.first].push_back(g);
            // what those lines wait for (the local ints not set yet among them): whoever sets them is the way
            for (auto& e : m.second) for (auto& t : e.second) Goal(m.first, t, o.tag, e.first, "dialog of " + o.tag + " (shut)");
        }
        if (o.type == 5 && !rd->offers.empty()) offers.push_back({o.id, o.area, o.pos, rd->maybe, rd->offers});
    }
    std::set<std::string> talksAny = talks;              // every quest its conversations write: no target of its own death
    for (auto* d : dlgs) talksAny.insert(d->quests.begin(), d->quests.end());
    Calls(sc, &self, o.tag, talksAny);
    // what it creates (scripts, conversation), the strings its scripts keep (a blueprint for later), where it
    // sends you
    // (under the journal / local int tests on the way to it; tests of what is not known: maybe)
    auto spawn = [&](uint16_t r, const std::string& res, const std::string& nt, const std::vector<NcsFlow::Guard>* gs) {
        bool maybe = false;
        if (gs) {
            if (!GuardsOk(*gs)) return;
            for (auto& g : *gs) if (g.quest.size() > 2 && g.quest[0] == '\x04' && (g.quest[1] == 's' || g.quest[1] == 'o')) maybe = true;
        }
        for (auto& x : Expand(res, o.tag)) if (!x.empty() && x.size() <= 16) { created.push_back({self, o.tag, r, x, nt, 0, talks}); created.back().maybe = maybe; }
    };
    for (auto& s : sc) {
        const NcsInfo& ni = QD_Script(s);
        for (auto& c : ni.creates) spawn(c.routine, c.tmpl, c.newTag, &c.guards);
        for (auto& l : ni.locals) { spawn(243, l, std::string(), nullptr); spawn(31, l, std::string(), nullptr); }
        for (auto& j : ni.jumps) for (auto& x : Expand(j, o.tag)) jumps.push_back({self, x});
        for (auto& v : ni.setsInt) setters.push_back({self, "\x04" + v, talks});
    }
    for (size_t di = 0; di < dlgs.size(); di++) {
        const DlgInfo* d = dlgs[di];
        const std::vector<char>& rc = rds[di]->reach;
        for (auto& c : d->creates) if (c.node < (int)rc.size() && rc[c.node]) spawn(c.c.routine, c.c.tmpl, c.c.newTag, &c.c.guards);
        for (auto& j : d->jumps) if (j.node < (int)rc.size() && rc[j.node]) for (auto& x : Expand(j.tag, o.tag)) jumps.push_back({self, x});
        for (auto& v : d->setsInt) setters.push_back({self, "\x04" + v, talks});
    }
    return true;
}

bool Round::Spawned(bool& done) {
    done = ci >= created.size();
    if (done) return true;
    Spawn sp = created[ci];
    const Blueprint& bp = QD_Blueprint(sp.routine, sp.resref);
    const DlgRound* rd = bp.ok && bp.dlg ? &Eval(*bp.dlg) : nullptr;
    ci++;
    if (QD_pending) {                                    // not read yet: again after the others
        if (++sp.tries < 1000) created.push_back(sp);
        return false;
    }
    if (!bp.ok) return true;
    if (!bp.tag.empty() && bp.tag == sp.selfTag) return true;           // a copy of itself (henchmen ...)
    if (!bp.tag.empty()) spawners.push_back({sp.self, bp.tag, sp.talks, sp.maybe});
    if (!sp.newTag.empty()) spawners.push_back({sp.self, sp.newTag, sp.talks, sp.maybe});
    // someone whose conversation or scripts move a quest on: the one who creates him is the way there
    if (rd)
        for (auto& m : rd->moves) {
            if (sp.talks.count(m.first)) continue;
            Target t = sp.self; t.entry = m.second.begin()->first; t.why = "creates one who talks"; t.via = sp.resref; t.maybe = sp.maybe;
            objectives[m.first].push_back(t);
        }
    for (auto& bsc : bp.scripts)
        for (auto& c : QD_Script(bsc).calls)
            if (Later(c.quest, c.entry) && GuardsOk(c.guards) && !sp.talks.count(c.quest)) {
                Target t = sp.self; t.entry = c.entry < 0 ? 0x7fffffff : (uint32_t)c.entry; t.why = "creates one whose script"; t.via = sp.resref; t.maybe = sp.maybe;
                objectives[c.quest].push_back(t);
            }
    for (auto& bsc : bp.scripts) for (auto& v : QD_Script(bsc).setsInt) setters.push_back({sp.self, "\x04" + v, sp.talks});
    if (sp.depth < 2) {
        for (auto& it : bp.items) { created.push_back({sp.self, sp.selfTag, 31, it, std::string(), sp.depth + 1, sp.talks}); created.back().maybe = sp.maybe; }
        for (auto& bsc : bp.scripts)
            for (auto& c : QD_Script(bsc).creates)
                for (auto& x : Expand(c.tmpl, bp.tag)) if (!x.empty() && x.size() <= 16) { created.push_back({sp.self, sp.selfTag, c.routine, x, c.newTag, sp.depth + 1, sp.talks}); created.back().maybe = sp.maybe; }
    }
    return true;
}

void Round::Match() {
    for (auto& g : goal) for (auto& t : g.second) questsOf[t.first].push_back({g.first, t.second});
    for (auto& q : questsOf) {
        auto bt = byTag.find(q.first);
        if (bt == byTag.end()) continue;
        for (auto& qe : q.second)
            for (auto t : bt->second) { t.entry = qe.second; t.why = "tag"; t.via = q.first; objectives[qe.first].push_back(t); }
    }
    for (auto& sp : spawners) {
        auto q = questsOf.find(sp.tag);
        if (q == questsOf.end()) continue;
        for (auto& qe : q->second) {
            if (sp.talks.count(qe.first)) continue;          // a giver handing it over (or taking it back)
            Target t = sp.self; t.entry = qe.second; t.why = "creates"; t.via = sp.tag; t.maybe = sp.maybe; objectives[qe.first].push_back(t);
        }
    }
    // local ints a later entry needs: whoever sets them
    for (auto& st : setters) {
        auto q = questsOf.find(st.tag);
        if (q == questsOf.end()) continue;
        for (auto& qe : q->second) {
            if (st.talks.count(qe.first)) continue;          // the giver's own conversation keeping count
            Target t = st.self; t.entry = qe.second; t.why = "sets"; t.via = st.tag; objectives[qe.first].push_back(t);
        }
    }
    // teleports: from whoever sends you to the area of where you land
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (auto& j : jumps) {
        auto bt = byTag.find(j.second);
        if (bt == byTag.end() || bt->second.empty()) continue;
        uint32_t to = bt->second[0].area;
        if (to == j.first.area || !seen.insert({j.first.id, to}).second) continue;
        edges.push_back({j.first.area, to, j.first.pos, j.first.id});
    }
}

void Round::Item(const std::string& tag, const Target& where, bool mine) {
    if (mine) { held.insert(tag); return; }
    auto q = questsOf.find(tag);
    if (q == questsOf.end()) return;
    for (auto& qe : q->second) { Target t = where; t.entry = qe.second; t.why = "item"; t.via = tag; objectives[qe.first].push_back(t); }
}

// the lowest entry; a giver whose line needs what an objective of that entry was found by (the head the
// chieftain carries) waits for it
// (an entry that ends the quest comes after those that carry on: a fail / an alternative ending numbered lower)
std::vector<const Target*> Round::Best(const std::string& quest, const std::vector<const Target*>& v0) const {
    auto def = QD_quests.find(quest);
    auto R = [&](const Target* t) -> uint64_t { return t->entry + (def != QD_quests.end() && def->second.ends.count(t->entry) ? (1ull << 32) : 0); };
    // what you carry already is no objective any more, and nobody waits for it
    std::vector<const Target*> v;
    bool firm = false;                                    // anyone but the fallback givers
    for (auto* t : v0) if (!t->loose) firm = true;
    for (auto* t : v0) {
        if (firm && t->loose) continue;
        if (!t->giver && !held.empty() && held.count(t->via) && (!strcmp(t->why, "creates") || !strcmp(t->why, "tag") || !strcmp(t->why, "item"))) continue;
        v.push_back(t);
    }
    std::vector<const Target*> r;
    uint64_t lo = ~0ull;
    for (auto* t : v) if (R(t) < lo) lo = R(t);
    std::set<std::string> found;
    for (auto* t : v) if (R(t) == lo && !t->giver && (!strcmp(t->why, "tag") || !strcmp(t->why, "item") || !strcmp(t->why, "creates") || !strcmp(t->why, "sets"))) found.insert(t->via);
    for (auto* t : v) {
        if (R(t) != lo) continue;
        if (t->giver || !strcmp(t->why, "own script")) {      // waits for what its line / script needs
            bool waits = false;
            size_t p = 0;
            while (p < t->needs.size()) {
                size_t q = t->needs.find(' ', p + 1);
                if (q == std::string::npos) q = t->needs.size();
                std::string w = t->needs.substr(p + 1, q - p - 1);
                if (!w.empty() && w[0] == '\x06') {                // one of several: waits unless you have one
                    std::vector<std::string> grp;
                    for (size_t a = 1, b; a <= w.size(); a = b + 1) { b = w.find('|', a); if (b == std::string::npos) b = w.size(); grp.push_back(w.substr(a, b - a)); }
                    bool have = false, there = false;
                    for (auto& g : grp) { if (held.count(g)) have = true; if (found.count(g)) there = true; }
                    if (there && !have) waits = true;
                } else if (found.count(w) && !held.count(w)) waits = true;
                p = q;
            }
            if (waits) continue;
        }
        r.push_back(t);
    }
    if (r.empty()) for (auto* t : v) if (R(t) == lo) r.push_back(t);
    bool sure = false;                                    // created only maybe: after the sure ones
    for (auto* t : r) if (!t->maybe) sure = true;
    if (sure) r.erase(std::remove_if(r.begin(), r.end(), [](const Target* t) { return t->maybe; }), r.end());
    bool party = !r.empty();
    for (auto* t : r) if (!t->party) party = false;
    if (party) r.clear();                                 // talk to your companion (nothing to point at)
    return r;
}
}  // namespace QP
