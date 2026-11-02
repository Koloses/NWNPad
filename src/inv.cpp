// NWNPad - client-side inventory / character data and actions
#include "inv.h"
#include "game.h"
#include "gamedata.h"
#include "text.h"
#include "res.h"
#include "render.h"
#include "log.h"
#include "twoda.h"
#include "hooks.h"
#include <windows.h>
#include <cstring>
#include <set>
#include <map>
#include <algorithm>

using namespace G;

namespace {
// CNWCCreature (Win)
const uintptr_t VA_PerformEquipItem = 0x1408050d0;     // (cre, itemId, slotMask)
const uintptr_t VA_PerformUnEquipItem = 0x140805690;   // (cre, itemId)
const uintptr_t VA_PerformDropItem = 0x140804ed0;      // (cre, itemId)
const uintptr_t VA_PerformExamine = 0x140805290;       // (cre, id, 0, 0)
const uintptr_t VA_Item_ActivateRadial = 0x14083ce30;  // (item, int)
const uintptr_t VA_Item_GetName = 0x14083d8a0;         // (item, CExoString*, int)
const uintptr_t VA_App_GetNWCMessage = 0x1406abf90;    // (app)
const uintptr_t VA_App_GetInventoryPanel = 0x1406abbe0; // (app)
const uintptr_t VA_Msg_RepositoryMove = 0x1407c1030;   // (msg, item, x, y, repository)
const uintptr_t VA_Msg_SendToServer = 0x1407c25c0;     // (msg, major, minor, data, size)
const uintptr_t VA_Msg_SplitItem = 0x1407c0050;        // (msg, item, count)
const uintptr_t VA_Msg_MergeItem = 0x1407bfd90;        // (msg, item in place, dropped item)
const uintptr_t VA_Msg_UseObject = 0x1407c07c0;        // (msg, object, 0, 0): opens a bag
const uintptr_t VA_PanelInv_HandleXButton = 0x140856420;   // (panel)
const uintptr_t VA_Msg_GuiInventoryStatus = 0x1407be110;   // (msg, open, creatureId)
const uintptr_t VA_Msg_GuiCharSheetStatus = 0x1407bddd0;   // (msg, char page (-1 close), creatureId)
// CNWCCreatureStats
const uintptr_t VA_Stats_ClassAbilityAdjust = 0x1407f1110;   // (stats, ability) -> char
const uintptr_t VA_Stats_GetArmorClass = 0x1407ef9c0;        // (stats, int)
const uintptr_t VA_Stats_CombatModeAC = 0x1407f1ed0;         // (stats, creature)
const uintptr_t VA_Stats_BAB = 0x1407f0900;                  // (stats)
const uintptr_t VA_Stats_ClassesString = 0x1407f1230;        // (stats, CExoString*, char)
const uintptr_t VA_Stats_RaceString = 0x1407f67c0;           // (stats, CExoString*)
const uintptr_t VA_Stats_ExpNeeded = 0x1407f3da0;            // (stats)
const uintptr_t VA_Stats_GetLevel = 0x1407f5d70;

// CNWCItem field offsets (Win)
const size_t IT_BaseItem = 0x40, IT_Id = 0x50, IT_Identified = 0x1c0, IT_Icon = 0x238;
const size_t IT_EquippedBy = 0x25c, IT_Possessor = 0x264, IT_Stack = 0x284;
const size_t IT_GridX = 0x254, IT_GridY = 0x255;
const size_t IT_InContainer = 0x268, IT_Stolen = 0x1c4, IT_Cost = 0x280, IT_Usable = 0x288;
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
    if (id == 0x7f000000 || !App() || G::Loading()) return nullptr;
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

// single player: the server's CNWSCreature::CanUseItem for the player; multiplayer: the flag the server
// sends with each item's properties (the original inventory paints such items red)
bool CanUse(uint32_t id) {
    typedef void* (*Get_t)(void*, uint32_t);
    static HMODULE exe = GetModuleHandleA(nullptr);
    static auto fItem = (Get_t)GetProcAddress(exe, "?GetItemByGameObjectID@CServerExoApp@@QEAAPEAVCNWSItem@@I@Z");
    static auto fCre = (Get_t)GetProcAddress(exe, "?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z");
    static auto fCanUse = (int (*)(void*, void*, int))GetProcAddress(exe, "?CanUseItem@CNWSCreature@@QEAAHPEAVCNWSItem@@H@Z");
    static void** am = (void**)GetProcAddress(exe, "?g_pAppManager@@3PEAVCAppManager@@EA");
    void* pl = Player();
    if (!pl) return true;
    void* srv = am && *am ? F<void*>(*am, 8) : nullptr;
    if (srv && F<void*>(srv, 8) && !Loading() && fItem && fCre && fCanUse) {
        void* si = fItem(srv, id & 0x7fffffffu);
        void* sc = fCre(srv, Id(pl) & 0x7fffffffu);
        if (si && sc) return fCanUse(sc, si, 1) != 0;
    }
    void* it = ItemById(id);
    return !it || F<int>(it, IT_Usable) != 0;
}

// weight in tenths of a pound: the server's CNWSItem::GetWeight in single player (properties, armour AC),
// else the base item's weight times the stack
static int ItemWeight(uint32_t id, int baseItem, int stack) {
    typedef void* (*Get_t)(void*, uint32_t);
    static HMODULE exe = GetModuleHandleA(nullptr);
    static auto fItem = (Get_t)GetProcAddress(exe, "?GetItemByGameObjectID@CServerExoApp@@QEAAPEAVCNWSItem@@I@Z");
    static auto fWeight = (int (*)(void*))GetProcAddress(exe, "?GetWeight@CNWSItem@@QEAAHXZ");
    static void** am = (void**)GetProcAddress(exe, "?g_pAppManager@@3PEAVCAppManager@@EA");
    void* srv = am && *am ? F<void*>(*am, 8) : nullptr;
    if (srv && F<void*>(srv, 8) && !Loading() && fItem && fWeight) {
        void* si = fItem(srv, id & 0x7fffffffu);
        if (si) return fWeight(si);
    }
    const TwoDA& t = TDA("baseitems");
    int w = t.Int(baseItem, "TenthLBS", 0);
    return (w > 0 ? w : 0) * (stack > 0 ? stack : 1);
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
    out.usable = CanUse(id);
    out.weight = ItemWeight(id, out.baseItem, out.stack);
    {
        const TwoDA& t = TDA("baseitems");
        out.invW = std::max(1, std::min(4, t.Int(out.baseItem, "InvSlotWidth", 1)));
        out.invH = std::max(1, std::min(4, t.Int(out.baseItem, "InvSlotHeight", 1)));
    }
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
        if (a.page != b.page) return a.page < b.page;
        if (a.gy != b.gy) return a.gy < b.gy;
        return a.gx < b.gx;
    });
}

// ---- backpack pages ----
void* InventoryPanel();
namespace {
const int kPages = 6;                    // the creature repository is 10 x 36, sent as pages of 10 x 6
struct PageCache { bool seen = false; };
PageCache g_pages[kPages];
int g_curPage = -1, g_wantPage = -1;
double g_reqTime = 0;
bool g_rescan = false;
std::set<uint32_t> g_lastSet;
std::map<uint32_t, int> g_pageOf;       // page an item was first sent with
double g_lastChange = 0;                // when the shown items last changed: a page has arrived once they hold still
uint32_t g_pagesFor = 0;
bool g_invOpen = false;
double NowS() { return GetTickCount64() / 1000.0; }
void ResetPages() { for (auto& p : g_pages) p = PageCache(); g_pageOf.clear(); g_curPage = -1; g_wantPage = -1; }
void RequestPage(int p) {
    void* panel = InventoryPanel();
    if (!panel || p < 0 || p >= kPages) return;
    typedef void (*fn)(void*, int);
    ((fn)A(0x1408562e0))(panel, p);              // CPanelInventory::HandleSelect: the server switches pages
    g_wantPage = p; g_reqTime = NowS();
    g_lastSet.clear(); g_lastChange = g_reqTime;
}
}  // namespace

void RescanPages() { g_rescan = true; }
bool PagesComplete() { for (auto& p : g_pages) if (!p.seen) return false; return true; }
void Gather(void* cre, std::vector<InvItem>& pack, InvItem equipped[ES_TOTAL]) {
    pack.clear();
    for (int i = 0; i < ES_TOTAL; i++) equipped[i] = InvItem();
    if (!cre) return;
    uint32_t cid = Id(cre);
    for (int i = 0; i < ES_TOTAL; i++) {
        uint32_t id = EquippedId(cre, i);
        if (id != 0x7f000000 && Fill(id, equipped[i])) equipped[i].equippedSlot = i;
    }
    // single player: the server's item says who holds it (the client keeps an item of a backpack page it is
    // not being sent any more as it was: dropped, sold, given or used up)
    static HMODULE exe = GetModuleHandleA(nullptr);
    static auto fSrvItem = (void* (*)(void*, uint32_t))GetProcAddress(exe, "?GetItemByGameObjectID@CServerExoApp@@QEAAPEAVCNWSItem@@I@Z");
    static void** am = (void**)GetProcAddress(exe, "?g_pAppManager@@3PEAVCAppManager@@EA");
    void* srv = am && *am ? F<void*>(*am, 8) : nullptr;
    bool local = srv && F<void*>(srv, 8) && fSrvItem && !Loading();
    ForEachItem([&](void* it) {
        if (F<uint32_t>(it, IT_Possessor) != cid) return;
        if (F<uint32_t>(it, IT_EquippedBy) == cid) return;
        if (F<int>(it, IT_InContainer) != 0) return;
        if (F<void*>(it, OFS::Obj_Area)) return;                  // dropped: lying in the area now
        uint32_t id = F<uint32_t>(it, IT_Id);
        if (local) {
            void* si = fSrvItem(srv, id & 0x7fffffffu);
            if (!si || F<uint32_t>(si, 0x3c8) != (cid & 0x7fffffffu)) return;   // CNWSItem possessor
        }
        for (int i = 0; i < ES_TOTAL; i++) if (equipped[i].id == id) return;
        InvItem ii;
        if (!Fill(id, ii)) return;
        // creature skins / claws (e.g. the 1.69 horse scripts' "PC Properties" hide) are not the player's items
        if ((ii.equipMask & 0x3C000) && !(ii.equipMask & 0x3FFF)) return;
        pack.push_back(ii);
    });
    // pages: the server sends the page the panel shows and the client keeps what it has been sent, so walking
    // through all six once when the inventory opens brings in every item; then back to the first page
    if (g_invOpen) {
        if (Id(cre) != g_pagesFor) { g_pagesFor = Id(cre); ResetPages(); }
        double now = NowS();
        if (g_wantPage >= 0) {
            std::set<uint32_t> ids; for (auto& it : pack) ids.insert(it.id);
            if (ids != g_lastSet) { g_lastSet = ids; g_lastChange = now; }
            bool still = now - g_lastChange > (local ? 0.25 : 0.5) && now - g_reqTime > (local ? 0.3 : 0.6);
            if (still || now - g_reqTime > 2.5) {
                g_curPage = g_wantPage; g_wantPage = -1;
                g_pages[g_curPage].seen = true;
                for (auto& it : pack) if (!g_pageOf.count(it.id)) g_pageOf[it.id] = g_curPage;
            }
        }
        if (g_wantPage < 0) {
            int next = -1;
            for (int p = 0; p < kPages && next < 0; p++) if (!g_pages[p].seen) next = p;
            if (next >= 0) RequestPage(next);
            else if (g_curPage != 0) RequestPage(0);
            // online an item that left another page (given away, used up) is only taken off the client's list
            // when that page is sent again: walk the pages again after the hero did something with an item
            else if (g_rescan && !local) { for (auto& p : g_pages) p.seen = false; g_rescan = false; }
            else g_rescan = false;
        }
    }
    for (auto& it : pack) { auto pg = g_pageOf.find(it.id); it.page = pg != g_pageOf.end() ? pg->second : 0; }
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

// gold taken from a container becomes coins at once on the server, but the client keeps the gold
// item it was shown as: remember what was taken so it is not listed again
static std::set<uint32_t> g_takenGold;

// The client marks an item it was shown in a container as "in a container", possessed by the viewer, and
// keeps it after the container is closed without saying which container it was. Opening a container makes
// the server send its items again (as new), so the container open at the time an item arrives is its own.
static std::map<uint32_t, uint32_t> g_itemContainer;     // item id -> container it was last shown in
typedef int (*ItemAdd_t)(void*, int, void**);
static ItemAdd_t o_ItemAdd = nullptr;
static int hk_ItemAdd(void* msg, int a, void** out) {
    int r = o_ItemAdd(msg, a, out);
    uint32_t c = OpenContainerId();
    if (out && *out && c != 0x7f000000) g_itemContainer[F<uint32_t>(*out, IT_Id)] = c;
    return r;
}
void InstallHooks() {
    o_ItemAdd = (ItemAdd_t)Hook_Create(0x14079fe30, (void*)hk_ItemAdd, "HandleServerToPlayerItemUpdate_Add");
}

void GatherContainer(void* cre, std::vector<InvItem>& out) {
    out.clear();
    if (!cre) return;
    uint32_t cid = Id(cre);
    ForEachItem([&](void* it) {
        if (F<int>(it, IT_InContainer) != 1) return;
        // (one you put in is left with no possessor)
        uint32_t pos = F<uint32_t>(it, IT_Possessor);
        if (pos != cid && pos != 0x7f000000) return;
        if (g_takenGold.count(F<uint32_t>(it, IT_Id))) return;
        auto ic = g_itemContainer.find(F<uint32_t>(it, IT_Id));
        if (o_ItemAdd && (ic == g_itemContainer.end() || ic->second != OpenContainerId())) return;   // shown in another container
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
    if (void* it = ItemById(id)) if (F<int>(it, IT_BaseItem) == 76) g_takenGold.insert(id);
    typedef void (*fn)(void*, uint32_t, uint8_t, uint8_t, uint32_t);
    ((fn)A(VA_Msg_RepositoryMove))(m, id, 0xff, 0xff, 0x7f000000);
}

void PutItem(uint32_t id) {
    void* m = Msg();
    uint32_t c = OpenContainerId();
    if (!m || c == 0x7f000000) return;
    Log("put %08x into container %08x", id, c);
    g_itemContainer[id] = c;
    typedef void (*fn)(void*, uint32_t, uint8_t, uint8_t, uint32_t);
    ((fn)A(VA_Msg_RepositoryMove))(m, id, 0xff, 0xff, c);
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
    g_invOpen = open;
    ResetPages();                                         // rescan: things may have moved since
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
    RescanPages();
    Log("drop %08x", id);
    typedef void (*fn)(void*, uint32_t);
    ((fn)A(VA_PerformDropItem))(cre, id);
}

// "Use" without the radial menu: potions are drunk straight away, other items with one usable property
// (wands, scrolls, kits, rods) start the game's item target mode, as clicking the property in the radial does
int ActiveProperties(void* item) { return item && Readable((char*)item + 0x1f4, 12) ? F<int>(item, 0x1f4) : 0; }
void UsePower(uint32_t id, int index) {
    typedef void* (*fnew)(size_t);
    typedef void* (*fctor)(void*);
    typedef void (*fact)(void*, uint32_t, uint64_t, char*);
    void* t = ((fnew)A(0x140c10324))(0x168);                  // CNWCActionTarget (item property)
    if (!t) return;
    t = ((fctor)A(0x1407037f0))(t);
    char resref[32] = {};
    Log("use %08x power %d: item target mode", id, index);
    ((fact)A(0x1407039f0))(t, id, (uint64_t)index, resref);
}
bool UseItem(void* cre, uint32_t id) {
    void* it = ItemById(id);
    if (!cre || !it) return false;
    int n = ActiveProperties(it);
    if (n != 1) { Log("use %08x: %d usable properties, radial", id, n); return false; }
    int base = F<int>(it, 0x40);
    if (base == 49) {                // potion: always on the drinker
        float pos[3] = {0, 0, 0};
        Log("drink %08x", id);
        typedef void (*fn)(void*, uint32_t, uint32_t, uint32_t, float*);
        ((fn)A(0x140805740))(cre, id, 0u | (0xffu << 16), Id(cre), pos);
        return true;
    }
    UsePower(id, 0);
    return true;
}

// active properties: CNWItemProperty[ +0x1f4 count, +0x1f8 array ], 0x30 each: +0 property (itempropdef.2da),
// +2 subtype (cast spell: iprp_spells.2da)
std::vector<std::string> Powers(uint32_t id) {
    std::vector<std::string> v;
    void* it = ItemById(id);
    int n = ActiveProperties(it);
    const uint8_t* a = n > 0 ? F<const uint8_t*>(it, 0x1f8) : nullptr;
    if (!a || !Readable(a, n * 0x30)) return v;
    for (int i = 0; i < n; i++) {
        int prop = F<uint16_t>(a + i * 0x30, 0), sub = F<uint16_t>(a + i * 0x30, 2);
        std::string name;
        if (prop == 15) name = Tlk((uint32_t)TDA("iprp_spells").Int(sub, "Name", 0));          // cast spell
        if (name.empty()) {
            const TwoDA& d = TDA("itempropdef");
            name = Tlk((uint32_t)d.Int(prop, "GameStrRef", d.Int(prop, "Name", 0)));
        }
        if (name.empty()) name = "Power " + std::to_string(i + 1);
        v.push_back(name);
    }
    return v;
}

void OpenRadial(uint32_t id) {
    void* it = ItemById(id);
    if (!it) return;
    Log("item radial %08x", id);
    typedef void (*fn)(void*, int);
    ((fn)A(VA_Item_ActivateRadial))(it, 0);
}

void Split(uint32_t id, int count) {
    void* m = Msg();
    if (!m || count < 1) return;
    Log("split %08x %d", id, count);
    typedef void (*fn)(void*, uint32_t, uint16_t);
    ((fn)A(VA_Msg_SplitItem))(m, id, (uint16_t)count);
}

void Merge(uint32_t into, uint32_t from) {
    void* m = Msg();
    if (!m || into == from) return;
    Log("merge %08x <- %08x", into, from);
    typedef void (*fn)(void*, uint32_t, uint32_t);
    ((fn)A(VA_Msg_MergeItem))(m, into, from);
}

void OpenBag(uint32_t id) {
    void* m = Msg();
    if (!m) return;
    Log("open bag %08x", id);
    typedef void (*fn)(void*, uint32_t, int, int);
    ((fn)A(VA_Msg_UseObject))(m, id, 0, 0);
}

void MoveToBag(uint32_t id, uint32_t bag) {
    void* m = Msg();
    if (!m) return;
    Log("move %08x to bag %08x", id, bag);
    typedef void (*fn)(void*, uint32_t, uint8_t, uint8_t, uint32_t);
    ((fn)A(VA_Msg_RepositoryMove))(m, id, 0xff, 0xff, bag);
}

bool IsContainer(const InvItem& it) {
    return it.baseItem >= 0 && TDA("baseitems").Int(it.baseItem, "Container", 0) != 0;
}

int MaxStack(const InvItem& it) {
    return it.baseItem >= 0 ? std::max(1, TDA("baseitems").Int(it.baseItem, "Stacking", 1)) : 1;
}

const char* SlotName(int s) {
    static const char* n[] = {"Head", "Armour", "Boots", "Gloves", "Main hand", "Off hand", "Cloak", "Left ring",
                              "Right ring", "Amulet", "Belt", "Arrows", "Bullets", "Bolts"};
    return s >= 0 && s < ES_COUNT_PLAYER ? n[s] : "";
}

uint32_t IconTex(const std::string& r) { return r.empty() ? 0 : Res_Texture(r.c_str()); }

void DrawIcon(const InvItem& it, float x, float y, float size, float a) {
    bool any = false;
    if (!it.usable) R::RoundRect(x, y, size, size, size * 0.12f, Col(150, 28, 22, (int)(150 * a)));   // like the original: red = can't use
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
    // the character sheet's own data: totals at +0x70, item / effect bonuses at +0x7d
    if (F<uint8_t>(st, 0x70) > 0) {
        c.sheet = true;
        for (int i = 0; i < 6; i++) {
            c.ab[i] = F<uint8_t>(st, 0x70 + i);
            c.abBase[i] = c.ab[i] - (signed char)F<char>(st, 0x7d + i);
        }
    } else for (int i = 0; i < 6; i++) c.abBase[i] = c.ab[i] - (signed char)F<char>(st, bonusOff[i]);
    c.fort = (signed char)F<char>(st, 0x98);
    c.will = (signed char)F<char>(st, 0x99);
    c.ref = (signed char)F<char>(st, 0x9a);
    {
        short lc = F<short>(st, 0xc6), ge = F<short>(st, 0xc8);
        int ref = lc < 31 ? (ge < 31 ? 0x1359 : ge > 69 ? 0x1353 : 0x1356)
                : lc < 70 ? (ge == 50 ? 0x1355 : ge < 31 ? 0x1358 : ge > 69 ? 0x1352 : 0x1355)
                : (ge < 31 ? 0x1357 : ge < 70 ? 0x1354 : 0x1351);
        c.alignment = Tlk(ref);
        bool up = true;
        for (auto& ch : c.alignment) { if (up && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32); up = ch == ' '; }
    }
    // attacks: base attack per attack minus the iterative penalty, plus the hand's modifier, the combat mode
    // and the unconditional bonuses (as CNWCCreatureStats::GetGeneralAttackBonusString)
    if (char* ci = F<char*>(st, 0xa8)) {
        c.sr = (uint8_t)ci[5]; c.asf = (uint8_t)ci[6]; c.acp = (uint8_t)ci[7];
        typedef int (*fi)(void*, int);
        typedef int (*f1)(void*);
        typedef int (*fm)(void*, void*);
        int mode = ((fm)A(0x1407f1f60))(st, cre);
        auto general = [&](bool off) {
            int sum = 0;
            void** nodes = F<void**>(ci, 0x20); int n = F<int>(ci, 0x28);
            for (int i = 0; nodes && i < n && i < 64; i++) {
                char* nd = (char*)nodes[i];
                if (!nd) continue;
                uint8_t t = (uint8_t)nd[2];
                bool kind = (uint8_t)(t - 1) < 2 || (uint8_t)(t - 6) < 2;
                if (!kind || (uint8_t)nd[5] != 0x1c || nd[3] || nd[4]) continue;
                if (off ? t == 1 : t == 2) continue;
                sum += nd[0];
            }
            return sum;
        };
        auto list = [&](int count, int handMod, bool off) {
            std::string r;
            int add = handMod + mode + general(off);
            for (int i = 0; i < count && i < 8; i++) {
                int v = ((fi)A(0x1407f6590))(st, i) + add;
                char b[16]; snprintf(b, sizeof b, "%s%+d", i ? "/" : "", v);
                r += b;
            }
            return r;
        };
        c.attack = list(((f1)A(0x1407f66c0))(st), (signed char)ci[1], false);
        int off = ((f1)A(0x1407f6650))(st);
        if (off > 0) c.attackOff = list(off, (signed char)ci[3], true);
    }
    c.ok = true;
    return c;
}

std::vector<SkillInfo> Skills(void* cre) {
    std::vector<SkillInfo> v;
    void* st = cre ? F<void*>(cre, OFS::Cre_Stats) : nullptr;
    void* rules = Rules();
    if (!st || !rules) return v;
    int n = F<uint8_t>(rules, 0x107);
    uint8_t* ranks = F<uint8_t*>(st, 0xd0);
    const TwoDA& t = TDA("skills");
    typedef char (*fm)(void*, uint16_t);
    // single player: the server's own numbers (feats, items and effects included)
    static HMODULE exe = GetModuleHandleA(nullptr);
    static auto fCre = (void* (*)(void*, uint32_t))GetProcAddress(exe, "?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z");
    static auto fRank = (char (*)(void*, uint8_t, void*, int))GetProcAddress(exe, "?GetSkillRank@CNWSCreatureStats@@QEAADEPEAVCNWSObject@@H@Z");
    static void** am = (void**)GetProcAddress(exe, "?g_pAppManager@@3PEAVCAppManager@@EA");
    void* srv = am && *am ? F<void*>(*am, 8) : nullptr;
    void* sst = nullptr;
    if (srv && F<void*>(srv, 8) && !Loading() && fCre && fRank)
        if (void* sc = fCre(srv, Id(cre) & 0x7fffffffu)) sst = F<void*>(sc, 0xad0);
    for (int i = 0; i < n && i < 64; i++) {
        SkillInfo s;
        s.id = i;
        if (sst) {
            s.rank = fRank(sst, (uint8_t)i, nullptr, 1);
            s.total = fRank(sst, (uint8_t)i, nullptr, 0);
        } else {
            s.rank = ranks && !F<int>(st, 0x110) ? ranks[i] : 0;
            s.total = s.rank + ((fm)A(0x1407f6a10))(st, (uint16_t)i);
        }
        int nm = t.Int(i, "Name", -1);
        if (nm < 0) continue;
        s.name = Tlk(nm);
        int ds = t.Int(i, "Description", -1);
        if (ds >= 0) s.desc = Tlk(ds);
        s.trainedOnly = t.Int(i, "Untrained", 1) == 0;
        if (s.name.empty() || (s.trainedOnly && s.rank == 0)) continue;
        v.push_back(s);
    }
    std::sort(v.begin(), v.end(), [](const SkillInfo& a, const SkillInfo& b) { return a.name < b.name; });
    return v;
}

std::vector<FeatInfo> Feats(void* cre) {
    std::vector<FeatInfo> v;
    void* st = cre ? F<void*>(cre, OFS::Cre_Stats) : nullptr;
    if (!st) return v;
    uint16_t* ids = F<uint16_t*>(st, 0x118);
    int n = F<int>(st, 0x120);
    const TwoDA& t = TDA("feat");
    for (int i = 0; ids && i < n && i < 1000; i++) {
        FeatInfo f; f.id = ids[i];
        int nm = t.Int(f.id, "FEAT", -1);
        if (nm < 0) continue;
        f.name = Tlk(nm);
        int ds = t.Int(f.id, "DESCRIPTION", -1);
        if (ds >= 0) f.desc = Tlk(ds);
        if (!f.name.empty()) v.push_back(f);
    }
    std::sort(v.begin(), v.end(), [](const FeatInfo& a, const FeatInfo& b) { return a.name < b.name; });
    return v;
}
Load Encumbrance(void* cre, int itemsWeight, int str) {
    Load l; l.weight = itemsWeight;
    static HMODULE exe = GetModuleHandleA(nullptr);
    static auto fCre = (void* (*)(void*, uint32_t))GetProcAddress(exe, "?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z");
    static void** am = (void**)GetProcAddress(exe, "?g_pAppManager@@3PEAVCAppManager@@EA");
    void* srv = am && *am ? F<void*>(*am, 8) : nullptr;
    if (cre && srv && F<void*>(srv, 8) && !Loading() && fCre) {
        if (void* sc = fCre(srv, Id(cre) & 0x7fffffffu)) {
            typedef int (*fn)(void*);
            l.weight = ((fn)A(0x140388fa0))(sc);             // CNWSCreature::ComputeTotalWeightCarried
            static auto fStr = (uint8_t (*)(void*))GetProcAddress(exe, "?GetSTRStat@CNWSCreatureStats@@QEAAEXZ");
            void* sst = F<void*>(sc, 0xad0);
            if (sst && fStr) str = fStr(sst);                // what UpdateEncumbranceState uses
            l.state = F<int>(sc, 0x970);
        }
    }
    l.str = str;
    const TwoDA& t = TDA("encumbrance");
    int row = std::max(0, str);
    l.normal = t.Int(row, "Normal", 0);
    l.heavy = t.Int(row, "Heavy", 0);
    return l;
}
}  // namespace Inv
