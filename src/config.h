#pragma once
struct Config {
    int enabled = 1;
    int newHud = 1;            // replace the game's HUD with the gamepad HUD
    int minimap = 1;           // minimap in the top-right corner of the gamepad HUD
    int newDialog = 1;         // gamepad dialogue window
    int newMenus = 1;          // gamepad hub (inventory, character, journal...)
    float deadzoneL = 0.24f, deadzoneR = 0.20f;
    float walkThreshold = 0.55f;    // stick deflection below this walks
    float camYawSpeed = 1.0f, camPitchSpeed = 0.8f, camZoomSpeed = 0.8f;   // 1.0 = keyboard speed
    int invertCamX = 0, invertCamY = 0;
    float targetRange = 12.0f, interactRange = 5.0f;
    int rumble = 1;
    float uiScale = 0.0f;      // 0 = auto
    int codepage = 0;          // 0 = auto (from game language)
    int hideCursorWithPad = 1;
    char injectFile[260] = ""; // developer: virtual pad input file
    int devDumpArea = 0;
    int glDiag = 0;            // developer: OpenGL diagnostics and frame capture (gldiag.cpp)       // developer: log area objects when the hub opens
};
extern Config g_cfg;
void Config_Load(const char* iniPath);
