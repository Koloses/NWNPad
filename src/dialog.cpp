// NWNPad - gamepad conversation window (replaces the game's dialogue panel)
#include "dialog.h"
#include "uikit.h"
#include "game.h"
#include "gamedata.h"
#include "hooks.h"
#include "pad.h"
#include "config.h"
#include "text.h"
#include "log.h"
#include "ui.h"
#include <windows.h>
#include <vector>
#include <string>
#include <cmath>

using namespace G;
using namespace R;

namespace {
const uintptr_t VA_ShowDialogEntry = 0x1406f8bd0;
const uintptr_t VA_ShowDialogReplies = 0x1406f8f50;
const uintptr_t VA_HandleDialogSelection = 0x1406f5850;

void (*o_ShowDialogEntry)(void* gig, void* text, uint32_t p3, uint32_t speaker) = nullptr;
void (*o_ShowDialogReplies)(void* gig, int n, void* strs, uint32_t* ids, int p4, uint32_t p5, int p6) = nullptr;
typedef void (*PanelFn)(void*);
PanelFn fHide = nullptr, fUnHide = nullptr;

struct State {
    bool active = false;
    uint32_t speaker = 0;
    std::string name, text;
    std::vector<std::string> replies;
    int sel = 0;
    int scroll = 0;
    double openTime = 0, textTime = 0;
    bool hasReplies = false;
} DS;

void* GIG() { void* in = Internal(); return in ? F<void*>(in, OFS::In_GuiInGame) : nullptr; }
void* DlgPanel() { void* g = GIG(); return g ? F<void*>(g, 0x70) : nullptr; }

void HidePanel() {
    if (!g_cfg.newDialog || !UI_PadPresent()) return;
    void* p = DlgPanel();
    if (p && fHide && (F<uint32_t>(p, 0xb0) & 2) == 0) fHide(p);
}

// the classic conversation panel must never reach the screen while the pad window is up, not even for a
// frame (a one-frame flash of it reads as the screen blinking): keep it hidden when the game shows it again
PanelFn o_UnHide = nullptr;
void hk_UnHide(void* p) {
    if (p && p == DlgPanel() && g_cfg.newDialog && UI_PadPresent()) {
        static double last = -10;
        if (g_pad.now - last > 2.0) { Log("dialog: game tried to show its panel, kept hidden"); last = g_pad.now; }
        return;
    }
    o_UnHide(p);
}

void hk_ShowDialogEntry(void* gig, void* text, uint32_t p3, uint32_t speaker) {
    DS.text = ExoStr(text);
    DS.speaker = speaker;
    DS.name = GD::ObjName(speaker);
    DS.replies.clear();
    DS.hasReplies = false;
    DS.sel = 0; DS.scroll = 0;
    if (!DS.active) DS.openTime = g_pad.now;
    DS.textTime = g_pad.now;
    DS.active = true;
    Log("dialog entry from %08x '%s'", speaker, DS.name.c_str());
    o_ShowDialogEntry(gig, text, p3, speaker);
    HidePanel();
}

void hk_ShowDialogReplies(void* gig, int n, void* strs, uint32_t* ids, int p4, uint32_t p5, int p6) {
    DS.replies.clear();
    for (int i = 0; i < n && i < 64; i++) DS.replies.push_back(ExoStr((char*)strs + i * 16));
    DS.hasReplies = true;
    DS.sel = 0; DS.scroll = 0;
    Log("dialog replies %d (p4=%d p5=%u p6=%d)", n, p4, p5, p6);
    o_ShowDialogReplies(gig, n, strs, ids, p4, p5, p6);
    HidePanel();
}

void Select(int idx) {
    void* g = GIG();
    if (!g || !DlgPanel()) return;
    Log("dialog select %d", idx);
    typedef void (*fn)(void*, int);
    ((fn)A(VA_HandleDialogSelection))(g, idx);
}
}  // namespace

void Dialog_InstallHooks() {
    o_ShowDialogEntry = (decltype(o_ShowDialogEntry))Hook_Create(VA_ShowDialogEntry, (void*)hk_ShowDialogEntry, "ShowDialogEntry");
    o_ShowDialogReplies = (decltype(o_ShowDialogReplies))Hook_Create(VA_ShowDialogReplies, (void*)hk_ShowDialogReplies, "ShowDialogReplies");
    fHide = (PanelFn)GetProcAddress(GetModuleHandleA(nullptr), "?Hide@CGuiPanel@@QEAAXXZ");
    fUnHide = (PanelFn)GetProcAddress(GetModuleHandleA(nullptr), "?UnHide@CGuiPanel@@QEAAXXZ");
    if (fUnHide) {
        uintptr_t va = (uintptr_t)fUnHide - (uintptr_t)GetModuleHandleA(nullptr) + 0x140000000ull;
        o_UnHide = (PanelFn)Hook_Create(va, (void*)hk_UnHide, "CGuiPanel::UnHide");
        if (o_UnHide) fUnHide = o_UnHide;      // our own unhide (pad gone) goes straight to the game
    }
}

void Dialog_OnPanelActivated(void* p) {     // CGuiPanel::Activate hook (guiscan)
    if (p && p == DlgPanel()) HidePanel();
}

bool Dialog_Active() {
    if (!g_cfg.newDialog) return false;
    static double goneSince = -1;
    if (DlgPanel()) goneSince = -1;
    if (DS.active && !DlgPanel()) {
        // the game may drop and recreate the panel between lines: only close after a short absence
        if (goneSince < 0) goneSince = g_pad.now;
        if (g_pad.now - goneSince > 0.3) { DS.active = false; goneSince = -1; Log("dialog closed"); }
    }
    if (DS.active && !UI_PadPresent()) {
        // pad gone: give the conversation back to the game's own panel
        void* p = DlgPanel();
        if (p && fUnHide && (F<uint32_t>(p, 0xb0) & 2) != 0) fUnHide(p);
        return false;
    }
    return DS.active;
}
void* Dialog_Panel() { return DlgPanel(); }

static int ReplyCount() { return DS.hasReplies ? (int)DS.replies.size() : 0; }

bool Dialog_Input() {
    if (!Dialog_Active()) return false;
    HidePanel();
    int n = ReplyCount();
    if (n > 0) {
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) DS.sel = (DS.sel + n - 1) % n;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) DS.sel = (DS.sel + 1) % n;
        static int lastSel = -1;
        if (DS.sel != lastSel) { lastSel = DS.sel; Log("dialog highlight %d", DS.sel); }
        if (g_pad.Pressed(PB_A)) Select(DS.sel);
    } else if (g_pad.Pressed(PB_A)) {
        Select(-3);
    }
    if (g_pad.Pressed(PB_B)) Select(-3);
    return true;
}

void Dialog_Draw() {
    if (!Dialog_Active()) return;
    float t = (float)fmin(1.0, (g_pad.now - DS.openTime) / 0.18);
    float ease = 1 - (1 - t) * (1 - t);
    float w = fminf(px(1280), W() - px(80));
    float x = (W() - w) * 0.5f;
    float port = px(150);
    float pad = px(28);
    float textX = x + pad + port + px(26);
    float textW = w - (textX - x) - pad;
    float fsText = px(27), fsReply = px(25);
    // measure
    float textH = TextWrap(FONT_BODY, fsText, 0, 0, textW, DS.text.c_str(), Theme::Text, 0, 1.18f, true);
    int n = ReplyCount();
    std::vector<float> rh(n);
    float repliesH = 0;
    for (int i = 0; i < n; i++) {
        rh[i] = TextWrap(FONT_BODY, fsReply, 0, 0, textW - px(56), DS.replies[i].c_str(), Theme::Text, 0, 1.12f, true) + px(14);
        repliesH += rh[i] + px(4);
    }
    if (n == 0) repliesH = px(46);
    float maxReplies = H() * 0.36f;
    float shownReplies = fminf(repliesH, maxReplies);
    float h = pad + px(40) + textH + px(22) + shownReplies + pad;
    if (h < port + pad * 2 + px(20)) h = port + pad * 2 + px(20);
    float y = H() - h - px(96) + (1 - ease) * px(40);
    float a = ease;
    UIK::Panel(x, y, w, h, px(18), a);
    // portrait
    void* sp = GetObj(DS.speaker);
    uint32_t tex = sp ? GD::PortraitTex(sp, 'm') : 0;
    float py = y + pad;
    if (tex) {
        float ph = fminf(port * 1.56f, h - pad * 2);
        ImageRounded(tex, x + pad, py, port, ph, px(12), Col(255, 255, 255, (int)(255 * a)), 0, 0, 1, 0.78f * (ph / (port * 1.56f)));
        RoundRectBorder(x + pad, py, port, port * 1.6f > h - pad * 2 ? h - pad * 2 : port * 1.6f, px(12), px(1.5f), Theme::Line.alpha(a));
    } else {
        RoundRect(x + pad, py, port, port, px(12), Col(40, 34, 28, (int)(220 * a)));
        TextCentered(FONT_TITLE, px(64), x + pad + port * 0.5f, py + port * 0.2f, DS.name.empty() ? "?" : DS.name.substr(0, 1).c_str(), Theme::GoldDim.alpha(a));
    }
    // name + text
    Text(FONT_TITLE, px(30), textX, y + pad - px(4), DS.name.c_str(), Theme::Gold.alpha(a));
    float ty = y + pad + px(40);
    // reveal text over time (typewriter, fast)
    TextWrap(FONT_BODY, fsText, textX, ty, textW, DS.text.c_str(), Theme::Text.alpha(a), 0, 1.18f);
    ty += textH + px(10);
    RoundRect(textX, ty, textW, px(1), 0, Theme::Line.alpha(0.6f * a));
    ty += px(12);
    // replies
    PushClip(textX - px(8), ty - px(2), textW + px(16), shownReplies + px(6));
    // keep selection visible
    float selTop = 0;
    for (int i = 0; i < DS.sel && i < n; i++) selTop += rh[i] + px(4);
    float offset = 0;
    if (n && selTop + rh[DS.sel] > shownReplies) offset = selTop + rh[DS.sel] - shownReplies;
    float ry = ty - offset;
    for (int i = 0; i < n; i++) {
        bool sel = i == DS.sel;
        UIK::RowSel(textX - px(6), ry, textW + px(12), rh[i], sel);
        char num[8]; snprintf(num, sizeof num, "%d", i + 1);
        if (sel) UIK::Glyph(GL_A, textX + px(4), ry + px(6), px(28), a);
        else TextCentered(FONT_BODY_BOLD, px(20), textX + px(18), ry + px(9), num, Theme::TextFaint.alpha(a));
        TextWrap(FONT_BODY, fsReply, textX + px(48), ry + px(7), textW - px(56), DS.replies[i].c_str(),
                 sel ? Col(255, 244, 222).alpha(a) : Theme::TextDim.alpha(a), 0, 1.12f);
        ry += rh[i] + px(4);
    }
    if (n == 0) {
        UIK::Hint(GL_A, DS.hasReplies ? "End conversation" : "Continue", textX, ty + px(6), px(30), a);
    }
    PopClip();
    // hints
    HintItem hints[3]; int hn = 0;
    if (n > 0) { hints[hn++] = {GL_DPAD, "Choose"}; hints[hn++] = {GL_A, "Reply"}; }
    hints[hn++] = {GL_B, "Leave"};
    UI_DrawHints(hints, hn, a);
}
