// NWNPad - gamepad version of the game's death screen.
//
// PopUpDeathGUIPanel makes the client create a CPanelDeathMenu (CGuiInGame +0xe8) with four buttons:
// Load game (+0x1a8), Respawn (+0x278), Exit (+0x348) and Wait for help (+0x418). A script can switch
// Respawn / Wait off (the button gets the "disabled" animation); Wait for help only exists online. We show
// the same choices and call the buttons' own handlers, so the server sees exactly what a click would do.
// Load opens the hub's System page with the save list; Exit asks first (the game's OkCancel, which the
// pad pop-up handles).
#include "death.h"
#include "ui.h"
#include "uikit.h"
#include "render.h"
#include "game.h"
#include "hub.h"
#include "popup.h"
#include "pad.h"
#include "log.h"
#include "guiscan.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace G;
using namespace R;

void HubSystem_ShowLoad();

namespace {
const size_t GIG_Death = 0xe8;
const size_t B_Load = 0x1a8, B_Respawn = 0x278, B_Exit = 0x348, B_Wait = 0x418;
const uintptr_t H_Load = 0x140885580, H_Respawn = 0x1408856d0, H_Exit = 0x140885470, H_Wait = 0x140885970;
const uintptr_t H_StopWaiting = 0x140885390;            // CPanelDeathMenu::HandleAnyInputEvent
const size_t D_Waiting = 0x100;                          // CGuiPressAnyKeyControl while waiting for help
const size_t D_Message = 0x110;                          // label with the module's message (PopUpDeathGUIPanel)

struct Choice { const char* label; const char* sub; uintptr_t handler; size_t button; };
int g_sel = 0;
double g_shown = -10;
void* g_lastPanel = nullptr;

void* GuiMan() {
    static void** p = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?g_pGuiMan@@3PEAVCGuiMan@@EA");
    return p ? *p : nullptr;
}
void* TopModal() {
    void* gm = GuiMan();
    if (!gm) return nullptr;
    int n = F<int>(gm, 0x90);
    void** arr = F<void**>(gm, 0x88);
    return (n > 0 && arr) ? arr[n - 1] : nullptr;
}
void* Panel() {
    void* in = Internal();
    void* gig = in ? F<void*>(in, OFS::In_GuiInGame) : nullptr;
    void* p = gig ? F<void*>(gig, GIG_Death) : nullptr;
    return p && Readable(p, 0x4b8) ? p : nullptr;
}
bool Online() {
    static void** am = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?g_pAppManager@@3PEAVCAppManager@@EA");
    return am && *am && F<int>(*am, 0x28) != 0;
}
// a button the game switched off shows its "disabled" animation
bool Usable(void* p, size_t b) {
    const char* anim = F<const char*>((char*)p + b, 0xc0);
    return !(anim && Readable(anim, 9) && strcmp(anim, "disabled") == 0);
}
std::vector<Choice> Choices(void* p) {
    std::vector<Choice> v;
    if (Usable(p, B_Respawn)) v.push_back({"Respawn", "Come back to life (the module decides the price)", H_Respawn, B_Respawn});
    if (Online() && Usable(p, B_Wait)) v.push_back({"Wait for help", "Stay down until someone raises you", H_Wait, B_Wait});
    if (!Online() && Usable(p, B_Load)) v.push_back({"Load game", "Pick a saved game", H_Load, B_Load});
    v.push_back({"Exit to main menu", "Leave the game", H_Exit, B_Exit});
    return v;
}
bool Waiting(void* p) { return F<void*>(p, D_Waiting) != nullptr; }
}  // namespace

bool Death_KeepHidden(void* panel) { return panel && panel == Panel() && UI_PadPresent(); }

bool Death_Active() {
    void* p = Panel();
    if (!p || !UI_PadPresent() || Hub_Open()) return false;
    if (p != g_lastPanel) { g_lastPanel = p; g_shown = g_pad.now; g_sel = 0; Log("death screen"); }
    return Waiting(p) || TopModal() == p;
}

bool Death_Input() {
    if (!Death_Active()) return false;
    void* p = Panel();
    if (Waiting(p)) {
        if (g_pad.Pressed(PB_A) || g_pad.Pressed(PB_B)) { Log("death: stop waiting"); ((void (*)(void*))A(H_StopWaiting))(p); }
        return true;
    }
    auto v = Choices(p);
    int n = (int)v.size();
    if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) g_sel = (g_sel + n - 1) % n;
    if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) g_sel = (g_sel + 1) % n;
    if (g_sel >= n) g_sel = n - 1;
    if (g_pad.Pressed(PB_A) && g_sel >= 0) {
        const Choice& c = v[g_sel];
        Log("death: %s", c.label);
        if (c.handler == H_Load) HubSystem_ShowLoad();            // our save list; the panel stays until a load
        else ((void (*)(void*))A(c.handler))(p);
    }
    return true;                                                  // nothing else happens while dead
}

void Death_Draw() {
    if (!Death_Active()) return;
    void* p = Panel();
    float a = fminf(1.0f, (float)(g_pad.now - g_shown) * 2.0f);
    RoundRect(0, 0, (float)W(), (float)H(), 0, Col(30, 0, 0, (int)(110 * a)));
    float w = px(560), x = (W() - w) * 0.5f;
    if (Waiting(p)) {
        float h = px(170), y = H() * 0.55f;
        UIK::Panel(x, y, w, h, px(16), a);
        TextCentered(FONT_TITLE, px(30), x + w * 0.5f, y + px(26), "Waiting for help", Theme::Gold.alpha(a));
        TextCentered(FONT_BODY, px(19), x + w * 0.5f, y + px(74), "Someone has to raise you.", Theme::TextDim.alpha(a));
        float hw = UIK::HintWidth(GL_A, "Back to the choices", px(26));
        UIK::Hint(GL_A, "Back to the choices", x + (w - hw) * 0.5f, y + h - px(46), px(26), a);
        return;
    }
    auto v = Choices(p);
    std::string msg = Gui_Text((char*)p + D_Message);
    float tw = w - px(80);
    float mh = msg.empty() ? 0 : TextWrap(FONT_BODY, px(19), 0, 0, tw, msg.c_str(), Theme::Text, 1, 1.15f, true) + px(18);
    float row = px(74), h = px(110) + mh + row * v.size() + px(30);
    float y = (H() - h) * 0.5f;
    UIK::Panel(x, y, w, h, px(16), a);
    TextCentered(FONT_TITLE, px(40), x + w * 0.5f, y + px(26), "You have died", Col(220, 70, 60).alpha(a));
    if (!msg.empty()) TextWrap(FONT_BODY, px(19), x + px(40), y + px(100), tw, msg.c_str(), Theme::Text.alpha(a), 1);
    float ry = y + px(110) + mh;
    for (int i = 0; i < (int)v.size(); i++) {
        bool sel = i == g_sel;
        float rx = x + px(40), rw = w - px(80);
        if (sel) RoundRect(rx, ry, rw, row - px(10), px(10), Theme::Sel0.alpha(a));
        else RoundRectBorder(rx, ry, rw, row - px(10), px(10), px(1), Theme::Line.alpha(0.5f * a));
        Text(FONT_TITLE, px(25), rx + px(22), ry + px(8), v[i].label, (sel ? Col(255, 240, 210) : Theme::Text).alpha(a));
        Text(FONT_BODY, px(17), rx + px(22), ry + px(38), v[i].sub, Theme::TextDim.alpha(a));
        ry += row;
    }
    HintItem hints[] = {{GL_DPAD, "Choose"}, {GL_A, "Select"}};
    UI_DrawHints(hints, 2, a);
}
