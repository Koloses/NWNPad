#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
Config g_cfg;
static float getf(const char* s, const char* k, float def, const char* ini) {
    char buf[64]; char d[64]; snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA(s, k, d, buf, sizeof buf, ini);
    return (float)atof(buf);
}
static int geti(const char* s, const char* k, int def, const char* ini) { return GetPrivateProfileIntA(s, k, def, ini); }
void Config_Load(const char* ini) {
    Config& c = g_cfg;
    c.enabled = geti("General", "Enabled", 1, ini);
    c.newHud = geti("Interface", "GamepadHUD", 1, ini);
    c.minimap = geti("Interface", "Minimap", 1, ini);
    c.newDialog = geti("Interface", "GamepadDialogue", 1, ini);
    c.newMenus = geti("Interface", "GamepadMenus", 1, ini);
    c.uiScale = getf("Interface", "Scale", 0.0f, ini);
    c.codepage = geti("Interface", "CodePage", 0, ini);
    c.hideCursorWithPad = geti("Interface", "HideMouseCursor", 1, ini);
    c.deadzoneL = getf("Controls", "DeadzoneLeft", 0.24f, ini);
    c.deadzoneR = getf("Controls", "DeadzoneRight", 0.20f, ini);
    c.walkThreshold = getf("Controls", "WalkThreshold", 0.55f, ini);
    c.camYawSpeed = getf("Camera", "TurnSpeed", 1.0f, ini);
    c.camPitchSpeed = getf("Camera", "TiltSpeed", 0.8f, ini);
    c.camZoomSpeed = getf("Camera", "ZoomSpeed", 0.8f, ini);
    c.invertCamX = geti("Camera", "InvertX", 0, ini);
    c.invertCamY = geti("Camera", "InvertY", 0, ini);
    c.targetRange = getf("Targeting", "Range", 12.0f, ini);
    c.interactRange = getf("Targeting", "InteractRange", 5.0f, ini);
    c.rumble = geti("Controls", "Rumble", 1, ini);
    GetPrivateProfileStringA("Developer", "InjectFile", "", c.injectFile, sizeof c.injectFile, ini);
    c.devDumpArea = geti("Developer", "DumpArea", 0, ini);
    c.glDiag = geti("Developer", "GLDiag", 0, ini);
    Log("config: enabled=%d hud=%d dialog=%d menus=%d inject='%s'", c.enabled, c.newHud, c.newDialog, c.newMenus, c.injectFile);
}
