// NWNPad UI core: overlay frame hook, input routing between screens
#include "ui.h"
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
#include "loot.h"
#include "party.h"
#include "popup.h"
#include "store.h"
#include "game.h"
#include "gamedata.h"
#include "hooks.h"
#include "pad.h"
#include "config.h"
#include "log.h"
#include "res.h"
#include "MinHook.h"
#include <windows.h>

static BOOL(WINAPI* o_SwapBuffers)(HDC) = nullptr;

float UI_UserScale() { return g_cfg.uiScale > 0 ? g_cfg.uiScale : 1.0f; }
int Config_CodePage() { return g_cfg.codepage; }

bool UI_PanelHandled(void* panel) {
    if (panel && panel == Dialog_Panel() && Dialog_Active()) return true;
    if (panel && panel == Loot_Panel() && Loot_Active()) return true;
    if (Store_PanelHandled(panel)) return true;
    if (Popup_Handled(panel)) return true;
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
    if (!g_pad.connected) { Popup_Frame(); Cursor_SetMouseVisible(true); return false; }
    HubSystem_Update();
    Popup_Frame();
    if (Popup_Input()) { Cursor_Update(false); return true; }
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

static void UI_Frame(HDC hdc) {
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
        if (!Dialog_Active() && !Cursor_Active() && !hub && !loot && !store && UI_PadPresent()) { HUD_Draw(); Party_Draw(); }
        Dialog_Draw();
        if (hub) Hub_Draw();
        if (loot) Loot_Draw();
        if (store) Store_Draw();
    }
    if (g_pad.connected) Popup_Draw();
    if (g_pad.connected) Cursor_Draw();
    bool menus = Hub_Open() || Loot_Active() || Store_Active();
    if (pl && g_pad.connected && !Cursor_Active() && !Dialog_Active() && (!menus || g_ctl.radialOpen)) {
        if (g_ctl.radialOpen) {
            HintItem h[] = {{GL_LS, "Choose"}, {GL_A, "Select"}, {GL_Y, "Back"}, {GL_B, "Close"}};
            UI_DrawHints(h, 4, 0.95f);
        } else if (g_ctl.inActionTargetMode) {
            HintItem h[] = {{GL_A, "Use on target"}, {GL_RB, "Next target"}, {GL_Y, "Self"}, {GL_B, "Cancel"}};
            UI_DrawHints(h, 4, 0.95f);
        }
    }
    R::End();
}

static BOOL WINAPI hk_SwapBuffers(HDC hdc) {
    if (g_cfg.enabled) UI_Frame(hdc);
    return o_SwapBuffers(hdc);
}

void UI_Init(const char*) {
    Res_Init();
    GD::Init();
    Cursor_Init();
    Hub_Init();
    Loot_Init();
    Store_Init();
}

void UI_InstallHooks() {
    void* sb = (void*)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SwapBuffers");
    if (sb && MH_CreateHook(sb, (void*)hk_SwapBuffers, (void**)&o_SwapBuffers) == MH_OK && MH_EnableHook(sb) == MH_OK)
        Log("hooked SwapBuffers @%p", sb);
    else Log("SwapBuffers hook FAILED");
    Dialog_InstallHooks();
    HUD_InstallHooks();
    QB_InstallHooks();
    Cursor_InstallHooks();
    Describe_InstallHooks();
    Popup_InstallHooks();
}
