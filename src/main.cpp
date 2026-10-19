// NWNPad - Xbox gamepad support and gamepad interface for Neverwinter Nights: Enhanced Edition
#include <windows.h>
#include <cstdio>
#include "log.h"
#include "config.h"
#include "game.h"
#include "hooks.h"
#include "pad.h"
#include "control.h"
#include "ui.h"

void Proxy_Load();
static char g_dir[MAX_PATH];

static void Init() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    // only act inside the game client
    const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    if (_stricmp(base, "nwmain.exe") != 0) return;
    strcpy(g_dir, exe);
    *strrchr(g_dir, '\\') = 0;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\NWNPad.log", g_dir);
    Log_Init(path);
    Log("NWNPad 1.0 starting (%s), module base %p", exe, (void*)GetModuleHandleA("version.dll"));
    snprintf(path, sizeof path, "%s\\NWNPad.ini", g_dir);
    Config_Load(path);
    if (!g_cfg.enabled) { Log("disabled in ini"); return; }
    if (!G::Init()) { Log("game globals not found - unsupported build"); return; }
    Pad_Init();
    Control_Init();
    UI_Init(g_dir);
    if (!Hooks_Install()) { Log("hook installation failed"); return; }
    UI_InstallHooks();
    Log("ready");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        Proxy_Load();
        Init();
    }
    return TRUE;
}
