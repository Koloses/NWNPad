// NWNPad - client-side inventory / character data and actions
#pragma once
#include <string>
#include <vector>
#include <cstdint>

enum EquipSlot {
    ES_HEAD = 0, ES_CHEST, ES_BOOTS, ES_ARMS, ES_RHAND, ES_LHAND, ES_CLOAK, ES_LRING, ES_RRING,
    ES_NECK, ES_BELT, ES_ARROWS, ES_BULLETS, ES_BOLTS, ES_COUNT_PLAYER, ES_TOTAL = 18
};

struct InvItem {
    uint32_t id = 0x7f000000;
    void* item = nullptr;            // CNWCItem*
    std::string name;
    int baseItem = -1;
    int stack = 1;
    bool identified = true;
    bool stolen = false;
    uint32_t cost = 0;               // base value (gp) before store markup
    uint32_t equipMask = 0;          // base item EquipableSlots
    int equippedSlot = -1;
    int gx = 0, gy = 0;              // grid position (sorting)
    std::string icon[3];             // up to 3 icon layers (bottom..top)
    int iconLayers = 0;
    bool plt = false;                // icon[] are PLT parts coloured with pltColors
    std::vector<std::string> pltParts;
    std::vector<uint16_t> pltColors;
    std::string typeName;            // base item name
    bool usable = true;              // the hero meets its requirements (class, race, alignment, proficiency, level)
};

namespace Inv {
void Init();
// gather backpack items (owned by creature, not equipped) and equipped items
void Gather(void* creature, std::vector<InvItem>& pack, InvItem equipped[ES_TOTAL]);
bool Fill(uint32_t id, InvItem& out);
bool CanUse(uint32_t id);                // the player can use / equip it (shown red otherwise)
void* ItemById(uint32_t id);
uint32_t EquippedId(void* creature, int slot);
int Gold(void* creature);
// actions
void Equip(void* creature, const InvItem& it, int slot);   // slot -1 = choose
void UnEquip(void* creature, uint32_t itemId);
void Examine(void* creature, uint32_t itemId);
void Drop(void* creature, uint32_t itemId);
// tell the server our inventory view is open (it then streams repository updates)
void SetGuiOpen(void* creature, bool open);
// same for the character sheet (server then sends experience etc.)
void SetCharSheetOpen(void* creature, bool open);
bool UseItem(void* creature, uint32_t id);   // false: use the radial menu instead
void OpenRadial(uint32_t itemId);
void Split(uint32_t itemId, int count);                     // new stack of `count` split off
void Merge(uint32_t intoId, uint32_t fromId);               // combine two stacks of the same item
void OpenBag(uint32_t bagId);                               // open a bag in the backpack (container window)
void MoveToBag(uint32_t itemId, uint32_t bagId);
bool IsContainer(const InvItem& it);                        // bag / box that holds items
int  MaxStack(const InvItem& it);
int  BestSlot(void* creature, const InvItem& it);           // -1 if not equippable
const char* SlotName(int slot);
void* Message();                            // CNWCMessage*
// containers
void* InventoryPanel();
uint32_t OpenContainerId();                 // 0x7f000000 if none
void GatherContainer(void* creature, std::vector<InvItem>& out);
void TakeItem(uint32_t itemId);
void CloseContainer();
void ContainerPage(int dir);
// icons
uint32_t IconTex(const std::string& resref);
void DrawIcon(const InvItem& it, float x, float y, float size, float alpha = 1.0f);

// character sheet data
struct CharInfo {
    std::string name, classes, race;
    int level = 0, xp = 0, xpCur = 0, xpNext = 0, ac = 0, bab = 0;
    int ab[6] = {};      // STR DEX CON INT WIS CHA
    int hp = 0, hpMax = 0, gold = 0;
    bool ok = false;
};
CharInfo Character(void* creature);
}
