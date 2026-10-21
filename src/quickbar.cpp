// NWNPad - quickbar access (names, icons, edit radial) for the HUD and the hub
#include "quickbar.h"
#include "control.h"
#include "uikit.h"
#include "game.h"
#include "hooks.h"
#include "text.h"
#include "res.h"
#include "log.h"
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
