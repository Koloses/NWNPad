// NWNPad - the game's message / confirmation pop-ups (CPanelOk, CPanelOkCancel, CPanelOkScroll,
// CPanelYesNoCancel) shown as gamepad dialogs: the texts the panel sets on activation are captured,
// the buttons are pressed through the panel's own handlers.
#include "popup.h"
#include "guiscan.h"
#include "ui.h"
#include "uikit.h"
#include "render.h"
#include "game.h"
#include "hooks.h"
#include "text.h"
#include "pad.h"
#include "log.h"
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstring>
#include <cmath>

using namespace G;
using namespace R;

extern void (*g_textSink)(void* gob, const char* s, bool reset);

namespace {
typedef void (*PanelFn)(void*);
PanelFn o_Activate = nullptr;
PanelFn fHide = nullptr;

enum Kind { K_NONE, K_OK, K_OKCANCEL, K_OKSCROLL, K_YESNOCANCEL };
struct Pop {
    void* panel = nullptr;
    Kind kind = K_NONE;
    std::string message;
    std::vector<std::string> buttons;    // in on-screen order
    double time = 0;
    bool ready = false;
};
Pop g_pop;
void* g_pending = nullptr;               // activated this frame, texts still arriving
std::map<void*, std::string> g_texts;
double g_shownAt = 0;
PanelFn fOk = nullptr, fOkCancelOk = nullptr, fOkCancelCancel = nullptr, fScrollOk = nullptr, fYes = nullptr, fNo = nullptr, fCancel = nullptr;

void Sink(void* gob, const char* s, bool reset) {
    std::string& t = g_texts[gob];
    if (reset) t.clear();
    t += G2UClean(s);
}

Kind KindOf(void* p) {
    const char* n = ClassName(p);
    if (!strcmp(n, "CPanelOk")) return K_OK;
    if (!strcmp(n, "CPanelOkCancel")) return K_OKCANCEL;
    if (!strcmp(n, "CPanelOkScroll")) return K_OKSCROLL;
    if (!strcmp(n, "CPanelYesNoCancel")) return K_YESNOCANCEL;
    return K_NONE;
}

void hk_Activate(void* panel) {
    o_Activate(panel);
    if (KindOf(panel) == K_NONE) return;
    g_pending = panel;
    g_texts.clear();
    g_textSink = Sink;
    if (UI_PadPresent()) Gui_HidePanel(panel);
}

void* GuiMan() {
    static void** p = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?g_pGuiMan@@3PEAVCGuiMan@@EA");
    return p ? *p : nullptr;
}
bool IsModal(void* panel) {
    void* gm = GuiMan();
    if (!gm) return false;
    int n = F<int>(gm, 0x90);
    void** arr = F<void**>(gm, 0x88);
    for (int i = 0; arr && i < n && i < 64; i++) if (arr[i] == panel) return true;
    return false;
}
bool IsTop(void* panel) {
    void* gm = GuiMan();
    if (!gm) return false;
    int n = F<int>(gm, 0x90);
    void** arr = F<void**>(gm, 0x88);
    return n > 0 && arr && arr[n - 1] == panel;
}

// texts set by the panel: find which of its own fields point at each text object
void Finish() {
    void* p = g_pending;
    g_pending = nullptr;
    g_textSink = nullptr;
    if (!p || !IsModal(p)) return;
    struct T { size_t off; std::string s; };
    std::vector<T> found;
    for (auto& kv : g_texts) {
        if (kv.second.empty()) continue;
        for (size_t off = 8; off < 0x600; off += 8) {
            if (!Readable((char*)p + off, 8)) break;
            if (F<void*>(p, off) == kv.first) { found.push_back({off, kv.second}); break; }
        }
    }
    g_texts.clear();
    if (found.empty()) { Log("popup: %s - no texts found", ClassName(p)); return; }
    std::sort(found.begin(), found.end(), [](const T& a, const T& b) { return a.off < b.off; });
    // the message is the longest text; the others are the button labels in field order
    size_t mi = 0;
    for (size_t i = 1; i < found.size(); i++) if (found[i].s.size() > found[mi].s.size()) mi = i;
    Pop q;
    q.panel = p; q.kind = KindOf(p); q.message = found[mi].s; q.time = g_pad.now; q.ready = true;
    for (size_t i = 0; i < found.size(); i++) if (i != mi) q.buttons.push_back(found[i].s);
    g_pop = q;
    g_shownAt = g_pad.now;
    Log("popup: %s '%.60s' (%d buttons)", ClassName(p), q.message.c_str(), (int)q.buttons.size());
}

void Press(int which) {     // 0 = confirm (A), 1 = second (X: "No"), 2 = cancel (B)
    void* p = g_pop.panel;
    g_pop = Pop();
    if (!p || !IsModal(p)) return;
    Log("popup: press %d", which);
    switch (KindOf(p)) {
    case K_OK: if (fOk) fOk(p); break;
    case K_OKSCROLL: if (fScrollOk) fScrollOk(p); break;
    case K_OKCANCEL: if (which == 0 && fOkCancelOk) fOkCancelOk(p); else if (fOkCancelCancel) fOkCancelCancel(p); break;
    case K_YESNOCANCEL:
        if (which == 0 && fYes) fYes(p);
        else if (which == 1 && fNo) fNo(p);
        else if (fCancel) fCancel(p);
        break;
    default: break;
    }
}
}  // namespace

void Popup_InstallHooks() {
    HMODULE exe = GetModuleHandleA(nullptr);
    auto P = [&](const char* n) { return (PanelFn)GetProcAddress(exe, n); };
    fHide = P("?Hide@CGuiPanel@@QEAAXXZ");
    fOk = P("?OnOkButtonPressed@CPanelOk@@MEAAXXZ");
    fOkCancelOk = P("?OnOkButtonPressed@CPanelOkCancel@@IEAAXXZ");
    fOkCancelCancel = P("?OnCancelButtonPressed@CPanelOkCancel@@IEAAXXZ");
    fScrollOk = P("?OnOkButtonPressed@CPanelOkScroll@@IEAAXXZ");
    fYes = P("?HandleYesButton@CPanelYesNoCancel@@IEAAXXZ");
    fNo = P("?HandleNoButton@CPanelYesNoCancel@@IEAAXXZ");
    fCancel = P("?HandleCancelButton@CPanelYesNoCancel@@IEAAXXZ");
    void* a = (void*)GetProcAddress(exe, "?Activate@CGuiModalPanel@@QEAAXXZ");
    if (a) o_Activate = (PanelFn)Hook_Create((uintptr_t)a - G::base + 0x140000000ULL, (void*)hk_Activate, "CGuiModalPanel::Activate");
}

void Popup_Frame() {
    if (g_pending) Finish();
    if (!UI_PadPresent() && g_pop.panel && IsModal(g_pop.panel)) {        // mouse user: give the panel back
        Gui_ShowPanel(g_pop.panel);
        g_pop = Pop();
    }
    if (g_pop.panel && !IsModal(g_pop.panel)) g_pop = Pop();     // closed by the game
}

bool Popup_Active() { return g_pop.panel != nullptr && IsTop(g_pop.panel); }
bool Popup_Handled(void* panel) { return panel && panel == g_pop.panel; }
bool Popup_KeepHidden(void* p) { return p && UI_PadPresent() && (p == g_pop.panel || p == g_pending); }

bool Popup_Input() {
    if (!Popup_Active()) return false;
    if (fHide && (F<uint32_t>(g_pop.panel, 0xb0) & 2) == 0) fHide(g_pop.panel);
    if (g_pad.now - g_shownAt < 0.25) return true;            // ignore the press that opened it
    if (g_pad.Pressed(PB_A)) Press(0);
    else if (g_pad.Pressed(PB_X) && g_pop.kind == K_YESNOCANCEL) Press(1);
    else if (g_pad.Pressed(PB_B) && g_pop.kind != K_OK && g_pop.kind != K_OKSCROLL) Press(2);
    else if (g_pad.Pressed(PB_B)) Press(0);
    return true;
}

void Popup_Draw() {
    if (!Popup_Active()) return;
    float t = (float)fmin(1.0, (g_pad.now - g_shownAt) / 0.14);
    float a = 1 - (1 - t) * (1 - t);
    R::Rect(0, 0, (float)W(), (float)H(), Col(0, 0, 0, (int)(120 * a)));
    float w = fminf(px(720), W() - px(80));
    float tw = w - px(80);
    float th = TextWrap(FONT_BODY, px(23), 0, 0, tw, g_pop.message.c_str(), Theme::Text, 1, 1.3f, true);
    th = fminf(th, H() * 0.55f);
    float h = px(60) + th + px(110);
    float x = (W() - w) * 0.5f, y = (H() - h) * 0.5f - px(20);
    UIK::Panel(x, y, w, h, px(18), a);
    PushClip(x + px(30), y + px(40), w - px(60), th + px(4));
    TextWrap(FONT_BODY, px(23), x + px(40), y + px(44), tw, g_pop.message.c_str(), Theme::Text.alpha(a), 1, 1.3f);
    PopClip();
    // buttons
    std::vector<std::pair<PadGlyph, std::string>> b;
    auto lab = [&](size_t i, const char* def) { return i < g_pop.buttons.size() && !g_pop.buttons[i].empty() ? g_pop.buttons[i] : std::string(def); };
    switch (g_pop.kind) {
    case K_OK: case K_OKSCROLL: b.push_back({GL_A, lab(0, "OK")}); break;
    case K_OKCANCEL: b.push_back({GL_A, lab(0, "OK")}); b.push_back({GL_B, lab(1, "Cancel")}); break;
    case K_YESNOCANCEL: b.push_back({GL_A, lab(0, "Yes")}); b.push_back({GL_X, lab(1, "No")}); b.push_back({GL_B, lab(2, "Cancel")}); break;
    default: break;
    }
    float total = 0;
    for (auto& e : b) total += UIK::HintWidth(e.first, e.second.c_str(), px(30)) + px(40);
    float bx = x + (w - total + px(40)) * 0.5f, by = y + h - px(62);
    RoundRect(x + px(30), by - px(18), w - px(60), px(1), 0, Theme::Line.alpha(0.4f * a));
    for (auto& e : b) bx += UIK::Hint(e.first, e.second.c_str(), bx, by, px(30), a) + px(40);
}
