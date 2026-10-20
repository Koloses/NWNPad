// NWNPad - party command wheel (left stick click): orders for henchmen, companions and summons
#include "party.h"
#include "uikit.h"
#include "render.h"
#include "game.h"
#include "gamedata.h"
#include "inv.h"
#include "res.h"
#include "control.h"
#include "pad.h"
#include "log.h"
#include <cmath>
#include <string>
#include <vector>

using namespace G;
using namespace R;

namespace {
const uintptr_t VA_Creature_PerformQuickChat = 0x140803f40;   // (creature, voice id): shouted orders
const uintptr_t VA_Msg_AssociateCommand = 0x1407bdbe0;        // (msg, associate, command)
const uintptr_t VA_Msg_PossessFamiliar = 0x1407bea40;         // (msg)
const uintptr_t VA_App_PlayerAssociateType = 0x1406a9820;     // (app) -> type of the creature we control

enum Cmd { C_FOLLOW, C_GUARD, C_ATTACK, C_HOLD, C_HEAL, C_STEALTH, C_SEARCH, C_POSSESS };
struct Item { Cmd cmd; const char* label; const char* icon; };
const Item ITEMS[8] = {
    {C_FOLLOW, "Follow me", "ir_follow"}, {C_GUARD, "Guard me", "ir_guard"}, {C_ATTACK, "Attack", "ir_attacknearest"},
    {C_HOLD, "Hold position", "ir_standground"}, {C_HEAL, "Heal me", "ir_healme"}, {C_STEALTH, "Stealth (party)", "ir_stealth"},
    {C_SEARCH, "Search (party)", "ir_search"}, {C_POSSESS, "Possess familiar", "ife_possess"},
};

bool g_open = false;
int g_hover = -1;
double g_openTime = 0;

int AssocType() {
    if (!App()) return 0;
    typedef short (*fn)(void*);
    return ((fn)A(VA_App_PlayerAssociateType))(App());
}
// familiar of the player in the area (associate type 3 at creature+0x274)
uint32_t Familiar() {
    for (void* m : GD::PartyMembers(Player())) if (F<uint16_t>(m, 0x274) == 3) return Id(m);
    return 0;
}
bool Possessing() { return AssocType() == 3; }

const char* Label(int i) {
    if (ITEMS[i].cmd == C_POSSESS && Possessing()) return "Return to master";
    return ITEMS[i].label;
}
bool Enabled(int i) {
    switch (ITEMS[i].cmd) {
    case C_POSSESS: return Possessing() || Familiar() != 0;
    case C_STEALTH: case C_SEARCH: return !GD::PartyMembers(Player()).empty();
    default: return true;
    }
}

void Say(int voice) {
    void* pl = Player();
    if (!pl) return;
    typedef void (*fn)(void*, int16_t);
    ((fn)A(VA_Creature_PerformQuickChat))(pl, (int16_t)voice);
}
void AssocCmd(uint32_t id, int cmd) {
    void* m = Inv::Message();
    if (!m) return;
    typedef void (*fn)(void*, uint32_t, int);
    ((fn)A(VA_Msg_AssociateCommand))(m, id, cmd);
}

void Do(int i) {
    Cmd c = ITEMS[i].cmd;
    Log("party: %s", Label(i));
    switch (c) {
    case C_FOLLOW: Say(22); break;
    case C_GUARD: Say(9); break;
    case C_ATTACK: Say(0); break;
    case C_HOLD: Say(10); break;
    case C_HEAL: Say(4); break;
    case C_STEALTH: for (void* m : GD::PartyMembers(Player())) AssocCmd(Id(m), -22); break;
    case C_SEARCH: for (void* m : GD::PartyMembers(Player())) AssocCmd(Id(m), -23); break;
    case C_POSSESS:
        // the same request possesses the familiar and returns to the master (as the game's quickbar button)
        if (void* m = Inv::Message()) { typedef void (*fn)(void*); ((fn)A(VA_Msg_PossessFamiliar))(m); }
        break;
    }
}
}  // namespace

bool Party_Open() { return g_open; }

bool Party_Input() {
    if (!g_open) {
        if (g_pad.Pressed(PB_LS) && Player() && !g_ctl.radialOpen && !g_ctl.inActionTargetMode) {
            g_open = true; g_hover = -1; g_openTime = g_pad.now;
            return true;
        }
        return false;
    }
    if (!Player() || g_pad.Pressed(PB_B) || g_pad.Pressed(PB_LS)) { g_open = false; return true; }
    float x = g_pad.lx, y = g_pad.ly;
    if (fabsf(g_pad.rx) + fabsf(g_pad.ry) > fabsf(x) + fabsf(y)) { x = g_pad.rx; y = g_pad.ry; }
    if (x * x + y * y > 0.35f * 0.35f) {
        float ang = atan2f(x, y);                    // 0 = up, clockwise
        if (ang < 0) ang += 6.2831853f;
        g_hover = (int)floorf(ang / (6.2831853f / 8) + 0.5f) % 8;
    }
    if (g_pad.Pressed(PB_A) && g_hover >= 0) {
        if (Enabled(g_hover)) { Do(g_hover); g_open = false; }
    }
    return true;
}

void Party_Draw() {
    if (!g_open) return;
    float t = (float)fmin(1.0, (g_pad.now - g_openTime) / 0.12);
    float a = 1 - (1 - t) * (1 - t);
    float cx = W() * 0.5f, cy = H() * 0.5f;
    float R0 = px(190);
    Circle(cx, cy, R0 + px(70), Col(0, 0, 0, (int)(110 * a)));
    Ring(cx, cy, R0, px(1.5f), Theme::Line.alpha(0.5f * a));
    for (int i = 0; i < 8; i++) {
        float ang = i * 6.2831853f / 8;
        float ix = cx + sinf(ang) * R0, iy = cy - cosf(ang) * R0;
        bool on = i == g_hover, en = Enabled(i);
        float r = px(on ? 48 : 42);
        Shadow(ix - r, iy - r, 2 * r, 2 * r, r, px(12), Col(0, 0, 0, (int)(150 * a)));
        Circle(ix, iy, r, (on ? Theme::Sel0 : Col(18, 15, 12, 235)).alpha(a));
        Ring(ix, iy, r, px(on ? 2.5f : 1.5f), (on ? Theme::LineStrong : Theme::Line).alpha(a));
        uint32_t tex = Res_Texture(ITEMS[i].icon);
        if (tex) ImageRounded(tex, ix - r * 0.62f, iy - r * 0.62f, r * 1.24f, r * 1.24f, r * 0.62f, (en ? Col(255, 255, 255) : Col(90, 90, 90)).alpha(a));
        else TextCentered(FONT_TITLE, px(22), ix, iy - px(14), "?", Theme::GoldDim.alpha(a));
    }
    // centre: title + hovered label
    Circle(cx, cy, px(92), Col(14, 12, 10, (int)(235 * a)));
    Ring(cx, cy, px(92), px(1.5f), Theme::Line.alpha(a));
    TextCentered(FONT_TITLE, px(20), cx, cy - px(46), "Party", Theme::GoldDim.alpha(a));
    if (g_hover >= 0) {
        const char* l = Label(g_hover);
        float fs = px(21);
        if (TextWidth(FONT_BODY_BOLD, fs, l) > px(170)) fs = px(17);
        TextCentered(FONT_BODY_BOLD, fs, cx, cy - px(10), l, (Enabled(g_hover) ? Theme::Text : Theme::TextFaint).alpha(a));
        if (!Enabled(g_hover)) TextCentered(FONT_BODY, px(15), cx, cy + px(20), "no one to order", Theme::TextFaint.alpha(a));
    }
    int n = (int)GD::PartyMembers(Player()).size();
    char b[48]; snprintf(b, sizeof b, n == 1 ? "%d companion" : "%d companions", n);
    TextCentered(FONT_BODY, px(16), cx, cy + px(44), b, Theme::TextDim.alpha(a));
    HintItem h[] = {{GL_LS, "Choose"}, {GL_A, "Order"}, {GL_B, "Close"}};
    UI_DrawHints(h, 3, a);
}
