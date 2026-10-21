#include "hooks.h"
#include "game.h"
#include "log.h"
#include "control.h"
#include "MinHook.h"
#include <windows.h>
#include <cstring>

void (*o_ProcessInput)(void*) = nullptr;
void (*o_UpdateDriveMode)(void*) = nullptr;
int (*o_FHC_Hostile)(void*, void**, void*, int*, int) = nullptr;
int (*o_FHC_NonHostile)(void*, void**, void*, int*, int) = nullptr;
void* (*o_GuiObjectHitCheck)(void*, int, int, int, int) = nullptr;
void (*o_UpdateMouseCursor)(void*, int) = nullptr;
HitOverride g_hitOverride = {};

void* Hook_Create(uintptr_t va, void* detour, const char* name) {
    void* target = (void*)G::A(va);
    void* orig = nullptr;
    MH_STATUS s = MH_CreateHook(target, detour, &orig);
    if (s != MH_OK) { Log("hook %s @%p FAILED (%d)", name, target, s); return nullptr; }
    s = MH_EnableHook(target);
    if (s != MH_OK) { Log("enable %s FAILED (%d)", name, s); return nullptr; }
    Log("hooked %s @%p", name, target);
    return orig;
}

static void hk_ProcessInput(void* in) {
    Control_PreInput(in);
    o_ProcessInput(in);
    Control_PostInput(in);
}
static void hk_UpdateDriveMode(void* in) {
    Control_Drive(in, o_UpdateDriveMode);
}
static int applyOverride(void** obj, void* pos, int* walk) {
    HitOverride& h = g_hitOverride;
    if (obj) *obj = h.obj;
    if (pos) { float* p = (float*)pos; p[0] = h.x; p[1] = h.y; p[2] = h.z; }
    if (walk) *walk = 1;
    return h.code;
}
static int hk_FHC_Hostile(void* in, void** obj, void* pos, int* walk, int p4) {
    if (g_hitOverride.active) return applyOverride(obj, pos, walk);
    return o_FHC_Hostile(in, obj, pos, walk, p4);
}
static int hk_FHC_NonHostile(void* in, void** obj, void* pos, int* walk, int p4) {
    if (g_hitOverride.active) return applyOverride(obj, pos, walk);
    return o_FHC_NonHostile(in, obj, pos, walk, p4);
}
static void* hk_GuiObjectHitCheck(void* gm, int x, int y, int p3, int p4) {
    if (g_hitOverride.active) return nullptr;
    return o_GuiObjectHitCheck(gm, x, y, p3, p4);
}

// mouse-over feedback (hilite, name plate, cursor shape): while playing with the pad the
// "hovered" object is the gamepad soft target
static void hk_UpdateMouseCursor(void* in, int p) {
    extern bool Control_HoverOverride(void** obj);
    void* obj = nullptr;
    if (Control_HoverOverride(&obj)) {
        g_hitOverride.active = true; g_hitOverride.obj = obj; g_hitOverride.code = obj ? 2 : 0;
        if (obj) { float* q = (float*)((char*)obj + 0x38); g_hitOverride.x = q[0]; g_hitOverride.y = q[1]; g_hitOverride.z = q[2]; }
        o_UpdateMouseCursor(in, 1);
        g_hitOverride.active = false;
        return;
    }
    o_UpdateMouseCursor(in, p);
}

// expected first bytes of each function (sanity check that this is the supported build)
struct Sig { uintptr_t va; const char* name; unsigned char bytes[8]; };
#include "sigs.inc"

bool Hooks_Install() {
    for (auto& s : g_sigs) {
        if (memcmp((void*)G::A(s.va), s.bytes, 8) != 0) {
            Log("signature mismatch for %s at %p - unsupported game build, NWNPad disabled", s.name, (void*)G::A(s.va));
            return false;
        }
    }
    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) { Log("MH_Initialize failed"); return false; }
    o_ProcessInput = (decltype(o_ProcessInput))Hook_Create(G::VA::ProcessInput, (void*)hk_ProcessInput, "ProcessInput");
    o_UpdateDriveMode = (decltype(o_UpdateDriveMode))Hook_Create(G::VA::UpdateDriveMode, (void*)hk_UpdateDriveMode, "UpdateDriveMode");
    o_FHC_Hostile = (decltype(o_FHC_Hostile))Hook_Create(G::VA::FeedbackHitCheckHostile, (void*)hk_FHC_Hostile, "FeedbackHitCheckHostile");
    o_FHC_NonHostile = (decltype(o_FHC_NonHostile))Hook_Create(G::VA::FeedbackHitCheckNonHostile, (void*)hk_FHC_NonHostile, "FeedbackHitCheckNonHostile");
    o_GuiObjectHitCheck = (decltype(o_GuiObjectHitCheck))Hook_Create(G::VA::GuiMan_GuiObjectHitCheck, (void*)hk_GuiObjectHitCheck, "GuiObjectHitCheck");
    o_UpdateMouseCursor = (decltype(o_UpdateMouseCursor))Hook_Create(G::VA::UpdateMouseCursorAppearance, (void*)hk_UpdateMouseCursor, "UpdateMouseCursorAppearance");
    return o_ProcessInput && o_UpdateDriveMode && o_FHC_Hostile && o_FHC_NonHostile && o_GuiObjectHitCheck;
}
