// NWNPad - convenience accessors for client-side game data used by the UI
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace GD {
void Init();
std::string ObjName(uint32_t id);
std::string PortraitBase(void* creature);                  // e.g. "po_hu_f_04_"
uint32_t PortraitTex(void* creature, char size = 'm');      // GL texture or 0
struct HP { int cur = 0, max = 0, temp = 0; bool known = false; };
HP CreatureHP(void* creature);
bool IsDead(void* creature);
int Reaction(void* creature);                               // 0 neutral 1 friendly 2 hostile
uint32_t MasterId(void* creature);
std::vector<void*> PartyMembers(void* player);              // associates in the area
int Level(void* creature);
bool IsPaused();
int Language();                                              // client language id
std::string LocStr(const void* exoLocString);               // text in the client language (UTF-8, colour tokens removed)
}
