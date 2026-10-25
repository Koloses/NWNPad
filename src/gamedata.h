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
// exact hit points reach the client only for the player and his associates; for everyone else the server
// sends a damage level (stats+0x8e: 0 >= 95%, 1 >= 75%, 2 >= 50%, 3 >= 25%, 4 alive, 5 dead)
struct HP { int cur = 0, max = 0, temp = 0; bool known = false; bool exact = false; int level = 0; float frac = 1.0f; };
const char* DamageLevelName(int level);
HP CreatureHP(void* creature);
bool IsDead(void* creature);
int Reaction(void* creature);                               // 0 neutral 1 friendly 2 hostile
uint32_t MasterId(void* creature);
std::vector<void*> PartyMembers(void* player);              // associates in the area
// effect icons (effecticons.2da rows) on a creature: the server's list in single player, otherwise what
// the game's player state bar shows for the hero (the server tells a client only about its own hero)
std::vector<int> EffectIcons(void* creature);
uint32_t EffectIconTex(int icon);                           // GL texture of effecticons.2da Icon, or 0
std::string EffectIconName(int icon);
int Level(void* creature);
bool IsPaused();
bool LevelUpReady();
int Language();                                              // client language id
std::string LocStr(const void* exoLocString);               // text in the client language (UTF-8, colour tokens removed)
}
