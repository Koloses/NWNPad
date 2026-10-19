#pragma once
#include <cstdint>
enum PadBtn : uint32_t {
    PB_DUP = 0x0001, PB_DDOWN = 0x0002, PB_DLEFT = 0x0004, PB_DRIGHT = 0x0008,
    PB_START = 0x0010, PB_BACK = 0x0020, PB_LS = 0x0040, PB_RS = 0x0080,
    PB_LB = 0x0100, PB_RB = 0x0200, PB_A = 0x1000, PB_B = 0x2000, PB_X = 0x4000, PB_Y = 0x8000,
    PB_LT = 0x10000, PB_RT = 0x20000,          // synthesized digital triggers
    PB_LUP = 0x100000, PB_LDOWN = 0x200000, PB_LLEFT = 0x400000, PB_LRIGHT = 0x800000 // LS as digital (menus)
};
struct Pad {
    bool connected = false;
    uint32_t held = 0, pressed = 0, released = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;   // sticks after deadzone, -1..1 ; triggers 0..1
    double holdTime[32] = {};   // seconds each button has been held
    double now = 0, dt = 0;
    bool Down(uint32_t b) const { return (held & b) != 0; }
    bool Pressed(uint32_t b) const { return (pressed & b) != 0; }
    bool Released(uint32_t b) const { return (released & b) != 0; }
    double Held(uint32_t b) const;
    // auto-repeat for menu navigation: true on press and then every `rate` after `delay`
    bool Repeat(uint32_t b, double delay = 0.35, double rate = 0.09);
    uint32_t repeatFired = 0; double repeatNext[32] = {};
};
extern Pad g_pad;
void Pad_Init();
void Pad_Update();          // once per game frame
void Pad_Rumble(float lo, float hi, double seconds);
