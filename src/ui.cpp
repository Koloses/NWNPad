// NWNPad UI core: overlay frame hook, input routing between screens
#include "ui.h"
#include <algorithm>
#include <cstdio>
#include "questmark.h"
#include "gldiag.h"
#include "render.h"
#include "uikit.h"
#include "control.h"
#include "navcursor.h"
#include "dialog.h"
#include "hud.h"
#include "hub.h"
#include "hub_pages.h"
#include "describe.h"
#include "quickbar.h"
#include "inv.h"
#include "loot.h"
#include "party.h"
#include "popup.h"
#include "guiscan.h"
#include "charpanel.h"
#include "store.h"
#include "rest.h"
#include "death.h"
#include "prof.h"
#include "talklog.h"
#include "game.h"
#include "gamedata.h"
#include "hooks.h"
#include "pad.h"
#include "config.h"
#include "log.h"
#include "res.h"
#include "MinHook.h"
#include <windows.h>
#include <GL/gl.h>

static BOOL(WINAPI* o_SwapBuffers)(HDC) = nullptr;

float UI_UserScale() { return g_cfg.uiScale > 0 ? g_cfg.uiScale : 1.0f; }
int Config_CodePage() { return g_cfg.codepage; }

bool UI_PanelHandled(void* panel) {
    if (panel && panel == Dialog_Panel() && Dialog_Active()) return true;
    if (panel && panel == Loot_Panel() && Loot_Active()) return true;
    if (Store_PanelHandled(panel)) return true;
    if (Rest_KeepHidden(panel)) return true;
    if (Death_KeepHidden(panel)) return true;
    if (Popup_Handled(panel)) return true;
    if (CharPanel_Handled(panel)) return true;
    return false;
}

// called from the game's input processing (before gameplay controls)
// the gamepad HUD replaces the game's HUD only while a pad is in use (short drop-outs ignored)
bool UI_PadPresent() {
    static double lastSeen = -100;
    if (g_pad.connected) lastSeen = g_pad.now;
    return g_pad.now - lastSeen < 5.0;
}

bool Control_UIWantsInput() {
    void* pl = G::Player();
    HUD_UpdatePanels(pl != nullptr && UI_PadPresent());
    if (!g_pad.connected) { Popup_Frame(); CharPanel_Frame(); Cursor_SetMouseVisible(true); return false; }
    HubSystem_Update();
    Popup_Frame();
    CharPanel_Frame();
    if (Rest_Input()) return true;
    if (Death_Input()) { Cursor_Update(false); return true; }
    if (Popup_Input()) { Cursor_Update(false); return true; }
    if (CharPanel_Input()) { Cursor_Update(false); return true; }
    if (Dialog_Input()) { Cursor_Update(false); return true; }
    bool needCursor = Cursor_GameNeedsCursor(Dialog_Active());
    Cursor_Update(needCursor);
    if (Hub_ExternalInput(Cursor_Active())) return true;
    if (Cursor_Active()) return true;
    if (Store_Input()) return true;
    if (Loot_Input()) return true;
    if (Hub_Input()) return true;
    if (Party_Input()) return true;
    // hide the mouse pointer while playing with the pad
    if (g_cfg.hideCursorWithPad) Cursor_SetMouseVisible(false);
    return false;
}

void Gui_EnforceHidden();

// frame rate: frames per second over the last half second, average and worst frame time over the last second
static struct { double last = 0, t[256] = {}; int n = 0, head = 0; float fps = 0, avg = 0, worst = 0; double shown = 0; } g_fps;
static void Fps_Tick() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    double now = (double)c.QuadPart / f.QuadPart;
    if (g_fps.last > 0) { g_fps.t[g_fps.head] = now - g_fps.last; g_fps.head = (g_fps.head + 1) & 255; if (g_fps.n < 256) g_fps.n++; }
    g_fps.last = now;
    if (now - g_fps.shown < 0.5) return;
    g_fps.shown = now;
    double sum = 0, worst = 0; int k = 0;
    for (int i = 1; i <= g_fps.n && sum < 1.0; i++) { double d = g_fps.t[(g_fps.head - i) & 255]; sum += d; worst = d > worst ? d : worst; k++; }
    g_fps.avg = k ? (float)(sum / k * 1000.0) : 0;
    g_fps.fps = sum > 0 ? (float)(k / sum) : 0;
    g_fps.worst = (float)(worst * 1000.0);
}
static void Fps_Draw() {
    using namespace R;
    char a[64], b[96];
    snprintf(a, sizeof a, "%.0f fps", g_fps.fps);
    snprintf(b, sizeof b, "%.1f ms avg   %.1f ms worst", g_fps.avg, g_fps.worst);
    const char* rn = Renderer();
    float x = px(16), y = px(12), sz = px(17), small = px(14);
    float w = std::max(TextWidth(FONT_BODY_BOLD, sz, a), std::max(TextWidth(FONT_BODY, small, b), TextWidth(FONT_BODY, small, rn))) + px(20);
    RoundRect(x, y, w, px(72), px(8), Col(0, 0, 0, 150));
    Col c = g_fps.fps >= 55 ? Col(120, 230, 120) : g_fps.fps >= 30 ? Col(240, 210, 90) : Col(240, 100, 80);
    Text(FONT_BODY_BOLD, sz, x + px(10), y + px(6), a, c);
    Text(FONT_BODY, small, x + px(10), y + px(30), b, Col(220, 214, 200));
    Text(FONT_BODY, small, x + px(10), y + px(50), rn, Col(170, 164, 150));
}

static void UI_Frame(HDC hdc) {
    Gui_EnforceHidden();
    HWND wnd = WindowFromDC(hdc);
    RECT rc; if (!wnd || !GetClientRect(wnd, &rc)) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (!R::Begin(w, h)) return;
    void* pl = G::Player();
    if (pl) {
        if (Hub_Open() && !UI_PadPresent()) Hub_Close();
        bool hub = Hub_Open() && !Cursor_Active();
        bool loot = Loot_Active() && !Cursor_Active();
        bool store = Store_Active() && !Cursor_Active();
        if (!Dialog_Active() && !Cursor_Active() && !hub && !loot && !store && !CharPanel_Active() && UI_PadPresent() && !Death_Active()) { HUD_Draw(); Party_Draw(); }
        Dialog_Draw();
        if (hub) Hub_Draw();
        if (loot) Loot_Draw();
        if (store) Store_Draw();
    }
    if (Rest_Active()) Rest_Draw();
    Death_Draw();
    TalkLog_Frame(Dialog_Panel() != nullptr);
    if (UI_PadPresent()) CharPanel_Draw();
    if (UI_PadPresent()) Popup_Draw();
    if (g_pad.connected) Cursor_Draw();
    bool menus = Hub_Open() || Loot_Active() || Store_Active();
    if (pl && g_pad.connected && !Cursor_Active() && !Dialog_Active() && !Death_Active() && (!menus || g_ctl.radialOpen)) {
        if (g_ctl.radialOpen) {
            HintItem h[] = {{GL_LS, "Choose"}, {GL_A, "Select"}, {GL_Y, "Back"}, {GL_B, "Close"}};
            UI_DrawHints(h, 4, 0.95f);
        } else if (g_ctl.giveItem) {
            char lbl[160]; snprintf(lbl, sizeof lbl, "Give %s", Control_GiveName());
            HintItem h[] = {{GL_LS, "Pointer"}, {GL_A, lbl}, {GL_RB, "Next"}, {GL_B, "Cancel"}};
            UI_DrawHints(h, 4, 0.95f);
        } else if (g_ctl.inActionTargetMode) {
            HintItem h[] = {{GL_LS, "Pointer"}, {GL_A, "Use on target"}, {GL_RB, "Next target"}, {GL_Y, "Self"}, {GL_B, "Cancel"}};
            UI_DrawHints(h, 5, 0.95f);
        }
    }
    if (g_cfg.showFps) Fps_Draw();
    { ProfScope ps(PS_GL); R::End(); }
}

static BOOL WINAPI hk_SwapBuffers(HDC hdc) {
    G::NewFrame();
    GLDiag_PreOverlay();
    Fps_Tick();
    Prof_Frame();
    Log_Tick();
    Res_NewFrame();
    if (g_cfg.enabled) { ProfScope ps(PS_UI); UI_Frame(hdc); }
    GLDiag_PostOverlay();
    return o_SwapBuffers(hdc);
}

void UI_Init(const char*) {
    Res_Init();
    GD::Init();
    Cursor_Init();
    Hub_Init();
    Loot_Init();
    Store_Init();
    CharPanel_Init();
    Quest_Init();
}

void UI_InstallHooks() {
    void* sb = (void*)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SwapBuffers");
    if (sb && MH_CreateHook(sb, (void*)hk_SwapBuffers, (void**)&o_SwapBuffers) == MH_OK && MH_EnableHook(sb) == MH_OK)
        Log("hooked SwapBuffers @%p", sb);
    else Log("SwapBuffers hook FAILED");
    Dialog_InstallHooks();
    HUD_InstallHooks();
    QB_InstallHooks();
    Inv::InstallHooks();
    Cursor_InstallHooks();
    Describe_InstallHooks();
    Popup_InstallHooks();
    Gui_InstallHooks();
    Rest_InstallHooks();
}

// one line of what the pad UI shows (diagnostics)
std::string Hub_DebugState();
std::string UI_DebugState() {
    char b[256];
    snprintf(b, sizeof b, "hub %s, dialog %d, charpanel %d, popup %d, held %04x", Hub_DebugState().c_str(), (int)Dialog_Active(),
             (int)CharPanel_Active(), (int)Popup_Active(), (unsigned)g_pad.held);
    return b;
}
