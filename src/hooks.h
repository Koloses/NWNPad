#pragma once
#include <cstdint>
bool Hooks_Install();
void* Hook_Create(uintptr_t va, void* detour, const char* name);   // returns trampoline (original)
// originals
extern void (*o_ProcessInput)(void* in);
extern void (*o_UpdateDriveMode)(void* in);
extern int  (*o_FHC_Hostile)(void* in, void** obj, void* pos, int* walk, int p4);
extern int  (*o_FHC_NonHostile)(void* in, void** obj, void* pos, int* walk, int p4);
extern void* (*o_GuiObjectHitCheck)(void* gm, int x, int y, int p3, int p4);
// hit-check override (pad targeting)
struct HitOverride { bool active; void* obj; float x, y, z; int code; };
extern HitOverride g_hitOverride;
