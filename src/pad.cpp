#include "pad.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstdio>

Pad g_pad;
struct XGP { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XST { DWORD dwPacketNumber; XGP Gamepad; };
struct XVIB { WORD l, r; };
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XST*);
typedef DWORD(WINAPI* XInputSetState_t)(DWORD, XVIB*);
static XInputGetState_t pGetState = nullptr;
static XInputSetState_t pSetState = nullptr;
static LARGE_INTEGER g_freq, g_t0;
static double g_rumbleUntil = 0;
static int g_userIndex = -1;

static int bitIndex(uint32_t b) { int i = 0; while (b > 1) { b >>= 1; i++; } return i; }
double Pad::Held(uint32_t b) const { return (held & b) ? holdTime[bitIndex(b)] : 0.0; }
bool Pad::Repeat(uint32_t b, double delay, double rate) {
    int i = bitIndex(b);
    if (pressed & b) { repeatNext[i] = now + delay; return true; }
    if ((held & b) && now >= repeatNext[i]) { repeatNext[i] = now + rate; return true; }
    return false;
}

void Pad_Init() {
    const char* dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (auto d : dlls) {
        HMODULE h = LoadLibraryA(d);
        if (h) {
            pGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
            pSetState = (XInputSetState_t)GetProcAddress(h, "XInputSetState");
            if (pGetState) { Log("XInput: using %s", d); break; }
        }
    }
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
}

static float stickAxis(SHORT v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }
static void radialDeadzone(float& x, float& y, float dz) {
    float m = sqrtf(x * x + y * y);
    if (m < dz) { x = y = 0; return; }
    float n = (m - dz) / (1.0f - dz);
    if (n > 1) n = 1;
    x = x / m * n; y = y / m * n;
}

void Pad_Update() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    double now = double(t.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    g_pad.dt = g_pad.now > 0 ? now - g_pad.now : 0.016;
    if (g_pad.dt > 0.25) g_pad.dt = 0.25;
    g_pad.now = now;

    XGP gp = {};
    bool ok = false;
    if (pGetState) {
        XST st;
        if (g_userIndex >= 0 && pGetState(g_userIndex, &st) == 0) { gp = st.Gamepad; ok = true; }
        else {
            // one slot at a time (an empty slot can take milliseconds to answer), all four within a second
            static double nextScan = 0;
            static int slot = 0;
            if (now >= nextScan) {
                nextScan = now + 0.25;
                int i = slot; slot = (slot + 1) & 3;
                if (pGetState(i, &st) == 0) { g_userIndex = i; gp = st.Gamepad; ok = true; Log("pad connected on slot %d", i); }
                if (!ok) g_userIndex = -1;
            }
        }
        if (!ok && g_userIndex >= 0) { Log("pad disconnected"); g_userIndex = -1; }
    }
    g_pad.connected = ok;
    uint32_t b = ok ? gp.wButtons : 0;
    g_pad.lx = stickAxis(gp.sThumbLX); g_pad.ly = stickAxis(gp.sThumbLY);
    g_pad.rx = stickAxis(gp.sThumbRX); g_pad.ry = stickAxis(gp.sThumbRY);
    radialDeadzone(g_pad.lx, g_pad.ly, g_cfg.deadzoneL);
    radialDeadzone(g_pad.rx, g_pad.ry, g_cfg.deadzoneR);
    g_pad.lt = gp.bLeftTrigger / 255.0f; g_pad.rt = gp.bRightTrigger / 255.0f;
    if (g_pad.lt > 0.35f) b |= PB_LT;
    if (g_pad.rt > 0.35f) b |= PB_RT;
    // left stick as digital directions with hysteresis (used by menus)
    static uint32_t lsDig = 0;
    auto dig = [&](uint32_t bit, float v, bool pos) {
        float a = pos ? v : -v;
        if (lsDig & bit) { if (a < 0.35f) lsDig &= ~bit; } else if (a > 0.6f) lsDig |= bit;
    };
    dig(PB_LUP, g_pad.ly, true); dig(PB_LDOWN, g_pad.ly, false);
    dig(PB_LRIGHT, g_pad.lx, true); dig(PB_LLEFT, g_pad.lx, false);
    b |= lsDig;

    g_pad.pressed = b & ~g_pad.held;
    g_pad.released = g_pad.held & ~b;
    g_pad.held = b;
    for (int i = 0; i < 32; i++) {
        if (b & (1u << i)) g_pad.holdTime[i] += g_pad.dt; else g_pad.holdTime[i] = 0;
    }
    if (g_rumbleUntil > 0 && now > g_rumbleUntil && pSetState && g_userIndex >= 0) {
        XVIB v = {0, 0}; pSetState(g_userIndex, &v); g_rumbleUntil = 0;
    }
}

void Pad_Rumble(float lo, float hi, double seconds) {
    if (!g_cfg.rumble || !pSetState || g_userIndex < 0) return;
    XVIB v = {(WORD)(lo * 65535), (WORD)(hi * 65535)};
    pSetState(g_userIndex, &v);
    g_rumbleUntil = g_pad.now + seconds;
}
