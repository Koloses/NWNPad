// NWNPad - hub "Journal" page: active / completed quests from the client quest journal
#include "hub_pages.h"
#include "hub.h"
#include "render.h"
#include "game.h"
#include "gamedata.h"
#include "pad.h"
#include "log.h"
#include "inv.h"
#include "questmark.h"
#include "talklog.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace G;
using namespace R;

namespace {
// CNWCJournal (CClientExoAppInternal+0x70): done list at +0, active list at +0x28
// CNWCSortedJournalArrayList: entries* +0, count +8, sorted index array +0x10
// SJournalEntry (0x50): name loc +0, text loc +0x10, day +0x20, time +0x24, tag +0x28,
//   priority +0x38, state +0x3c, picture +0x40, completed +0x44, displayed +0x48, updated +0x4c
struct Quest { std::string name, text, tag; int day = 0, time = 0; bool completed = false, updated = false; };

std::vector<Quest> g_q[2];      // 0 active, 1 completed
int g_list = 0, g_sel[3] = {0, 0, 0}, g_scroll[3] = {0, 0, 0};   // list 2: conversations
std::vector<Talk> g_talks;      // every conversation, newest first
std::vector<Talk> g_qtalks;     // conversations of the selected quest
std::string g_qtalksFor;
bool g_view = false;            // the selected quest's conversations instead of its journal text
float g_talkScroll = 0; int g_talkFor = -1;
int g_jump = 0;                 // d-pad in the conversation view: -1 / +1 = previous / next conversation
float g_textScroll = 0; int g_textFor = -1;
double g_lastRead = -10;
std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

void ReadList(void* lst, std::vector<Quest>& out) {
    out.clear();
    if (!lst) return;
    char* base = F<char*>(lst, 0);
    int n = F<int>(lst, 8);
    int* idx = F<int*>(lst, 0x10);
    if (!base || n <= 0 || n > 2000) return;
    for (int i = 0; i < n; i++) {
        int k = idx ? idx[i] : i;
        if (k < 0 || k >= 4000) continue;
        char* e = base + (size_t)k * 0x50;
        if (!Readable(e, 0x50)) continue;
        Quest q;
        q.name = GD::LocStr(e);
        q.text = GD::LocStr(e + 0x10);
        q.day = F<int>(e, 0x20); q.time = F<int>(e, 0x24);
        q.completed = F<int>(e, 0x44) != 0;
        q.updated = F<int>(e, 0x4c) != 0;
        if (const char* t = F<const char*>(e, 0x28)) { uint32_t l = F<uint32_t>(e, 0x30); if (l > 0 && l < 64) q.tag.assign(t, l); }
        if (q.name.empty()) q.name = "(untitled)";
        out.push_back(q);
    }
}

void Read(bool force = false) {
    if (!force && g_pad.now - g_lastRead < 1.0) return;
    g_lastRead = g_pad.now;
    void* in = Internal();
    void* j = in ? F<void*>(in, 0x70) : nullptr;
    if (!j) { g_q[0].clear(); g_q[1].clear(); g_talks = TalkLog_All(); return; }
    ReadList((char*)j + 0x28, g_q[0]);
    ReadList(j, g_q[1]);
    g_talks = TalkLog_All();
    g_qtalksFor = "\x01";
    for (int l = 0; l < 2; l++) {
        if (g_sel[l] >= (int)g_q[l].size()) g_sel[l] = (int)g_q[l].size() - 1;
        if (g_sel[l] < 0) g_sel[l] = 0;
    }
}

// the server sends the quest list only while the journal screen is open
const uintptr_t VA_Msg_SendPlayerToServerMessage = 0x1407c10c0;
bool g_screen = false;
void Screen(bool open) {
    if (open == g_screen) return;
    void* m = Inv::Message();
    if (!m) return;
    g_screen = open;
    typedef int (*fn)(void*, uint8_t, uint8_t, void*, uint32_t);
    ((fn)A(VA_Msg_SendPlayerToServerMessage))(m, 0x1c, open ? 10 : 11, nullptr, 0);
    Log("journal: screen %s", open ? "open" : "closed");
}
void Close() { Screen(false); }

void Open() {
    Screen(true);
    g_lastRead = -10;
    Read(true);
    if (g_q[0].empty() && !g_q[1].empty()) g_list = 1;
    g_textFor = -1; g_talkFor = -1; g_view = false;
}

int Count(int l) { return l == 2 ? (int)g_talks.size() : (int)g_q[l].size(); }
const Quest* SelQuest() {
    if (g_list == 2 || g_q[g_list].empty()) return nullptr;
    return &g_q[g_list][std::min(g_sel[g_list], (int)g_q[g_list].size() - 1)];
}
const std::vector<Talk>& QuestTalks() {
    const Quest* q = SelQuest();
    std::string t = q ? q->name : std::string();
    if (t != g_qtalksFor) { g_qtalksFor = t; g_qtalks = q ? TalkLog_ForQuest(t) : std::vector<Talk>(); }
    return g_qtalks;
}

void Input() {
    Read();
    if (g_view) {                                   // the quest's conversations: d-pad jumps between them, B goes back
        if (g_pad.Repeat(PB_DUP)) g_jump = -1;
        if (g_pad.Repeat(PB_DDOWN)) g_jump = 1;
        if (fabsf(g_pad.ry) > 0.2f) g_talkScroll -= g_pad.ry * px(900.0f) * (float)g_pad.dt;
        if (g_pad.Pressed(PB_B) || g_pad.Pressed(PB_Y)) { g_view = false; g_talkFor = -1; }
        return;
    }
    int n = Count(g_list);
    int& s = g_sel[g_list];
    if (n > 0) {
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) s = (s + n - 1) % n;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) s = (s + 1) % n;
        if (g_pad.Repeat(PB_LT)) s = s - 8 < 0 ? 0 : s - 8;
        if (g_pad.Repeat(PB_RT)) s = s + 8 >= n ? n - 1 : s + 8;
    }
    if (g_pad.Pressed(PB_DLEFT)) { g_list = (g_list + 2) % 3; g_textFor = -1; g_talkFor = -1; }
    if (g_pad.Pressed(PB_DRIGHT)) { g_list = (g_list + 1) % 3; g_textFor = -1; g_talkFor = -1; }
    if (g_pad.Pressed(PB_Y) && g_list < 2 && !QuestTalks().empty()) { g_view = true; g_talkFor = -1; }
    if (g_pad.Pressed(PB_X) && g_list == 0 && !g_q[0].empty()) {          // follow this quest (again: unfollow)
        const Quest& q = g_q[0][std::min(g_sel[0], (int)g_q[0].size() - 1)];
        if (Lower(q.tag) == Quest_Tracked()) Quest_Untrack(); else Quest_Track(q.tag);
    }
    if (fabsf(g_pad.ry) > 0.2f) {
        g_textScroll -= g_pad.ry * px(700.0f) * (float)g_pad.dt;
        g_talkScroll -= g_pad.ry * px(900.0f) * (float)g_pad.dt;
    }
    if (g_pad.Pressed(PB_A)) { g_textScroll = 0; g_talkScroll = 0; }
    if (g_pad.Pressed(PB_B)) Hub_Close();
}

bool QuestHasTalks(const std::string& title) {
    std::string n = Lower(title);
    for (auto& t : g_talks) for (auto& q : t.quests) if (Lower(q) == n) return true;
    return false;
}

// conversations as a transcript: a header per conversation (who, where), the NPC's lines, your replies
// indented; scrolled with the right stick, the d-pad jumps to the previous / next conversation
void DrawTalks(const std::vector<Talk>& v, float x, float y, float w, float h, float a) {
    const float fs = px(21), sp = 1.25f;
    const float indent = px(34);
    struct Blk { float top; };
    std::vector<float> tops;
    // measure
    float total = 0;
    for (size_t k = 0; k < v.size(); k++) {
        tops.push_back(total);
        total += px(36);                                     // header
        if (!v[k].quests.empty()) total += px(24);
        for (auto& l : v[k].lines) {
            std::string t = l.player || l.who == v[k].npc || l.who.empty() ? l.text : l.who + ": " + l.text;
            total += TextWrap(FONT_BODY, fs, 0, 0, w - px(20) - (l.player ? indent : 0), t.c_str(), Theme::Text, 0, sp, true) + px(10);
        }
        total += px(26);
    }
    float maxS = fmaxf(0.0f, total - h + px(10));
    if (g_jump) {                                            // to the previous / next conversation's header
        float cur = g_talkScroll, best = g_jump > 0 ? maxS : 0;
        for (float t : tops) {
            if (g_jump > 0 && t > cur + px(2)) { best = t; break; }
            if (g_jump < 0 && t < cur - px(2)) best = t;
        }
        g_talkScroll = best; g_jump = 0;
    }
    g_talkScroll = fminf(fmaxf(g_talkScroll, 0.0f), maxS);
    PushClip(x - px(4), y, w + px(8), h);
    float cy = y - g_talkScroll;
    for (size_t k = 0; k < v.size(); k++) {
        const Talk& t = v[k];
        RoundRect(x, cy + px(2), w - px(20), px(30), px(8), Col(60, 48, 30, (int)(110 * a)));
        Text(FONT_TITLE, px(21), x + px(12), cy + px(5), t.npc.empty() ? "(someone)" : t.npc.c_str(), Theme::Gold.alpha(a));
        if (!t.area.empty()) {
            float aw = TextWidth(FONT_BODY, px(16), t.area.c_str());
            Text(FONT_BODY, px(16), x + w - px(32) - aw, cy + px(9), t.area.c_str(), Theme::TextFaint.alpha(a));
        }
        cy += px(36);
        if (!t.quests.empty()) {
            std::string qs = "Journal: ";
            for (size_t i = 0; i < t.quests.size(); i++) qs += (i ? ", " : "") + t.quests[i];
            TextFit(FONT_BODY, px(16), x + px(12), cy, w - px(40), qs.c_str(), Theme::GoldDim.alpha(a));
            cy += px(24);
        }
        for (auto& l : t.lines) {
            std::string s = l.player || l.who == t.npc || l.who.empty() ? l.text : l.who + ": " + l.text;
            float lx = x + (l.player ? indent : px(4)), lw = w - px(20) - (l.player ? indent : 0);
            float lh = TextWrap(FONT_BODY, fs, 0, 0, lw, s.c_str(), Theme::Text, 0, sp, true);
            if (cy + lh > y - px(40) && cy < y + h + px(40)) {
                if (l.player) RoundRect(lx - px(12), cy + px(4), px(3), lh - px(6), px(1), Col(150, 185, 225, (int)(200 * a)));
                TextWrap(FONT_BODY, fs, lx, cy, lw, s.c_str(), (l.player ? Col(176, 200, 228) : Theme::Text).alpha(0.94f * a), 0, sp);
            }
            cy += lh + px(10);
        }
        cy += px(26);
    }
    PopClip();
    if (maxS > 0) {
        float t2 = h * h / total;
        RoundRect(x + w - px(6), y + (h - t2) * (g_talkScroll / maxS), px(4), t2, px(2), Theme::GoldDim.alpha(a));
    }
}

void ListTab(float x, float y, float w, const char* label, int count, bool on, float a) {
    char b[64]; snprintf(b, sizeof b, "%s  %d", label, count);
    RoundRect(x, y, w, px(44), px(10), on ? Theme::Sel1.alpha(0.8f * a) : Col(8, 7, 6, (int)(120 * a)));
    if (on) RoundRectBorder(x, y, w, px(44), px(10), px(1), Theme::LineStrong.alpha(0.7f * a));
    TextCentered(FONT_TITLE, px(21), x + w * 0.5f, y + px(9), b, (on ? Theme::Gold : Theme::TextFaint).alpha(a));
}

void Draw(float x, float y, float w, float h, float a) {
    Read();
    float lx = x + px(36), ly = y + px(18);
    float lw = fminf(px(560), w * 0.36f);
    // active / completed switch
    float tw = (lw - px(16)) / 3.0f;
    ListTab(lx, ly, tw, "Active", (int)g_q[0].size(), g_list == 0, a);
    ListTab(lx + tw + px(8), ly, tw, "Done", (int)g_q[1].size(), g_list == 1, a);
    ListTab(lx + (tw + px(8)) * 2, ly, tw, "Talks", (int)g_talks.size(), g_list == 2, a);
    ly += px(60);
    float lh = h - (ly - y) - px(20);
    RoundRect(lx, ly, lw, lh, px(12), Col(8, 7, 6, (int)(120 * a)));
    auto& v = g_q[g_list < 2 ? g_list : 0];
    int n = Count(g_list);
    int& s = g_sel[g_list];
    if (s >= n) s = n - 1;
    if (s < 0) s = 0;
    int& sc = g_scroll[g_list];
    float rowH = px(58);
    int vis = (int)(lh / rowH); if (vis < 1) vis = 1;
    if (s < sc) sc = s;
    if (s >= sc + vis) sc = s - vis + 1;
    if (sc > n - vis) sc = n - vis;
    if (sc < 0) sc = 0;
    PushClip(lx, ly, lw, lh);
    for (int i = sc; i < n && i < sc + vis + 1; i++) {
        float ry = ly + (i - sc) * rowH;
        bool sel = i == s;
        UIK::RowSel(lx + px(4), ry + px(3), lw - px(8), rowH - px(6), sel);
        float tx = lx + px(30);
        if (g_list == 2) {                                   // a conversation: who, where, which quests
            const Talk& t = g_talks[i];
            TextFit(FONT_BODY, px(22), lx + px(20), ry + px(5), lw - px(40), t.npc.empty() ? "(someone)" : t.npc.c_str(),
                    (sel ? Col(255, 244, 222) : Theme::Text).alpha(a));
            std::string sub = t.area;
            if (!t.quests.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + t.quests[0] + (t.quests.size() > 1 ? " ..." : "");
            TextFit(FONT_BODY, px(16), lx + px(20), ry + px(32), lw - px(40), sub.c_str(), Theme::TextFaint.alpha(a));
            continue;
        }
        if (QuestHasTalks(v[i].name)) Text(FONT_BODY, px(15), lx + lw - px(30), ry + rowH * 0.5f - px(9), "\xE2\x80\xA6", Theme::GoldDim.alpha(a));
        if (v[i].updated && g_list == 0) Circle(lx + px(17), ry + rowH * 0.5f, px(4), Theme::Gold.alpha(a));
        if (g_list == 0 && !v[i].tag.empty() && Lower(v[i].tag) == Quest_Tracked()) {        // on the minimap
            float mx = lx + px(17), my = ry + rowH * 0.5f, r = px(5.5f);
            Triangle(mx, my - r * 1.3f, mx + r, my, mx, my + r * 1.3f, Theme::Gold.alpha(a));
            Triangle(mx, my - r * 1.3f, mx - r, my, mx, my + r * 1.3f, Theme::Gold.alpha(a));
        }
        TextFit(FONT_BODY, px(23), tx, ry + (rowH - LineHeight(FONT_BODY, px(23))) * 0.5f, lw - px(50), v[i].name.c_str(),
                (sel ? Col(255, 244, 222) : g_list == 1 ? Theme::TextDim : Theme::Text).alpha(a));
    }
    if (n == 0)
        TextCentered(FONT_BODY, px(21), lx + lw * 0.5f, ly + px(30), g_list == 0 ? "No active quests" : g_list == 1 ? "No completed quests" : "No conversations yet", Theme::TextFaint.alpha(a));
    PopClip();
    UIK::Scrollbar(lx + lw - px(8), ly + px(6), lh - px(12), sc, vis, n);

    // entry
    float rx = lx + lw + px(50), rw = x + w - px(50) - rx;
    float ry = y + px(26);
    if (g_list == 2) {
        if (n == 0) {
            TextWrap(FONT_BODY, px(21), rx, ry + px(20), rw,
                     "Conversations are recorded here as you have them, and kept with your saves. Those that "
                     "moved a quest on are also listed with the quest (Y on the quest).", Theme::TextFaint.alpha(a), 0, 1.3f);
            return;
        }
        if (g_talkFor != 20000 + s) { g_talkFor = 20000 + s; g_talkScroll = 0; }
        std::vector<Talk> one(1, g_talks[s]);
        DrawTalks(one, rx, ry, rw, y + h - px(30) - ry, a);
        return;
    }
    if (n == 0) {
        TextWrap(FONT_BODY, px(21), rx, ry + px(20), rw,
                 "Quests you take on are recorded here. The newest entry of each quest is shown; "
                 "completed quests move to the second list.", Theme::TextFaint.alpha(a), 0, 1.3f);
        return;
    }
    const Quest& q = v[s];
    int key = g_list * 10000 + s;
    if (g_view) {
        if (g_talkFor != 30000 + key) { g_talkFor = 30000 + key; g_talkScroll = 0; }
        float th = TextWrap(FONT_TITLE, px(30), rx, ry, rw, q.name.c_str(), Theme::Gold.alpha(a));
        ry += th + px(4);
        Text(FONT_BODY_BOLD, px(18), rx, ry, "Conversations", Theme::TextDim.alpha(a));
        ry += px(34);
        DrawTalks(QuestTalks(), rx, ry, rw, y + h - px(30) - ry, a);
        return;
    }
    if (g_textFor != key) { g_textFor = key; g_textScroll = 0; }
    float th = TextWrap(FONT_TITLE, px(34), rx, ry, rw, q.name.c_str(), Theme::Gold.alpha(a));
    ry += th + px(10);
    int nt = (int)QuestTalks().size();
    if (nt > 0) {
        char b[64]; snprintf(b, sizeof b, "%d conversation%s", nt, nt == 1 ? "" : "s");
        float hw = UIK::HintWidth(GL_Y, b, px(26));
        UIK::Hint(GL_Y, b, rx + rw - hw, ry - px(4), px(26), a);
    }
    const char* status = q.completed || g_list == 1 ? "Completed" : q.updated ? "Updated" : "In progress";
    Col sc2 = q.completed || g_list == 1 ? Theme::Friendly : q.updated ? Theme::Gold : Theme::TextDim;
    Text(FONT_BODY_BOLD, px(19), rx, ry, status, sc2.alpha(a));
    ry += px(38);
    RoundRect(rx, ry, rw, px(1), 0, Theme::Line.alpha(0.4f * a));
    ry += px(22);
    float bh = y + h - px(30) - ry;
    PushClip(rx - px(4), ry, rw + px(8), bh);
    float full = TextWrap(FONT_BODY, px(22), 0, 0, rw - px(20), q.text.c_str(), Theme::Text, 0, 1.3f, true);
    float maxS = fmaxf(0.0f, full - bh + px(10));
    g_textScroll = fminf(fmaxf(g_textScroll, 0.0f), maxS);
    TextWrap(FONT_BODY, px(22), rx, ry - g_textScroll, rw - px(20), q.text.c_str(), Theme::Text.alpha(0.94f * a), 0, 1.3f);
    PopClip();
    if (maxS > 0) {
        float t2 = bh * bh / full;
        RoundRect(rx + rw - px(6), ry + (bh - t2) * (g_textScroll / maxS), px(4), t2, px(2), Theme::GoldDim.alpha(a));
    }
}

int Hints(HintItem* o) {
    int n = 0;
    if (g_view) {
        o[n++] = {GL_DPAD, "Previous / next"};
        o[n++] = {GL_RS, "Scroll"};
        o[n++] = {GL_B, "Back"};
        return n;
    }
    o[n++] = {GL_DPAD, "Choose"};
    o[n++] = {GL_DLEFT, g_list == 0 ? "Done" : g_list == 1 ? "Talks" : "Active"};
    if (g_list == 0) {
        bool cur = false;
        if (!g_q[0].empty()) { const Quest& q = g_q[0][std::min(g_sel[0], (int)g_q[0].size() - 1)]; cur = Lower(q.tag) == Quest_Tracked(); }
        o[n++] = {GL_X, cur ? "Unfollow" : "Follow"};
    }
    o[n++] = {GL_RS, "Scroll"};
    return n;
}
}  // namespace

const HubPage PAGE_JOURNAL = {Open, Input, Draw, Hints, Close};
