// NWNPad - quickbar access (names, icons, edit radial) for the HUD and the hub
#pragma once
#include <string>
#include <cstdint>
void QB_Init();
void QB_InstallHooks();
std::string QB_Name(int bank, int slot);              // what the slot does ("" if empty)
bool QB_Empty(int bank, int slot);
uint32_t QB_Icon(int bank, int slot, bool* disabled = nullptr);   // GL texture or 0
void QB_Clear(int bank, int slot);
void QB_Edit(int bank, int slot);
// modes (stances, detect, stealth ...) on quick slots: 0 not a mode, 1 off, 2 on, 3 switching (pressed,
// the game changes it at the next combat round)
int QB_ModeState(int bank, int slot);
void QB_NoteUsed(int bank, int slot);                  // a slot was activated (starts "switching")                     // open the game's "set button" radial menu
// pad binding that triggers this slot in gameplay (e.g. "LT + X")
void QB_Binding(int slot, int& modifier, int& button); // modifier: 0 none, 1 LT, 2 RT, 3 LT+RT; button: PadGlyph
