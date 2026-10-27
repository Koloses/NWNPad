#include "prof.h"
#include "log.h"
#include "config.h"
#include <windows.h>
#include <cstdio>

namespace {
const char* NAMES[PS_COUNT] = {"ui", "pre-input", "post-input", "drive", "hooks", "(hud", "(minimap", "(gl", "(quest"};
double g_frame[PS_COUNT], g_sum[PS_COUNT], g_max[PS_COUNT];
double g_frameTotalMax = 0, g_winStart = 0;
int g_frames = 0, g_spikes = 0;
double g_freq = 0;
}

double Prof_Now() {
    LARGE_INTEGER t;
    if (!g_freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_freq = (double)f.QuadPart / 1000.0; }
    QueryPerformanceCounter(&t);
    return t.QuadPart / g_freq;
}
void Prof_Add(ProfSec s, double ms) { g_frame[s] += ms; }

void Prof_Frame() {
    // a long gap between two frames: the game stood still (what our code took that frame is below)
    static double lastSwap = 0;
    double nowSwap = Prof_Now();
    if (lastSwap > 0 && nowSwap - lastSwap > 400.0) {
        static int freezes = 0;
        if (freezes++ < 200) Log("prof: FREEZE %.0f ms between frames (NWNPad in it: ui %.1f pre-input %.1f post-input %.1f ms)", nowSwap - lastSwap, g_frame[PS_UI], g_frame[PS_PREINPUT], g_frame[PS_POSTINPUT]);
    }
    lastSwap = nowSwap;
    double total = 0;
    for (int i = 0; i < PS_HUD; i++) total += g_frame[i];          // the parts are inside "ui"
    if (total > 8.0 && g_spikes < 200) {                 // a frame where our code took long
        g_spikes++;
        char b[256]; int n = 0;
        for (int i = 0; i < PS_COUNT; i++) if (g_frame[i] > 0.5) n += snprintf(b + n, sizeof b - n, " %s %.1f", NAMES[i], g_frame[i]);
        Log("prof: frame spent %.1f ms in NWNPad:%s", total, b);
    }
    for (int i = 0; i < PS_COUNT; i++) { g_sum[i] += g_frame[i]; if (g_frame[i] > g_max[i]) g_max[i] = g_frame[i]; g_frame[i] = 0; }
    if (total > g_frameTotalMax) g_frameTotalMax = total;
    g_frames++;
    double now = Prof_Now();
    if (!g_winStart) g_winStart = now;
    if (now - g_winStart >= 10000) {
        if (g_cfg.profile) {
            char b[512]; int n = 0;
            for (int i = 0; i < PS_COUNT; i++) n += snprintf(b + n, sizeof b - n, " %s %.2f/%.1f", NAMES[i], g_sum[i] / g_frames, g_max[i]);
            Log("prof: %d frames, avg/max ms:%s, worst frame %.1f", g_frames, b, g_frameTotalMax);
        }
        for (int i = 0; i < PS_COUNT; i++) { g_sum[i] = 0; g_max[i] = 0; }
        g_frames = 0; g_frameTotalMax = 0; g_winStart = now;
    }
}
