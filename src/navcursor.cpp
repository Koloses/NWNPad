// NWNPad - gamepad cursor for the game's own panels (main menu, character creation, level-up,
// options, stores, ...): left stick moves a magnetic cursor, the d-pad jumps between controls.
#include "navcursor.h"
#include "game.h"
#include "hooks.h"
#include "pad.h"
#include "config.h"
#include "log.h"
#include "render.h"
#include "control.h"
#include "uikit.h"
#include <windows.h>
#include <cmath>
#include <cstring>

using namespace G;

namespace {
void** p_gm = nullptr;
typedef void (*SetMousePos_t)(void*, int, int);
typedef int (*MouseWheel_t)(void*, int);
typedef void (*ShowHide_t)(void*);
SetMousePos_t fSetMousePos; MouseWheel_t fWheel; ShowHide_t fShowMouse, fHideMouse;
float g_cx = -1, g_cy = -1;       // cursor in GUI coords (y up)
bool g_active = false;
bool g_mouseHidden = false;
double g_wheelAcc = 0;
bool g_aDown = false, g_xDown = false;

void* GM() { return p_gm ? *p_gm : nullptr; }
int GW() { void* g = GM(); return g ? F<int>(g, 0xb8) : 1; }
int GH() { void* g = GM(); return g ? F<int>(g, 0xbc) : 1; }

bool IsPanel(void* o) {
    const char* n = ClassName(o);
    return strstr(n, "Panel") != nullptr || strcmp(n, "CGuiInGame") == 0;
}
void* HitAt(float x, float y) {
    void* g = GM();
    if (!g || !o_GuiObjectHitCheck) return nullptr;
    void* r = o_GuiObjectHitCheck(g, (int)x, (int)y, 1, 1);
    if (r && IsPanel(r)) return nullptr;     // panel backgrounds are not controls
    return r;
}
void ClearHitCache() { void* g = GM(); if (g) F<void*>(g, 0x78) = nullptr; }

void MoveMouse(float x, float y) {
    void* g = GM();
    if (!g || !fSetMousePos) return;
    int w = GW(), h = GH();
    x = x < 0 ? 0 : (x > w - 1 ? (float)(w - 1) : x);
    y = y < 0 ? 0 : (y > h - 1 ? (float)(h - 1) : y);
    g_cx = x; g_cy = y;
    fSetMousePos(g, (int)x, (int)y);
}

// find the extent of object `obj` around (x,y) along one axis
float Extent(void* obj, float x, float y, float dx, float dy, float maxd) {
    float d = 0;
    for (float s = 4; s <= maxd; s += 4) {
        if (HitAt(x + dx * s, y + dy * s) != obj) break;
        d = s;
    }
    return d;
}

void CenterOn(void* obj, float x, float y) {
    float l = Extent(obj, x, y, -1, 0, 500), r = Extent(obj, x, y, 1, 0, 500);
    float d = Extent(obj, x, y, 0, -1, 300), u = Extent(obj, x, y, 0, 1, 300);
    float cx = x + (r - l) * 0.5f, cy = y + (u - d) * 0.5f;
    if (HitAt(cx, cy) != obj) { cx = x; cy = y; }
    MoveMouse(cx, cy);
}

void Snap(int dirX, int dirY) {
    void* cur = HitAt(g_cx, g_cy);
    float best = 1e9f; float bx = 0, by = 0; void* bobj = nullptr;
    const float offs[] = {0, 12, -12, 28, -28, 56, -56, 110, -110, 200, -200};
    int maxs = (int)(dirX ? GW() : GH());
    for (float o : offs) {
        // start beyond the current object's border along the direction
        for (float s = 8; s < maxs; s += 8) {
            float x = g_cx + dirX * s + (dirY ? o : 0);
            float y = g_cy + dirY * s + (dirX ? o : 0);
            if (x < 0 || y < 0 || x >= GW() || y >= GH()) break;
            void* h = HitAt(x, y);
            if (h && h != cur) {
                float score = s + fabsf(o) * 2.2f;
                if (score < best) { best = score; bx = x; by = y; bobj = h; }
                break;
            }
            if (s + fabsf(o) * 2.2f > best) break;
        }
    }
    if (bobj) CenterOn(bobj, bx, by);
    ClearHitCache();
}

typedef int (*LDown_t)(void*, int);
typedef void (*Up_t)(void*);
typedef int (*RDown_t)(void*);
LDown_t fLDown = nullptr; Up_t fLUp = nullptr; RDown_t fRDown = nullptr; Up_t fRUp = nullptr;
// clicks go straight to the GUI manager (the in-game input path ignores them on the front-end screens)
void Click(int button, bool down) {
    void* g = GM();
    if (!g || !fLDown || !fLUp || !fRDown || !fRUp) { HandleInputEvent(button, down ? 1 : 0); return; }
    if (button == 0) { if (down) fLDown(g, 0); else fLUp(g); }
    else { if (down) fRDown(g); else fRUp(g); }
}
}  // namespace

void Cursor_Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    p_gm = (void**)GetProcAddress(exe, "?g_pGuiMan@@3PEAVCGuiMan@@EA");
    fSetMousePos = (SetMousePos_t)GetProcAddress(exe, "?SetMousePosition@CGuiMan@@QEAAXHH@Z");
    fWheel = (MouseWheel_t)GetProcAddress(exe, "?HandleMouseWheel@CGuiMan@@QEAAHH@Z");
    fShowMouse = (ShowHide_t)GetProcAddress(exe, "?ShowMouse@CGuiMan@@QEAAXXZ");
    fHideMouse = (ShowHide_t)GetProcAddress(exe, "?HideMouse@CGuiMan@@QEAAXXZ");
    fLDown = (LDown_t)GetProcAddress(exe, "?HandleLMouseDown@CGuiMan@@QEAAHH@Z");
    fLUp = (Up_t)GetProcAddress(exe, "?HandleLMouseUp@CGuiMan@@QEAAXXZ");
    fRDown = (RDown_t)GetProcAddress(exe, "?HandleRMouseDown@CGuiMan@@QEAAHXZ");
    fRUp = (Up_t)GetProcAddress(exe, "?HandleRMouseUp@CGuiMan@@QEAAXXZ");
    Log("cursor: setmouse %p wheel %p", (void*)fSetMousePos, (void*)fWheel);
}

bool Cursor_Active() { return g_active; }

void Cursor_SetMouseVisible(bool vis) {
    void* g = GM();
    if (!g) return;
    if (vis && g_mouseHidden && fShowMouse) { fShowMouse(g); g_mouseHidden = false; }
    if (!vis && !g_mouseHidden && fHideMouse) { fHideMouse(g); g_mouseHidden = true; }
}

// game panels that need the cursor are open?
bool Cursor_GameNeedsCursor(bool dialogHandled) {
    void* g = GM();
    if (!g) return false;
    void* cap = F<void*>(g, 0x58);
    if (cap) {
        uint32_t t = VCall<uint32_t (*)(void*)>(cap, 0x20)(cap);
        if (t == 0x524d6e75) return false;                       // radial menu: gameplay layer drives it
        if (t == 0x41635467 && !(Player() && F<int>(g, 0x90) > 0)) return false;   // action target (unless a modal panel is up)
    }
    if (!Player()) return true;                       // front-end menus, character creation, loading
    if (F<int>(g, 0x90) > 0) return true;             // modal panel
    void* in = Internal();
    void* gig = in ? F<void*>(in, OFS::In_GuiInGame) : nullptr;
    if (gig) {
        int n = F<int>(gig, 0x3c4);
        for (int i = 0; i < n && i < 3; i++) {
            void* p = F<void*>(gig, 0x3a0 + i * 8);
            if (!p) continue;
            if (dialogHandled && p == F<void*>(gig, 0x70)) continue;   // conversation handled by NWNPad
            extern bool UI_PanelHandled(void* panel);
            if (UI_PanelHandled(p)) continue;
            return true;
        }
    }
    return false;
}

void Cursor_Update(bool active) {
    if (active != g_active) {
        g_active = active;
        if (active) {
            int mx = 0, my = 0;
            void* g = GM();
            if (g) { mx = F<int>(g, 0xd4); my = F<int>(g, 0xd8); }
            g_cx = (float)mx; g_cy = (float)my;
            Cursor_SetMouseVisible(true);
        } else {
            if (g_aDown) { Click(0, false); g_aDown = false; }
            if (g_xDown) { Click(1, false); g_xDown = false; }
        }
    }
    if (!active) return;
    void* g = GM();
    if (!g) return;
    // follow the real mouse when it moves
    int mx = F<int>(g, 0xd4), my = F<int>(g, 0xd8);
    if (fabsf(mx - g_cx) > 2 || fabsf(my - g_cy) > 2) { g_cx = (float)mx; g_cy = (float)my; }

    float dt = (float)g_pad.dt;
    float lx = g_pad.lx, ly = g_pad.ly;
    float m = sqrtf(lx * lx + ly * ly);
    if (m > 0.01f) {
        float speed = GH() * 1.25f * (0.25f + 0.75f * m * m);
        void* over = HitAt(g_cx, g_cy);
        ClearHitCache();
        if (over && m < 0.9f) speed *= 0.55f;          // magnetism over controls
        MoveMouse(g_cx + lx * speed * dt, g_cy + ly * speed * dt);
    }
    if (g_pad.Repeat(PB_DUP, 0.5, 0.16)) Snap(0, 1);
    if (g_pad.Repeat(PB_DDOWN, 0.5, 0.16)) Snap(0, -1);
    if (g_pad.Repeat(PB_DLEFT, 0.5, 0.16)) Snap(-1, 0);
    if (g_pad.Repeat(PB_DRIGHT, 0.5, 0.16)) Snap(1, 0);
    // buttons
    if (g_pad.Pressed(PB_A)) { MoveMouse(g_cx, g_cy); Click(0, true); g_aDown = true; }
    if (g_pad.Released(PB_A) && g_aDown) { MoveMouse(g_cx, g_cy); Click(0, false); g_aDown = false; }
    if (g_pad.Pressed(PB_X)) { MoveMouse(g_cx, g_cy); Click(1, true); g_xDown = true; }
    if (g_pad.Released(PB_X) && g_xDown) { Click(1, false); g_xDown = false; }
    if (g_pad.Pressed(PB_B)) { HandleInputEvent(0x12, 1); HandleInputEvent(0x12, 0); }
    // right stick scrolls
    if (fabsf(g_pad.ry) > 0.2f && fWheel) {
        g_wheelAcc += g_pad.ry * dt * 12.0;
        while (g_wheelAcc >= 1) { fWheel(g, 1); g_wheelAcc -= 1; }
        while (g_wheelAcc <= -1) { fWheel(g, -1); g_wheelAcc += 1; }
    }
}

void Cursor_Draw() {
    if (!g_active) return;
    // cursor halo (GUI y-up -> screen y-down)
    float sx = g_cx * R::W() / (float)GW();
    float sy = R::H() - g_cy * R::H() / (float)GH();
    R::Ring(sx, sy, R::px(16), R::px(2.5f), Col(255, 214, 140, 200));
    R::Circle(sx, sy, R::px(4), Col(255, 230, 180, 230));
    extern bool Hub_ExternalActive();
    HintItem hints[6]; int n = 0;
    if (Hub_ExternalActive()) hints[n++] = {GL_LB, "Tabs"};
    hints[n++] = {GL_A, "Select"};
    hints[n++] = {GL_X, "Right-click"};
    hints[n++] = {GL_B, "Back"};
    hints[n++] = {GL_RS, "Scroll"};
    hints[n++] = {GL_DPAD, "Jump"};
    UI_DrawHints(hints, n, 0.9f);
}
