// NWNPad - client-side inventory / character data and actions
#include "inv.h"
#include "game.h"
#include "gamedata.h"
#include "text.h"
#include "res.h"
#include "render.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <algorithm>

using namespace G;

namespace {
// CNWCCreature (Win)
const uintptr_t VA_PerformEquipItem = 0x140803bd0;     // (cre, itemId, slotMask)
const uintptr_t VA_PerformUnEquipItem = 0x140804190;   // (cre, itemId)
const uintptr_t VA_PerformDropItem = 0x1408039d0;      // (cre, itemId)
const uintptr_t VA_PerformExamine = 0x140803d90;       // (cre, id, 0, 0)
const uintptr_t VA_Item_ActivateRadial = 0x14083b890;  // (item, int)
const uintptr_t VA_Item_GetName = 0x14083c300;         // (item, CExoString*, int)
const uintptr_t VA_App_GetNWCMessage = 0x1406a9600;    // (app)
const uintptr_t VA_App_GetInventoryPanel = 0x1406a9250; // (app)
const uintptr_t VA_Msg_RepositoryMove = 0x1407bfb30;   // (msg, item, x, y, repository)
const uintptr_t VA_Msg_SendToServer = 0x1407c10c0;     // (msg, major, minor, data, size)
const uintptr_t VA_PanelInv_HandleXButton = 0x140854e80;   // (panel)
const uintptr_t VA_Msg_GuiInventoryStatus = 0x1407bcc10;   // (msg, open, creatureId)
const uintptr_t VA_Msg_GuiCharSheetStatus = 0x1407bc8d0;   // (msg, char page (-1 close), creatureId)
// CNWCCreatureStats
const uintptr_t VA_Stats_ClassAbilityAdjust = 0x1407efc10;   // (stats, ability) -> char
const uintptr_t VA_Stats_GetArmorClass = 0x1407ee4c0;        // (stats, int)
const uintptr_t VA_Stats_CombatModeAC = 0x1407f09d0;         // (stats, creature)
const uintptr_t VA_Stats_BAB = 0x1407ef400;                  // (stats)
const uintptr_t VA_Stats_ClassesString = 0x1407efd30;        // (stats, CExoString*, char)
const uintptr_t VA_Stats_RaceString = 0x1407f52c0;           // (stats, CExoString*)
const uintptr_t VA_Stats_ExpNeeded = 0x1407f28a0;            // (stats)
const uintptr_t VA_Stats_GetLevel = 0x1407f4870;

// CNWCItem field offsets (Win)
const size_t IT_BaseItem = 0x40, IT_Id = 0x50, IT_Identified = 0x1c0, IT_Icon = 0x238;
const size_t IT_EquippedBy = 0x25c, IT_Possessor = 0x264, IT_Stack = 0x284;
const size_t IT_GridX = 0x254, IT_GridY = 0x255;
const size_t IT_InContainer = 0x268, IT_Stolen = 0x1c4, IT_Cost = 0x280;
const size_t CRE_Equipped = 0x308, CRE_Gold = 0x1b8;
// CPanelInventory (Win)
const size_t PI_ContainerOpen = 0x2f64, PI_ContainerId = 0x268c;

void* p_rules = nullptr;       // CNWRules** (g_pRules)
typedef void* (*GetBaseItem_t)(void* arr, int id);
GetBaseItem_t fGetBaseItem = nullptr;
typedef void (*ExoDtor_t)(void*);
ExoDtor_t fExoDtor = nullptr;

struct Exo { char* p = nullptr; uint32_t len = 0, cap = 0; };
std::string TakeExo(Exo& s) {
    std::string r = s.p ? G2UClean(s.p, (int)s.len) : std::string();
    if (fExoDtor) fExoDtor(&s);
    return r;
}

void* Rules() { return p_rules ? *(void**)p_rules : nullptr; }
void* BaseItem(int id) {
    void* r = Rules();
    if (!r || !fGetBaseItem || id < 0) return nullptr;
    void* arr = F<void*>(r, 0x20);
    return arr ? fGetBaseItem(arr, id) : nullptr;
}

void* ObjArray() {
    void* in = Internal();
    return in ? F<void*>(in, 0x28) : nullptr;
}

void* ItemFromGameObject(void* go) {
    if (!go || !Readable(go, 0x10)) return nullptr;
    return VCall<void* (*)(void*)>(go, OFS::V_AsItem)(go);
}
}  // namespace

namespace Inv {
void Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    p_rules = (void*)GetProcAddress(exe, "?g_pRules@@3PEAVCNWRules@@EA");
    fGetBaseItem = (GetBaseItem_t)GetProcAddress(exe, "?GetBaseItem@CNWBaseItemArray@@QEBAPEAVCNWBaseItem@@H@Z");
    fExoDtor = (ExoDtor_t)GetProcAddress(exe, "??1CExoString@@QEAA@XZ");
    Log("inv: rules=%p getbaseitem=%p", p_rules, (void*)fGetBaseItem);
}

void* ItemById(uint32_t id) {
    if (id == 0x7f000000 || !App()) return nullptr;
    typedef void* (*fn)(void*, uint32_t);
    void* go = ((fn)A(VA::App_GetGameObject))(App(), id);
    return ItemFromGameObject(go);
}

uint32_t EquippedId(void* cre, int slot) {
    if (!cre || slot < 0 || slot >= ES_TOTAL) return 0x7f000000;
    return F<uint32_t>(cre, CRE_Equipped + slot * 4);
}

int Gold(void* cre) { return cre ? F<int>(cre, CRE_Gold) : 0; }

static void ReadIcon(void* item, InvItem& out) {
    out.iconLayers = 0;
    void* ic = F<void*>(item, IT_Icon);
    if (!ic || !Readable(ic, 0xc8)) return;
    const char* cn = ClassName(ic);
    auto ref = [&](size_t off) { char b[17] = {}; memcpy(b, (char*)ic + off, 16); return std::string(b); };
    if (cn && strstr(cn, "ArmorIcon")) {
        int n = F<int>(ic, 0xb0);
        uint16_t* c = F<uint16_t*>(ic, 0xb8);
        uint16_t* c2 = F<uint16_t*>(ic, 0xc0);
        out.plt = true;
        for (int k = 0; k < 6 && out.iconLayers < 3; k++) {
            std::string r = ref(0x48 + k * 0x11);
            if (r.empty()) continue;
            out.icon[out.iconLayers++] = r;
        }
        // up to 6 parts; keep the extra ones too
        std::vector<std::string> parts;
        for (int k = 0; k < 6; k++) { std::string r = ref(0x48 + k * 0x11); if (!r.empty()) parts.push_back(r); }
        out.pltParts = parts;
        if (n == 10) {
            for (int k = 0; k < 6; k++) {
                if (ref(0x48 + k * 0x11).empty()) continue;
                uint16_t* src = c2 && Readable(c2 + k * 10, 20) ? c2 + k * 10 : (c && Readable(c, 20) ? c : nullptr);
                for (int j = 0; j < 10; j++) out.pltColors.push_back(src ? src[j] : 0);
            }
        }
        return;
    }
    if (cn && strstr(cn, "LayeredIcon")) {
        int n = F<int>(ic, 0x48);
        uint16_t* c = F<uint16_t*>(ic, 0x50);
        out.plt = true;
        out.pltParts.push_back(ref(8));
        for (int j = 0; j < 10; j++) out.pltColors.push_back(n == 10 && c && Readable(c, 20) ? c[j] : 0);
        out.icon[out.iconLayers++] = ref(8);
        return;
    }
    if (cn && strstr(cn, "CompositeIcon")) {
        std::string a = ref(8), b = ref(0x48), c = ref(0x59);
        if (!a.empty()) out.icon[out.iconLayers++] = a;
        if (!b.empty()) out.icon[out.iconLayers++] = b;
        if (!c.empty()) out.icon[out.iconLayers++] = c;
    } else {
        std::string a = ref(8);
        if (!a.empty()) out.icon[out.iconLayers++] = a;
    }
}

bool Fill(uint32_t id, InvItem& out) {
    void* it = ItemById(id);
    if (!it || !Readable(it, 0x2a0)) return false;
    out = InvItem();
    out.id = id;
    out.item = it;
    out.baseItem = F<int>(it, IT_BaseItem);
    out.stack = F<int>(it, IT_Stack);
    if (out.stack < 1) out.stack = 1;
    out.identified = F<int>(it, IT_Identified) != 0;
    out.stolen = F<int>(it, IT_Stolen) != 0;
    out.cost = F<uint32_t>(it, IT_Cost);
    out.gx = F<uint8_t>(it, IT_GridX);
    out.gy = F<uint8_t>(it, IT_GridY);
    Exo s;
    typedef void (*fn)(void*, Exo*, int);
    ((fn)A(VA_Item_GetName))(it, &s, 1);
    out.name = TakeExo(s);
    void* bi = BaseItem(out.baseItem);
    if (bi) {
        out.equipMask = F<uint32_t>(bi, 4);
        out.typeName = Tlk(F<uint32_t>(bi, 0));
    }
    if (out.name.empty()) out.name = out.typeName;
    ReadIcon(it, out);
    return true;
}

template <class Fn> static void ForEachItem(Fn fn) {
    void* arr = ObjArray();
    if (!arr || !Readable(arr, 0x28)) return;
    void** buckets = F<void**>(arr, 0);
    uint32_t nb = F<uint32_t>(arr, 0x24);
    if (!buckets || nb == 0 || nb > (1u << 24) || !Readable(buckets, nb * 8)) return;
    for (uint32_t b = 0; b < nb; b++) {
        int guard = 0;
        for (char* node = (char*)buckets[b]; node && guard < 10000; node = *(char**)(node + 0x10), guard++) {
            void* go = *(void**)(node + 8);
            if (!go) continue;
            if (F<uint8_t>(go, OFS::Obj_Type) != OT_ITEM) continue;
            void* it = ItemFromGameObject(go);
            if (it) fn(it);
        }
    }
}

static void SortByGrid(std::vector<InvItem>& v) {
    std::sort(v.begin(), v.end(), [](const InvItem& a, const InvItem& b) {
        if (a.gy != b.gy) return a.gy < b.gy;
        return a.gx < b.gx;
    });
}

void Gather(void* cre, std::vector<InvItem>& pack, InvItem equipped[ES_TOTAL]) {
    pack.clear();
    for (int i = 0; i < ES_TOTAL; i++) equipped[i] = InvItem();
    if (!cre) return;
    uint32_t cid = Id(cre);
    for (int i = 0; i < ES_TOTAL; i++) {
        uint32_t id = EquippedId(cre, i);
        if (id != 0x7f000000 && Fill(id, equipped[i])) equipped[i].equippedSlot = i;
    }
    ForEachItem([&](void* it) {
        if (F<uint32_t>(it, IT_Possessor) != cid) return;
        if (F<uint32_t>(it, IT_EquippedBy) == cid) return;
        if (F<int>(it, IT_InContainer) != 0) return;
        uint32_t id = F<uint32_t>(it, IT_Id);
        for (int i = 0; i < ES_TOTAL; i++) if (equipped[i].id == id) return;
        InvItem ii;
        if (Fill(id, ii)) pack.push_back(ii);
    });
    SortByGrid(pack);
}

// ---- containers (chests, corpses, bags on the ground) ----
void* InventoryPanel() {
    if (!App()) return nullptr;
    typedef void* (*fp)(void*);
    return ((fp)A(VA_App_GetInventoryPanel))(App());
}

uint32_t OpenContainerId() {
    void* p = InventoryPanel();
    if (!p || !Readable((char*)p + 0x2f64, 8)) return 0x7f000000;
    if (F<int>(p, PI_ContainerOpen) == 0) return 0x7f000000;
    return F<uint32_t>(p, PI_ContainerId);
}

void GatherContainer(void* cre, std::vector<InvItem>& out) {
    out.clear();
    if (!cre) return;
    uint32_t cid = Id(cre);
    ForEachItem([&](void* it) {
        if (F<int>(it, IT_InContainer) != 1) return;
        if (F<uint32_t>(it, IT_Possessor) != cid) return;
        InvItem ii;
        if (Fill(F<uint32_t>(it, IT_Id), ii)) out.push_back(ii);
    });
    SortByGrid(out);
}

void* Message();
static void* Msg() { return Message(); }
void* Message() {
    if (!App()) return nullptr;
    typedef void* (*fm)(void*);
    return ((fm)A(VA_App_GetNWCMessage))(App());
}

void TakeItem(uint32_t id) {
    void* m = Msg();
    if (!m) return;
    Log("take %08x", id);
    typedef void (*fn)(void*, uint32_t, uint8_t, uint8_t, uint32_t);
    ((fn)A(VA_Msg_RepositoryMove))(m, id, 0xff, 0xff, 0x7f000000);
}

void CloseContainer() {
    void* p = InventoryPanel();
    if (!p) return;
    Log("close container");
    typedef void (*fn)(void*);
    ((fn)A(VA_PanelInv_HandleXButton))(p);
}

void ContainerPage(int dir) {
    void* m = Msg();
    if (!m) return;
    typedef void (*fn)(void*, uint8_t, uint8_t, void*, uint32_t);
    ((fn)A(VA_Msg_SendToServer))(m, 0x19, dir > 0 ? 2 : 3, nullptr, 0);
}

int BestSlot(void* cre, const InvItem& it) {
    uint32_t m = it.equipMask & ((1u << ES_COUNT_PLAYER) - 1);
    if (!m) return -1;
    // rings: prefer a free ring slot
    if ((m & (1u << ES_LRING)) && (m & (1u << ES_RRING))) {
        if (EquippedId(cre, ES_LRING) == 0x7f000000) return ES_LRING;
        if (EquippedId(cre, ES_RRING) == 0x7f000000) return ES_RRING;
        return ES_LRING;
    }
    if (m & (1u << ES_RHAND)) return ES_RHAND;
    for (int i = 0; i < ES_COUNT_PLAYER; i++) if (m & (1u << i)) return i;
    return -1;
}

void Equip(void* cre, const InvItem& it, int slot) {
    if (!cre) return;
    if (slot < 0) slot = BestSlot(cre, it);
    if (slot < 0) return;
    Log("equip %08x '%s' slot %d", it.id, it.name.c_str(), slot);
    typedef void (*fn)(void*, uint32_t, uint32_t);
    ((fn)A(VA_PerformEquipItem))(cre, it.id, 1u << slot);
}

void UnEquip(void* cre, uint32_t id) {
    if (!cre) return;
    Log("unequip %08x", id);
    typedef void (*fn)(void*, uint32_t);
    ((fn)A(VA_PerformUnEquipItem))(cre, id);
}

void Examine(void* cre, uint32_t id) {
    if (!cre) return;
    Log("examine %08x", id);
    typedef void (*fn)(void*, uint32_t, void*, void*);
    ((fn)A(VA_PerformExamine))(cre, id, nullptr, nullptr);
}

void SetGuiOpen(void* cre, bool open) {
    if (!cre || !App()) return;
    typedef void* (*fm)(void*);
    void* msg = ((fm)A(VA_App_GetNWCMessage))(App());
    if (!msg) return;
    if (open) {
        // repository updates are tagged with the inventory panel's character id
        typedef void* (*fp)(void*);
        void* panel = ((fp)A(VA_App_GetInventoryPanel))(App());
        if (panel && Readable((char*)panel + 0x2f68, 4)) {
            Log("inv panel %p char id %08x -> %08x", panel, F<uint32_t>(panel, 0x2f68), Id(cre));
            F<uint32_t>(panel, 0x2f68) = Id(cre);
        }
    }
    Log("inventory gui status %d", open);
    typedef void (*fn)(void*, int, uint32_t);
    ((fn)A(VA_Msg_GuiInventoryStatus))(msg, open ? 1 : 0, Id(cre));
}

void SetCharSheetOpen(void* cre, bool open) {
    if (!cre || !App()) return;
    typedef void* (*fm)(void*);
    void* msg = ((fm)A(VA_App_GetNWCMessage))(App());
    if (!msg) return;
    Log("char sheet status %d", open);
    typedef void (*fn)(void*, char, uint32_t);
    ((fn)A(VA_Msg_GuiCharSheetStatus))(msg, open ? 0 : -1, Id(cre));
}

void Drop(void* cre, uint32_t id) {
    if (!cre) return;
    Log("drop %08x", id);
    typedef void (*fn)(void*, uint32_t);
    ((fn)A(VA_PerformDropItem))(cre, id);
}

void OpenRadial(uint32_t id) {
    void* it = ItemById(id);
    if (!it) return;
    Log("item radial %08x", id);
    typedef void (*fn)(void*, int);
    ((fn)A(VA_Item_ActivateRadial))(it, 0);
}

const char* SlotName(int s) {
    static const char* n[] = {"Head", "Armour", "Boots", "Gloves", "Main hand", "Off hand", "Cloak", "Left ring",
                              "Right ring", "Amulet", "Belt", "Arrows", "Bullets", "Bolts"};
    return s >= 0 && s < ES_COUNT_PLAYER ? n[s] : "";
}

uint32_t IconTex(const std::string& r) { return r.empty() ? 0 : Res_Texture(r.c_str()); }

void DrawIcon(const InvItem& it, float x, float y, float size, float a) {
    bool any = false;
    if (it.plt && !it.pltParts.empty()) {
        int w = 0, h = 0;
        uint32_t t = Res_PltTexture(it.pltParts, it.pltColors, &w, &h);
        if (t) {
            float aw = (float)(w > 0 ? w : 1), ah = (float)(h > 0 ? h : 1);
            float s = size / (aw > ah ? aw : ah);
            float dw = aw * s, dh = ah * s;
            R::Image(t, x + (size - dw) * 0.5f, y + (size - dh) * 0.5f, dw, dh, Col(255, 255, 255, (int)(255 * a)));
            return;
        }
    }
    for (int i = 0; i < it.iconLayers; i++) {
        int w = 0, h = 0;
        uint32_t t = Res_Texture(it.icon[i].c_str(), &w, &h);
        if (!t) continue;
        // keep aspect (icons are 32x32 .. 64x128); fit into the square
        float aw = (float)(w > 0 ? w : 1), ah = (float)(h > 0 ? h : 1);
        float s = size / (aw > ah ? aw : ah);
        float dw = aw * s, dh = ah * s;
        R::Image(t, x + (size - dw) * 0.5f, y + (size - dh) * 0.5f, dw, dh, Col(255, 255, 255, (int)(255 * a)));
        any = true;
    }
    if (!any) {
        R::RoundRect(x + size * 0.1f, y + size * 0.1f, size * 0.8f, size * 0.8f, size * 0.12f, Col(60, 52, 42, (int)(200 * a)));
        std::string l = it.name.empty() ? "?" : it.name.substr(0, 1);
        R::TextCentered(FONT_TITLE, size * 0.45f, x + size * 0.5f, y + size * 0.22f, l.c_str(), Col(210, 180, 120, (int)(255 * a)));
    }
}

CharInfo Character(void* cre) {
    CharInfo c;
    if (!cre) return c;
    void* st = F<void*>(cre, OFS::Cre_Stats);
    if (!st || !Readable(st, 0x300)) return c;
    void* rules = Rules();
    c.name = GD::ObjName(Id(cre));
    {
        Exo s; typedef void* (*fn)(void*, Exo*, char);
        ((fn)A(VA_Stats_ClassesString))(st, &s, 0);
        c.classes = TakeExo(s);
    }
    {
        Exo s; typedef void* (*fn)(void*, Exo*);
        ((fn)A(VA_Stats_RaceString))(st, &s);
        c.race = TakeExo(s);
    }
    typedef int (*fl)(void*);
    c.level = (uint8_t)((fl)A(VA_Stats_GetLevel))(st);
    c.xp = F<int>(st, 0x94);
    if (rules && c.level > 0) c.xpCur = F<int>(rules, 0x28 + (c.level - 1) * 4);
    c.xpNext = ((fl)A(VA_Stats_ExpNeeded))(st);
    typedef short (*fac)(void*, int);
    typedef int (*fcm)(void*, void*);
    c.ac = ((fac)A(VA_Stats_GetArmorClass))(st, 0) + ((fcm)A(VA_Stats_CombatModeAC))(st, cre);
    c.bab = (signed char)((fl)A(VA_Stats_BAB))(st);
    // abilities: base + bonus + racial + class adjustments
    static const int baseOff[6] = {0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b};
    static const int bonusOff[6] = {0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82};
    static const int raceOff[6] = {0x18, 0x19, 0x1d, 0x1a, 0x1c, 0x1b};
    uint16_t race = F<uint16_t>(st, 0x50);
    char* races = rules ? F<char*>(rules, 0x118) : nullptr;
    typedef char (*fadj)(void*, int);
    for (int i = 0; i < 6; i++) {
        int v = (signed char)F<char>(st, baseOff[i]) + (signed char)F<char>(st, bonusOff[i]);
        if (races) v += (signed char)races[race * 0x68 + raceOff[i]];
        v += ((fadj)A(VA_Stats_ClassAbilityAdjust))(st, i);
        c.ab[i] = v < 3 ? 3 : v;
    }
    GD::HP h = GD::CreatureHP(cre);
    c.hp = h.cur; c.hpMax = h.max;
    c.gold = Gold(cre);
    c.ok = true;
    return c;
}
}  // namespace Inv
