// NWNPad - quickbar access (names, icons, edit radial) for the HUD and the hub
#include "quickbar.h"
#include "control.h"
#include "uikit.h"
#include "game.h"
#include "hooks.h"
#include "text.h"
#include "res.h"
#include "log.h"
#include "pad.h"
#include <windows.h>
#include <cstring>
#include <map>

using namespace G;

namespace {
const uintptr_t VA_QuickButton_HandleRightButton = 0x14085d270;   // opens the set-button / item radial
const uintptr_t VA_QuickButton_HandleTriggerTooltip = 0x14085d410;
const size_t QB_Type = 0x140, QB_IconRef = 0x11c, QB_Flags = 8;

typedef void (*ShowTooltip_t)(void* gm, void* exoStr, int p);
ShowTooltip_t o_ShowTooltip = nullptr;
bool g_capture = false;
std::string g_captured;

void hk_ShowTooltip(void* gm, void* s, int p) {
    if (g_capture) { g_captured = s ? ExoStr(s) : std::string(); return; }
    o_ShowTooltip(gm, s, p);
}

struct Key { uint8_t type; char ref[17]; bool operator<(const Key& o) const { return type != o.type ? type < o.type : strcmp(ref, o.ref) < 0; } };
struct Entry { Key key; std::string name; };
std::map<int, Entry> g_names;
}  // namespace

// ---- modes ----
// mode toggle buttons (type 10) carry the action mode (ACTION_MODE_*: 0 detect, 1 stealth, 2 parry,
// 3 power attack, 4 improved power attack, 5 counterspell, 6 flurry, 7 expertise, 8 improved expertise,
// 9 rapid shot, 10 defensive casting, 11 dirty fighting, 12 defensive stance); feat buttons (type 4) of
// the feats that toggle a mode count too (CNWSCreature::ToggleMode). The client creature keeps one flag
// per mode, set from the server's player state (CNWCMessage::HandleServerToPlayerUpdate_PlayerInfo).
namespace {
const size_t QB_Id = 0x118;
int ModeOfButton(void* b) {
    uint8_t t = F<uint8_t>(b, QB_Type);
    int id = F<int>(b, QB_Id);
    if (t == 10) return id >= 0 && id <= 12 ? id : -1;
    if (t == 4) {
        switch (id) {
        case 28: return 3; case 19: return 4; case 204: case 899: case 866: return 6;
        case 30: return 7; case 389: return 8; case 390: return 9; case 425: return 11; case 947: return 12;
        }
    }
    return -1;
}
int ModeFlag(void* cre, int mode) {          // -1 unknown (counterspell has no flag on the client)
    static const int ofs[13] = {0x234, 0x238, 0x240, 0x244, 0x248, -1, 0x258, 0x254, 0x25c, 0x260, 0x250, 0x264, 0x268};
    if (!cre || mode < 0 || mode > 12 || ofs[mode] < 0) return -1;
    return F<int>(cre, ofs[mode]) != 0;
}
struct Pending { int mode = -1; int want = 0; double t = 0; } g_pending[13];
}  // namespace

int QB_ModeState(int bank, int slot) {
    void* b = Control_QuickButton(bank, slot);
    void* pl = Player();
    if (!b || !pl) return 0;
    int m = ModeOfButton(b);
    if (m < 0 || m > 12) return 0;
    int on = ModeFlag(pl, m);
    Pending& p = g_pending[m];
    if (p.mode == m) {
        if (on == p.want || g_pad.now - p.t > 15.0) p.mode = -1;      // done (or never happened)
        else return 3;
    }
    return on == 1 ? 2 : 1;
}

void QB_NoteUsed(int bank, int slot) {
    void* b = Control_QuickButton(bank, slot);
    void* pl = Player();
    if (!b || !pl) return;
    int m = ModeOfButton(b);
    if (m < 0 || m > 12) return;
    int on = ModeFlag(pl, m);
    if (on < 0) return;
    g_pending[m] = {m, !on, g_pad.now};
    // one combat mode at a time: switching to another stance ends the current one
    Log("quickslot: mode %d %s", m, on ? "off" : "on");
}

void QB_Init() {}

void QB_InstallHooks() {
    void* p = (void*)GetProcAddress(GetModuleHandleA(nullptr), "?ShowTooltipText@CGuiMan@@QEAAXAEBVCExoString@@H@Z");
    if (p) o_ShowTooltip = (ShowTooltip_t)Hook_Create((uintptr_t)p - G::base + 0x140000000ULL, (void*)hk_ShowTooltip, "ShowTooltipText");
}

bool QB_Empty(int bank, int slot) {
    void* b = Control_QuickButton(bank, slot);
    return !b || F<uint8_t>(b, QB_Type) == 0;
}

uint32_t QB_Icon(int bank, int slot, bool* disabled) {
    void* b = Control_QuickButton(bank, slot);
    if (disabled) *disabled = b ? (F<uint32_t>(b, QB_Flags) & 1) != 0 : true;
    if (!b || !F<uint8_t>(b, QB_Type)) return 0;
    char ref[17] = {};
    memcpy(ref, (char*)b + QB_IconRef, 16);
    return ref[0] ? Res_Texture(ref) : 0;
}

std::string QB_Name(int bank, int slot) {
    void* b = Control_QuickButton(bank, slot);
    if (!b || !F<uint8_t>(b, QB_Type) || !o_ShowTooltip) return std::string();
    Key k{}; k.type = F<uint8_t>(b, QB_Type); memcpy(k.ref, (char*)b + QB_IconRef, 16);
    int idx = bank * 12 + slot;
    auto it = g_names.find(idx);
    if (it != g_names.end() && !(it->second.key < k) && !(k < it->second.key)) return it->second.name;
    // ask the button for its tooltip and catch the text instead of showing it
    g_capture = true; g_captured.clear();
    typedef void (*fn)(void*);
    ((fn)A(VA_QuickButton_HandleTriggerTooltip))(b);
    g_capture = false;
    std::string s = g_captured;
    size_t p = s.find(" : ");
    if (p != std::string::npos && p < 24) s = s.substr(p + 3);      // drop the key label ("F1 : ")
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
    for (auto& c : s) if (c == '\n') c = ' ';
    g_names[idx] = {k, s};
    return s;
}

void QB_Edit(int bank, int slot) {
    void* b = Control_QuickButton(bank, slot);
    if (!b) return;
    int w, h; Control_GuiSize(w, h);
    Control_SetMouse(w / 2, h / 2);
    Log("quickbar edit bank %d slot %d", bank, slot);
    // always the full "assign" menu (attack, special attacks, abilities, cast, commands, actions, emotes):
    // a filled slot would show only its own options; the slot keeps its content if nothing is chosen
    uint8_t type = F<uint8_t>(b, 0x140);
    F<uint8_t>(b, 0x140) = 0;
    typedef void (*fn)(void*);
    ((fn)A(VA_QuickButton_HandleRightButton))(b);
    if (F<uint8_t>(b, 0x140) == 0) F<uint8_t>(b, 0x140) = type;
}

void QB_Clear(int bank, int slot) {
    void* b = Control_QuickButton(bank, slot);
    if (!b) return;
    Log("quickbar clear bank %d slot %d", bank, slot);
    typedef void (*fn)(void*);
    ((fn)A(0x14085e6c0))(b);              // CGuiQuickButton::RadialMenuItemClearSlot
}

void QB_Binding(int slot, int& mod, int& btn) {
    static const int m[12] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3};
    static const int g[12] = {GL_X, GL_Y, GL_B, GL_X, GL_Y, GL_B, GL_A, GL_X, GL_Y, GL_B, GL_A, GL_A};
    mod = m[slot % 12]; btn = g[slot % 12];
}
