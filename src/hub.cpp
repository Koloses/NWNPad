// NWNPad - in-game hub menu (Start): inventory, character, and shortcuts to the game's panels
#include "hub.h"
#include "inv.h"
#include "quickbar.h"
#include "describe.h"
#include "hub_pages.h"
#include "uikit.h"
#include "render.h"
#include "game.h"
#include "gamedata.h"
#include "control.h"
#include "pad.h"
#include "config.h"
#include "log.h"
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

using namespace G;
using namespace R;

namespace {
struct Tab { const char* name; int event; const HubPage* page; };   // event 0 = custom screen
const Tab TABS[] = {
    {"Inventory", 0, nullptr}, {"Character", 0, nullptr}, {"Quickbar", 0, nullptr}, {"Spells", 0, &PAGE_SPELLS},
    {"Journal", 0, &PAGE_JOURNAL}, {"Map", 0, &PAGE_MAP}, {"System", 0, &PAGE_SYSTEM},
};
const int NTABS = sizeof(TABS) / sizeof(TABS[0]);

const int GRID_R = 5, GRID_C = 3;
const int GRID[GRID_R][GRID_C] = {
    {ES_HEAD, ES_NECK, ES_CLOAK},
    {ES_RHAND, ES_CHEST, ES_LHAND},
    {ES_LRING, ES_ARMS, ES_RRING},
    {ES_BELT, ES_BOOTS, -1},
    {ES_ARROWS, ES_BOLTS, ES_BULLETS},
};

struct HubState {
    bool open = false;
    int tab = 0;
    double openTime = 0;
    // game panel opened from the hub
    int extTab = -1; double extTime = 0; bool extSeen = false;
    // inventory
    int focus = 0;               // 0 equipment grid, 1 backpack list
    int gr = 1, gc = 0;
    int sel = 0, scroll = 0;
    int filterSlot = -1;
    uint32_t selId = 0;
    std::vector<InvItem> pack;
    InvItem eq[ES_TOTAL];
    double lastGather = -10;
    std::string toast; double toastTime = -10;
    Inv::CharInfo ch; double lastChar = -10;
    int qbBank = 0, qbSlot = 0;
    uint32_t descId = 0; float descScroll = 0;
    int sortMode = 0;            // 0 backpack order, 1 name, 2 type, 3 value, 4 weight
    bool grid = true;            // backpack as a grid of item footprints (like the original) or a list
    // item action menu / split picker
    enum { M_NONE, M_ACTIONS, M_BAGS, M_SPLIT } menu = M_NONE;
    std::vector<int> acts; int actSel = 0;
    uint32_t actItem = 0;
    std::vector<uint32_t> bags; int bagSel = 0;
    int splitN = 1, splitMax = 1;
    // combine stacks: one merge per server round trip
    bool merging = false; int mergeBase = -1; std::string mergeName; double mergeNext = 0, mergeEnd = 0;
} HS;

const char* SORT_NAMES[5] = {"Backpack order", "Name", "Type", "Value", "Weight"};
const int NSORT = 5;
std::vector<int> g_list;     // indices into HS.pack after filtering

// grid view: the items packed in the current order into a grid of cells, each item its footprint
struct Cell { int x, y, w, h; };
std::vector<Cell> g_cells;   // per g_list entry
int g_gridCols = 8, g_gridRows = 0;
void PackGrid(int cols) {
    g_gridCols = cols < 2 ? 2 : cols;
    g_cells.clear();
    std::vector<std::vector<char>> used;
    auto fits = [&](int x, int y, int w, int h) {
        if (x + w > g_gridCols) return false;
        for (int yy = y; yy < y + h; yy++) {
            if (yy >= (int)used.size()) continue;
            for (int xx = x; xx < x + w; xx++) if (used[yy][xx]) return false;
        }
        return true;
    };
    for (int i : g_list) {
        const InvItem& it = HS.pack[i];
        int w = std::min(it.invW, g_gridCols), h = it.invH;
        int px0 = 0, py0 = 0;
        for (int y = 0;; y++) {
            bool done = false;
            for (int x = 0; x + w <= g_gridCols; x++) if (fits(x, y, w, h)) { px0 = x; py0 = y; done = true; break; }
            if (done) break;
        }
        while ((int)used.size() < py0 + h) used.push_back(std::vector<char>(g_gridCols, 0));
        for (int yy = py0; yy < py0 + h; yy++) for (int xx = px0; xx < px0 + w; xx++) used[yy][xx] = 1;
        g_cells.push_back({px0, py0, w, h});
    }
    g_gridRows = (int)used.size();
}
// the item next to the selected one in a direction (dx, dy): nearest centre in that half-plane
int GridStep(int sel, int dx, int dy) {
    if (sel < 0 || sel >= (int)g_cells.size()) return sel;
    const Cell& c = g_cells[sel];
    float cx = c.x + c.w * 0.5f, cy = c.y + c.h * 0.5f;
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < (int)g_cells.size(); i++) {
        if (i == sel) continue;
        const Cell& o = g_cells[i];
        float ox = o.x + o.w * 0.5f - cx, oy = o.y + o.h * 0.5f - cy;
        float along = ox * dx + oy * dy, across = fabsf(ox * dy - oy * dx);
        if (along <= 0.01f) continue;
        // overlapping in the other axis counts as straight ahead
        bool inLine = dx ? (o.y < c.y + c.h && o.y + o.h > c.y) : (o.x < c.x + c.w && o.x + o.w > c.x);
        float d = along + (inLine ? 0.0f : across * 2.0f + 0.5f);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}


void Refresh(bool force = false) {
    if (!force && g_pad.now - HS.lastGather < 0.25) return;
    HS.lastGather = g_pad.now;
    Inv::Gather(Player(), HS.pack, HS.eq);
    if (HS.sortMode) {
        int m = HS.sortMode;
        std::stable_sort(HS.pack.begin(), HS.pack.end(), [m](const InvItem& a, const InvItem& b) {
            if (m == 2 && a.typeName != b.typeName) return a.typeName < b.typeName;
            if (m == 3 && a.cost != b.cost) return a.cost > b.cost;
            if (m == 4 && a.weight != b.weight) return a.weight > b.weight;
            return a.name < b.name;
        });
    }
    g_list.clear();
    for (int i = 0; i < (int)HS.pack.size(); i++) {
        if (HS.filterSlot >= 0 && !(HS.pack[i].equipMask & (1u << HS.filterSlot))) continue;
        g_list.push_back(i);
    }
    // keep selection on the same item
    if (HS.focus == 1 && HS.selId) {
        for (int i = 0; i < (int)g_list.size(); i++) if (HS.pack[g_list[i]].id == HS.selId) { HS.sel = i; break; }
    }
    if (HS.sel >= (int)g_list.size()) HS.sel = (int)g_list.size() - 1;
    if (HS.sel < 0) HS.sel = 0;
}

const InvItem* Selected() {
    if (HS.focus == 0) {
        int s = GRID[HS.gr][HS.gc];
        if (s >= 0 && HS.eq[s].item) return &HS.eq[s];
        return nullptr;
    }
    if (HS.sel >= 0 && HS.sel < (int)g_list.size()) return &HS.pack[g_list[HS.sel]];
    return nullptr;
}

void Toast(const std::string& s) { HS.toast = s; HS.toastTime = g_pad.now; }
}  // namespace
void Hub_Toast(const std::string& s) { Toast(s); }
void Hub_FireEvent(int ev) { HandleInputEvent(ev, 1); HandleInputEvent(ev, 0); }
void Hub_ShowTab(const char* name) {
    extern void Hub_SwitchTo(const char*);
    Hub_SwitchTo(name);
}
namespace {

void FireEvent(int ev) { HandleInputEvent(ev, 1); HandleInputEvent(ev, 0); }

bool g_invStatus = false, g_csStatus = false;
void InvStatus(bool open) {
    if (open == g_invStatus) return;
    g_invStatus = open;
    Inv::SetGuiOpen(Player(), open);
}
// keep the server-side "panel open" states in line with what the hub shows
void SyncStatus() {
    InvStatus(HS.open && HS.tab == 0);
    bool cs = HS.open && HS.tab == 1;
    if (cs != g_csStatus) { g_csStatus = cs; Inv::SetCharSheetOpen(Player(), cs); }
}

void OpenExternal(int t, int event) {
    HS.open = false;
    SyncStatus();
    HS.extTab = t; HS.extTime = g_pad.now; HS.extSeen = false;
    Log("hub: open game panel event %#x (tab %d)", event, t);
    FireEvent(event);
}

void ClosePage() {
    if (HS.open && TABS[HS.tab].page && TABS[HS.tab].page->close) TABS[HS.tab].page->close();
}

void SwitchTab(int t) {
    t = (t + NTABS) % NTABS;
    ClosePage();
    HS.tab = t;
    if (TABS[t].event) { OpenExternal(t, TABS[t].event); return; }
    HS.open = true;
    HS.extTab = -1;
    HS.openTime = g_pad.now;
    { static bool viewSet = false; if (!viewSet) { viewSet = true; HS.grid = g_cfg.inventoryGrid != 0; } }
    SyncStatus();
    HS.lastGather = -10;
    HS.lastChar = -10;
    if (TABS[t].page && TABS[t].page->open) TABS[t].page->open();
}

void CenterMouse() {
    int w, h; Control_GuiSize(w, h);
    Control_SetMouse(w / 2, h / 2);
}

// ---------------------------------------------------------------------------------------------
void MoveGrid(int dr, int dc) {
    int r = HS.gr, c = HS.gc;
    for (int i = 0; i < 4; i++) {
        r += dr; c += dc;
        if (c >= GRID_C) { HS.focus = 1; return; }
        if (r < 0 || r >= GRID_R || c < 0) return;
        if (GRID[r][c] >= 0) { HS.gr = r; HS.gc = c; return; }
    }
}

// ---- item actions ----------------------------------------------------------------------------
enum ItemAct { IA_EQUIP, IA_USE, IA_SPLIT, IA_MERGE, IA_OPEN, IA_TOBAG, IA_DROP, IA_EXAMINE, IA_MORE, IA_GIVE };
const char* ActName(int a) {
    static const char* N[] = {"Equip", "Use", "Split stack", "Combine stacks", "Open", "Put in a bag", "Drop", "Examine", "More actions", "Give to ..."};
    return N[a];
}
const InvItem* FindPack(uint32_t id) {
    for (auto& it : HS.pack) if (it.id == id) return &it;
    for (auto& it : HS.eq) if (it.item && it.id == id) return &it;
    return nullptr;
}
std::vector<const InvItem*> SameStacks(const InvItem& it) {
    std::vector<const InvItem*> v;
    if (Inv::MaxStack(it) <= 1) return v;
    for (auto& o : HS.pack) if (o.id != it.id && o.baseItem == it.baseItem && o.name == it.name && o.identified == it.identified) v.push_back(&o);
    return v;
}
void OpenActions(const InvItem& it) {
    void* pl = Player();
    HS.acts.clear();
    HS.actItem = it.id;
    if (it.equippedSlot < 0) {
        if (Inv::BestSlot(pl, it) >= 0) HS.acts.push_back(IA_EQUIP);
        if (Inv::IsContainer(it)) HS.acts.push_back(IA_OPEN);
        else if (Inv::BestSlot(pl, it) < 0) HS.acts.push_back(IA_USE);
        if (it.stack > 1) HS.acts.push_back(IA_SPLIT);
        if (!SameStacks(it).empty()) HS.acts.push_back(IA_MERGE);
        if (!Inv::IsContainer(it)) for (auto& o : HS.pack) if (Inv::IsContainer(o)) { HS.acts.push_back(IA_TOBAG); break; }
        HS.acts.push_back(IA_GIVE);
        HS.acts.push_back(IA_DROP);
    }
    HS.acts.push_back(IA_EXAMINE);
    HS.acts.push_back(IA_MORE);
    HS.actSel = 0;
    HS.menu = HubState::M_ACTIONS;
}
void DoAction(int a) {
    void* pl = Player();
    const InvItem* it = FindPack(HS.actItem);
    HS.menu = HubState::M_NONE;
    if (!it) return;
    InvItem c = *it;
    switch (a) {
    case IA_EQUIP:
        if (!c.usable) { Toast("You can't use " + c.name); break; }
        Inv::Equip(pl, c, -1); Toast("Equipped " + c.name); break;
    case IA_USE: if (!Inv::UseItem(pl, c.id)) { CenterMouse(); Inv::OpenRadial(c.id); } break;
    case IA_SPLIT:
        HS.splitMax = c.stack - 1; HS.splitN = std::max(1, c.stack / 2);
        HS.menu = HubState::M_SPLIT; return;
    case IA_MERGE:
        HS.merging = true; HS.mergeBase = c.baseItem; HS.mergeName = c.name;
        HS.mergeNext = 0; HS.mergeEnd = g_pad.now + 6.0;
        Toast("Combined " + c.name); break;
    case IA_OPEN: Inv::OpenBag(c.id); Hub_Close(); return;
    case IA_TOBAG:
        HS.bags.clear();
        for (auto& o : HS.pack) if (Inv::IsContainer(o)) HS.bags.push_back(o.id);
        HS.bagSel = 0; HS.menu = HubState::M_BAGS; return;
    case IA_DROP: Inv::Drop(pl, c.id); Toast("Dropped " + c.name); break;
    case IA_EXAMINE: Inv::Examine(pl, c.id); break;
    case IA_GIVE: Hub_Close(); Control_StartGive(c.id, c.name.c_str()); return;
    case IA_MORE: CenterMouse(); Inv::OpenRadial(c.id); break;
    }
    HS.lastGather = g_pad.now - 0.15;
}
// fill the fullest stack that still has room from the smallest one, until nothing moves
void MergeStep() {
    if (!HS.merging) return;
    if (g_pad.now > HS.mergeEnd) { HS.merging = false; return; }
    if (g_pad.now < HS.mergeNext) return;
    std::vector<const InvItem*> v;
    for (auto& o : HS.pack) if (o.baseItem == HS.mergeBase && o.name == HS.mergeName) v.push_back(&o);
    if (v.size() < 2) { HS.merging = false; return; }
    int mx = Inv::MaxStack(*v[0]);
    std::sort(v.begin(), v.end(), [](const InvItem* a, const InvItem* b) { return a->stack > b->stack; });
    const InvItem* into = nullptr;
    for (auto* o : v) if (o->stack < mx) { into = o; break; }
    const InvItem* from = v.back();
    if (!into || into == from) { HS.merging = false; return; }
    Inv::Merge(into->id, from->id);
    HS.mergeNext = g_pad.now + 0.6;
    HS.lastGather = g_pad.now - 0.5;
}

// returns true while a menu has the input
bool MenuInput() {
    if (HS.menu == HubState::M_NONE) return false;
    if (g_pad.Pressed(PB_B)) { HS.menu = HS.menu == HubState::M_ACTIONS ? HubState::M_NONE : HubState::M_ACTIONS; return true; }
    bool up = g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP), down = g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN);
    bool left = g_pad.Repeat(PB_DLEFT) || g_pad.Repeat(PB_LLEFT), right = g_pad.Repeat(PB_DRIGHT) || g_pad.Repeat(PB_LRIGHT);
    if (HS.menu == HubState::M_ACTIONS) {
        int n = (int)HS.acts.size();
        if (up) HS.actSel = (HS.actSel + n - 1) % n;
        if (down) HS.actSel = (HS.actSel + 1) % n;
        if (g_pad.Pressed(PB_A)) DoAction(HS.acts[HS.actSel]);
    } else if (HS.menu == HubState::M_BAGS) {
        int n = (int)HS.bags.size();
        if (n && up) HS.bagSel = (HS.bagSel + n - 1) % n;
        if (n && down) HS.bagSel = (HS.bagSel + 1) % n;
        if (n && g_pad.Pressed(PB_A)) {
            Inv::MoveToBag(HS.actItem, HS.bags[HS.bagSel]);
            const InvItem* b = FindPack(HS.bags[HS.bagSel]);
            Toast(std::string("Put into ") + (b ? b->name : "the bag"));
            HS.menu = HubState::M_NONE; HS.lastGather = g_pad.now - 0.15;
        }
    } else if (HS.menu == HubState::M_SPLIT) {
        int& v = HS.splitN;
        if (right) v++;
        if (left) v--;
        if (up) v += 10;
        if (down) v -= 10;
        if (g_pad.Pressed(PB_RT)) v = HS.splitMax;
        if (g_pad.Pressed(PB_LT)) v = 1;
        v = std::max(1, std::min(HS.splitMax, v));
        if (g_pad.Pressed(PB_A)) {
            Inv::Split(HS.actItem, v);
            Toast("Split off " + std::to_string(v));
            HS.menu = HubState::M_NONE; HS.lastGather = g_pad.now - 0.15;
        }
    }
    return true;
}
void DrawMenu(float x, float y, float w, float h, float a) {
    if (HS.menu == HubState::M_NONE) return;
    const InvItem* it = FindPack(HS.actItem);
    R::Rect(x, y, w, h, Col(0, 0, 0, (int)(120 * a)));
    float mw = px(520);
    std::string title = it ? it->name : std::string();
    if (HS.menu == HubState::M_SPLIT) {
        float mh = px(250), mx = x + (w - mw) * 0.5f, my = y + (h - mh) * 0.5f;
        UIK::Panel(mx, my, mw, mh, px(16), a);
        Text(FONT_TITLE, px(24), mx + px(28), my + px(22), "Split stack", Theme::Gold.alpha(a));
        TextFit(FONT_BODY, px(19), mx + px(28), my + px(58), mw - px(56), title.c_str(), Theme::TextDim.alpha(a));
        char b[64]; snprintf(b, sizeof b, "%d", HS.splitN);
        float cx = mx + mw * 0.5f, cy = my + px(104);
        Triangle(cx - px(110), cy + px(22), cx - px(90), cy + px(10), cx - px(90), cy + px(34), Theme::GoldDim.alpha(a));
        Triangle(cx + px(110), cy + px(22), cx + px(90), cy + px(10), cx + px(90), cy + px(34), Theme::GoldDim.alpha(a));
        TextCentered(FONT_TITLE, px(44), cx, cy, b, Theme::Text.alpha(a));
        char r[64]; snprintf(r, sizeof r, "of %d  -  %d stay", HS.splitMax + 1, HS.splitMax + 1 - HS.splitN);
        TextCentered(FONT_BODY, px(18), cx, cy + px(58), r, Theme::TextFaint.alpha(a));
        float hx = mx + px(28), hy = my + mh - px(46);
        hx += UIK::Hint(GL_DPAD, "+1 / +10", hx, hy, px(26), a) + px(18);
        hx += UIK::Hint(GL_A, "Split", hx, hy, px(26), a) + px(18);
        UIK::Hint(GL_B, "Back", hx, hy, px(26), a);
        return;
    }
    bool bags = HS.menu == HubState::M_BAGS;
    int n = bags ? (int)HS.bags.size() : (int)HS.acts.size();
    float rowH = px(48);
    float mh = px(96) + n * rowH + px(16), mx = x + (w - mw) * 0.5f, my = y + (h - mh) * 0.5f;
    UIK::Panel(mx, my, mw, mh, px(16), a);
    Text(FONT_TITLE, px(24), mx + px(28), my + px(20), bags ? "Put in which bag?" : "Actions", Theme::Gold.alpha(a));
    TextFit(FONT_BODY, px(19), mx + px(28), my + px(54), mw - px(56), title.c_str(), Theme::TextDim.alpha(a));
    for (int i = 0; i < n; i++) {
        float ry = my + px(90) + i * rowH;
        bool sel = i == (bags ? HS.bagSel : HS.actSel);
        UIK::RowSel(mx + px(16), ry, mw - px(32), rowH - px(6), sel);
        std::string label;
        if (bags) { const InvItem* b = FindPack(HS.bags[i]); label = b ? b->name : "Bag"; }
        else label = ActName(HS.acts[i]);
        Text(FONT_BODY, px(22), mx + px(34), ry + px(9), label.c_str(), (sel ? Col(255, 244, 222) : Theme::Text).alpha(a));
    }
}

void InventoryInput() {
    MergeStep();
    if (MenuInput()) return;
    void* pl = Player();
    Refresh();
    bool up = g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP);
    bool down = g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN);
    bool left = g_pad.Repeat(PB_DLEFT) || g_pad.Repeat(PB_LLEFT);
    bool right = g_pad.Repeat(PB_DRIGHT) || g_pad.Repeat(PB_LRIGHT);
    int n = (int)g_list.size();
    if (HS.focus == 0) {
        if (up) MoveGrid(-1, 0);
        if (down) MoveGrid(1, 0);
        if (left) MoveGrid(0, -1);
        if (right) MoveGrid(0, 1);
    } else if (HS.grid && (int)g_cells.size() == n) {
        if (n > 0) {
            int to = HS.sel;
            if (up) to = GridStep(HS.sel, 0, -1);
            if (down) to = GridStep(HS.sel, 0, 1);
            if (right) to = GridStep(HS.sel, 1, 0);
            if (left) { to = GridStep(HS.sel, -1, 0); if (to < 0) HS.focus = 0; }
            if (to >= 0) HS.sel = to;
            if (g_pad.Repeat(PB_LT)) for (int k = 0; k < 4; k++) { int t = GridStep(HS.sel, 0, -1); if (t >= 0) HS.sel = t; }
            if (g_pad.Repeat(PB_RT)) for (int k = 0; k < 4; k++) { int t = GridStep(HS.sel, 0, 1); if (t >= 0) HS.sel = t; }
        } else if (left) HS.focus = 0;
    } else {
        if (n > 0) {
            if (up) HS.sel = (HS.sel + n - 1) % n;
            if (down) HS.sel = (HS.sel + 1) % n;
            if (g_pad.Repeat(PB_LT)) HS.sel = HS.sel - 8 < 0 ? 0 : HS.sel - 8;
            if (g_pad.Repeat(PB_RT)) HS.sel = HS.sel + 8 >= n ? n - 1 : HS.sel + 8;
        }
        if (left) HS.focus = 0;
    }
    const InvItem* it = Selected();
    if (HS.focus == 1) HS.selId = it ? it->id : 0;
    if (fabsf(g_pad.ry) > 0.2f) HS.descScroll -= g_pad.ry * R::px(700.0f) * (float)g_pad.dt;

    if (g_pad.Pressed(PB_A)) {
        if (HS.focus == 0) {
            int s = GRID[HS.gr][HS.gc];
            if (it) { Inv::UnEquip(pl, it->id); Toast("Unequipped " + it->name); }
            else if (s >= 0) {
                HS.filterSlot = s; HS.focus = 1; HS.sel = 0; HS.selId = 0;
                Refresh(true);
                if (g_list.empty()) { Toast(std::string("Nothing to equip: ") + Inv::SlotName(s)); HS.filterSlot = -1; HS.focus = 0; Refresh(true); }
            }
        } else if (it) {
            if (HS.filterSlot >= 0 && !it->usable) {
                Toast("You can't use " + it->name);
            } else if (HS.filterSlot >= 0) {
                Inv::Equip(pl, *it, HS.filterSlot);
                Toast("Equipped " + it->name);
                HS.filterSlot = -1; HS.focus = 0;
            } else if (Inv::BestSlot(pl, *it) >= 0 && !it->usable) {
                Toast("You can't use " + it->name);
            } else if (Inv::BestSlot(pl, *it) >= 0) {
                Inv::Equip(pl, *it, -1);
                Toast("Equipped " + it->name);
            } else if (Inv::IsContainer(*it)) {
                Inv::OpenBag(it->id);
                Hub_Close();
                return;
            } else {
                CenterMouse();
                Inv::OpenRadial(it->id);
            }
            HS.lastGather = g_pad.now - 0.15;
        }
    }
    if (g_pad.Pressed(PB_X) && it) OpenActions(*it);
    if (g_pad.Pressed(PB_LS)) { HS.sortMode = (HS.sortMode + 1) % NSORT; Toast(std::string("Sort: ") + SORT_NAMES[HS.sortMode]); Refresh(true); }
    if (g_pad.Pressed(PB_BACK)) { HS.grid = !HS.grid; Toast(HS.grid ? "Grid view" : "List view"); }
    if (g_pad.Pressed(PB_Y) && it) { Inv::Examine(pl, it->id); }
    if (g_pad.Pressed(PB_B)) {
        if (HS.filterSlot >= 0) { HS.filterSlot = -1; HS.focus = 0; Refresh(true); }
        else Hub_Close();
    }
}

void QuickbarInput() {
    if (g_pad.Repeat(PB_DLEFT) || g_pad.Repeat(PB_LLEFT)) HS.qbSlot = (HS.qbSlot + 11) % 12;
    if (g_pad.Repeat(PB_DRIGHT) || g_pad.Repeat(PB_LRIGHT)) HS.qbSlot = (HS.qbSlot + 1) % 12;
    if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) HS.qbBank = (HS.qbBank + 2) % 3;
    if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) HS.qbBank = (HS.qbBank + 1) % 3;
    if (g_pad.Pressed(PB_A)) QB_Edit(HS.qbBank, HS.qbSlot);
    if (g_pad.Pressed(PB_BACK) && !QB_Empty(HS.qbBank, HS.qbSlot)) QB_Clear(HS.qbBank, HS.qbSlot);
    if (g_pad.Pressed(PB_X) && !QB_Empty(HS.qbBank, HS.qbSlot)) { Hub_Close(); Control_ClickQuickSlot(HS.qbBank, HS.qbSlot); }
    if (g_pad.Pressed(PB_Y)) { g_ctl.quickBank = HS.qbBank; Toast(std::string("Active set: ") + (HS.qbBank == 0 ? "I" : HS.qbBank == 1 ? "II" : "III")); }
    if (g_pad.Pressed(PB_B)) Hub_Close();
}

bool LevelUpReady() { return GD::LevelUpReady() || (HS.ch.ok && HS.ch.xpNext > 0 && HS.ch.xp >= HS.ch.xpNext); }

void CharacterInput() {
    if (g_pad.Pressed(PB_X) && LevelUpReady()) {
        Hub_Close();
        typedef void (*fn)();
        ((fn)A(0x1406f5c20))();          // CGuiInGame::HandleLevelUpButton
        Log("hub: level up");
        return;
    }
    if (g_pad.Pressed(PB_A)) OpenExternal(HS.tab, 0x42);
    if (g_pad.Pressed(PB_Y)) { Hub_Close(); FireEvent(0x6b); }    // rest
    if (g_pad.Pressed(PB_B)) Hub_Close();
}

// ---------------------------------------------------------------------------------------------
std::string Fmt(const char* f, int v) { char b[64]; snprintf(b, sizeof b, f, v); return b; }

void DrawTabs(float x, float y, float w, float a) {
    float fs = px(26);
    float gap = px(46);
    float total = 0;
    for (int i = 0; i < NTABS; i++) total += TextWidth(FONT_TITLE, fs, TABS[i].name) + (i ? gap : 0);
    float tx = x + (w - total) * 0.5f;
    float gs = px(30);
    UIK::Glyph(GL_LB, tx - gap - gs * 1.45f, y + px(2), gs, a);
    for (int i = 0; i < NTABS; i++) {
        float tw = TextWidth(FONT_TITLE, fs, TABS[i].name);
        bool on = i == HS.tab;
        Text(FONT_TITLE, fs, tx, y + px(2), TABS[i].name, (on ? Theme::Gold : Theme::TextFaint).alpha(a));
        if (on) RoundRect(tx - px(6), y + px(42), tw + px(12), px(3), px(1.5f), Theme::LineStrong.alpha(a));
        tx += tw + gap;
    }
    UIK::Glyph(GL_RB, tx - gap + gap, y + px(2), gs, a);
    RoundRect(x + px(30), y + px(52), w - px(60), px(1), 0, Theme::Line.alpha(0.5f * a));
}

void SectionTitle(float x, float y, const char* t, float a) {
    Text(FONT_TITLE, px(22), x, y, t, Theme::GoldDim.alpha(a));
}

void DrawSlotTile(float x, float y, float s, int slot, bool sel, float a) {
    const InvItem& it = HS.eq[slot];
    RoundRect(x, y, s, s, px(10), Col(8, 7, 6, (int)(170 * a)));
    RoundRectBorder(x, y, s, s, px(10), px(1), Theme::Line.alpha(0.35f * a));
    if (it.item) {
        Inv::DrawIcon(it, x + s * 0.12f, y + s * 0.12f, s * 0.76f, a);
        if (it.stack > 1) TextRight(FONT_BODY_BOLD, px(18), x + s - px(8), y + s - px(26), Fmt("%d", it.stack).c_str(), Theme::Text.alpha(a));
    } else {
        TextWrap(FONT_BODY, px(17), x + px(6), y + s * 0.5f - px(12), s - px(12), Inv::SlotName(slot), Theme::TextFaint.alpha(a), 1);
    }
    if (sel) {
        float p = 0.6f + 0.4f * sinf((float)g_pad.now * 5.0f);
        RoundRectBorder(x - px(3), y - px(3), s + px(6), s + px(6), px(12), px(2.5f), Theme::LineStrong.alpha(p * a));
    }
}

void DrawInventory(float x, float y, float w, float h, float a) {
    void* pl = Player();
    Refresh();
    // --- equipment
    float gap = px(12);
    float avail = h - px(58) - px(20);
    float tile = fminf(px(108), (avail - gap * (GRID_R - 1)) / GRID_R);
    float ew = tile * GRID_C + gap * (GRID_C - 1);
    float ex = x + px(36), ey = y + px(18);
    SectionTitle(ex, ey, "Equipment", a);
    ey += px(40);
    for (int r = 0; r < GRID_R; r++)
        for (int c = 0; c < GRID_C; c++) {
            int s = GRID[r][c];
            if (s < 0) continue;
            DrawSlotTile(ex + c * (tile + gap), ey + r * (tile + gap), tile, s, HS.focus == 0 && HS.gr == r && HS.gc == c, a);
        }
    // gold (in the free grid cell)
    {
        float gx = ex + 2 * (tile + gap), gy = ey + 3 * (tile + gap);
        float cx = gx + tile * 0.5f, cy = gy + tile * 0.36f;
        Circle(cx, cy, tile * 0.16f, Col(222, 180, 70, (int)(255 * a)));
        Ring(cx, cy, tile * 0.16f, px(2), Col(140, 100, 30, (int)(255 * a)));
        Circle(cx, cy, tile * 0.08f, Col(240, 206, 110, (int)(255 * a)));
        TextCentered(FONT_BODY_BOLD, px(22), cx, gy + tile * 0.58f, Fmt("%d", Inv::Gold(pl)).c_str(), Theme::Gold.alpha(a));
    }

    // --- details column
    float dw = px(400);
    float dx = x + w - dw - px(36);
    // --- backpack list
    float lx = ex + ew + px(40), lw = dx - lx - px(30);
    float ly = y + px(18);
    std::string title = HS.filterSlot >= 0 ? std::string("Backpack  ·  ") + Inv::SlotName(HS.filterSlot)
                      : HS.sortMode ? std::string("Backpack  ·  by ") + SORT_NAMES[HS.sortMode] : std::string("Backpack");
    SectionTitle(lx, ly, title.c_str(), a);
    TextRight(FONT_BODY, px(20), lx + lw, ly + px(2), Fmt((int)g_list.size() == 1 ? "%d item" : "%d items", (int)g_list.size()).c_str(), Theme::TextFaint.alpha(a));
    ly += px(40);
    float lh = h - (ly - y) - px(20);
    int n = (int)g_list.size();
    RoundRect(lx, ly, lw, lh, px(12), Col(8, 7, 6, (int)(120 * a)));
    if (HS.grid) {
        // grid of footprints, scrolled by rows
        float cell = px(60);
        int cols = (int)((lw - px(28)) / cell);
        PackGrid(cols);
        float gx0 = lx + (lw - g_gridCols * cell) * 0.5f, gy0 = ly + px(10);
        int visRows = (int)((lh - px(20)) / cell);
        if (visRows < 1) visRows = 1;
        if (HS.sel >= 0 && HS.sel < (int)g_cells.size()) {
            const Cell& c = g_cells[HS.sel];
            if (c.y < HS.scroll) HS.scroll = c.y;
            if (c.y + c.h > HS.scroll + visRows) HS.scroll = c.y + c.h - visRows;
        }
        if (HS.scroll > g_gridRows - visRows) HS.scroll = g_gridRows - visRows;
        if (HS.scroll < 0) HS.scroll = 0;
        PushClip(lx, ly + px(4), lw, lh - px(8));
        int rowsShown = std::max(visRows, g_gridRows - HS.scroll);
        for (int r = 0; r < rowsShown && r < visRows + 1; r++)
            for (int c = 0; c < g_gridCols; c++)
                RoundRect(gx0 + c * cell + px(2), gy0 + r * cell + px(2), cell - px(4), cell - px(4), px(6), Col(255, 240, 210, (int)(10 * a)));
        for (int i = 0; i < n; i++) {
            const Cell& c = g_cells[i];
            if (c.y + c.h <= HS.scroll || c.y >= HS.scroll + visRows + 1) continue;
            const InvItem& it = HS.pack[g_list[i]];
            float rx = gx0 + c.x * cell + px(2), ry = gy0 + (c.y - HS.scroll) * cell + px(2);
            float rw = c.w * cell - px(4), rh = c.h * cell - px(4);
            bool sel = HS.focus == 1 && i == HS.sel;
            RoundRect(rx, ry, rw, rh, px(8), (it.usable ? Col(40, 34, 26) : Col(120, 30, 24)).alpha(0.85f * a));
            InvItem shown = it; shown.usable = true;               // the whole footprint is red already
            float isz = fminf(rw, rh) - px(6);
            // tall / wide icons fill the footprint (the icon keeps its aspect inside the square it gets)
            float iw = fminf(rw - px(6), rh - px(6));
            if (c.w != c.h) isz = fmaxf(rw, rh) - px(6);
            float ix = rx + (rw - isz) * 0.5f, iy = ry + (rh - isz) * 0.5f;
            (void)iw;
            PushClip(rx, ry, rw, rh);
            Inv::DrawIcon(shown, ix, iy, isz, a);
            PopClip();
            if (!it.identified) Circle(rx + px(10), ry + px(10), px(4), Col(150, 170, 220).alpha(a));
            if (it.stack > 1) {
                std::string sn = Fmt("%d", it.stack);
                float tw = TextWidth(FONT_BODY_BOLD, px(15), sn.c_str());
                RoundRect(rx + rw - tw - px(10), ry + rh - px(20), tw + px(8), px(18), px(5), Col(0, 0, 0, (int)(170 * a)));
                Text(FONT_BODY_BOLD, px(15), rx + rw - tw - px(6), ry + rh - px(20), sn.c_str(), Theme::Gold.alpha(a));
            }
            if (sel) {
                float p = 0.65f + 0.35f * sinf((float)g_pad.now * 5.0f);
                RoundRectBorder(rx - px(2), ry - px(2), rw + px(4), rh + px(4), px(9), px(2.5f), Theme::LineStrong.alpha(p * a));
            } else RoundRectBorder(rx, ry, rw, rh, px(8), px(1), Theme::Line.alpha(0.35f * a));
        }
        if (n == 0) TextCentered(FONT_BODY, px(22), lx + lw * 0.5f, ly + px(30), "Empty", Theme::TextFaint.alpha(a));
        PopClip();
        UIK::Scrollbar(lx + lw - px(8), ly + px(6), lh - px(12), HS.scroll, visRows, std::max(g_gridRows, visRows));
        // name of the selected item under the grid is in the details column already
    } else {
    float rowH = px(62);
    int vis = (int)(lh / rowH);
    if (vis < 1) vis = 1;
    if (HS.sel < HS.scroll) HS.scroll = HS.sel;
    if (HS.sel >= HS.scroll + vis) HS.scroll = HS.sel - vis + 1;
    if (HS.scroll > n - vis) HS.scroll = n - vis;
    if (HS.scroll < 0) HS.scroll = 0;
    PushClip(lx, ly, lw, lh);
    for (int i = HS.scroll; i < n && i < HS.scroll + vis + 1; i++) {
        const InvItem& it = HS.pack[g_list[i]];
        float ry = ly + (i - HS.scroll) * rowH;
        bool sel = HS.focus == 1 && i == HS.sel;
        UIK::RowSel(lx + px(4), ry + px(3), lw - px(8), rowH - px(6), sel);
        Inv::DrawIcon(it, lx + px(14), ry + px(7), rowH - px(14), a);
        Col nc = it.identified ? (sel ? Col(255, 244, 222) : Theme::Text) : Col(150, 170, 220);
        TextFit(FONT_BODY, px(24), lx + px(14) + rowH + px(4), ry + px(9), lw - rowH - px(150), it.name.c_str(), nc.alpha(a));
        std::string sub = it.typeName;
        if (HS.sortMode == 3) sub += "  ·  " + Fmt("%d gp", (int)it.cost);
        if (HS.sortMode == 4) { char wb[32]; snprintf(wb, sizeof wb, "  ·  %.1f lb", it.weight / 10.0f); sub += wb; }
        TextFit(FONT_BODY, px(17), lx + px(14) + rowH + px(4), ry + px(36), lw - rowH - px(150), sub.c_str(), Theme::TextFaint.alpha(a));
        if (it.stack > 1) TextRight(FONT_BODY_BOLD, px(22), lx + lw - px(22), ry + px(18), Fmt("x%d", it.stack).c_str(), Theme::GoldDim.alpha(a));
    }
    if (n == 0) TextCentered(FONT_BODY, px(22), lx + lw * 0.5f, ly + px(30), "Empty", Theme::TextFaint.alpha(a));
    PopClip();
    UIK::Scrollbar(lx + lw - px(8), ly + px(6), lh - px(12), HS.scroll, vis, n);
    }

    // --- details
    float dy = y + px(18);
    SectionTitle(dx, dy, "Details", a);
    dy += px(40);
    RoundRect(dx, dy, dw, h - (dy - y) - px(20), px(12), Col(8, 7, 6, (int)(120 * a)));
    const InvItem* it = Selected();
    float cy = dy + px(20);
    if (it) {
        float bottom = y + h - px(20);
        // header: icon left, name / type / facts right
        float is = px(84);
        float ix = dx + px(18), iy = cy;
        RoundRect(ix, iy, is + px(16), is + px(16), px(12), Col(0, 0, 0, (int)(90 * a)));
        Inv::DrawIcon(*it, ix + px(8), iy + px(8), is, a);
        float tx = ix + is + px(32), tw = dx + dw - px(18) - tx;
        float ty = iy + px(2);
        ty += TextWrap(FONT_TITLE, px(23), tx, ty, tw, it->name.c_str(), Theme::Gold.alpha(a)) + px(4);
        Text(FONT_BODY, px(18), tx, ty, it->typeName.c_str(), Theme::TextDim.alpha(a)); ty += px(26);
        std::string facts;
        if (!it->identified) facts = "Unidentified";
        else if (it->equippedSlot >= 0) facts = std::string("Equipped: ") + Inv::SlotName(it->equippedSlot);
        if (it->stack > 1) { if (!facts.empty()) facts += "  -  "; facts += Fmt("Stack %d", it->stack); }
        if (!facts.empty()) { TextFit(FONT_BODY_BOLD, px(17), tx, ty, tw, facts.c_str(), (!it->identified ? Col(150, 170, 220) : Theme::Friendly).alpha(a)); ty += px(24); }
        if (!it->usable) { TextFit(FONT_BODY_BOLD, px(17), tx, ty, tw, "You can't use this", Theme::Hostile.alpha(a)); ty += px(24); }
        cy = fmaxf(iy + is + px(28), ty + px(8));
        if (it->equippedSlot < 0 && (it->equipMask & 0x3fff)) {
            std::string fits;
            for (int s = 0; s < ES_COUNT_PLAYER; s++)
                if (it->equipMask & (1u << s)) { if (!fits.empty()) fits += ", "; fits += Inv::SlotName(s); }
            cy += TextWrap(FONT_BODY, px(17), dx + px(20), cy, dw - px(40), ("Fits: " + fits).c_str(), Theme::TextFaint.alpha(a)) + px(8);
        }
        RoundRect(dx + px(18), cy, dw - px(36), px(1), 0, Theme::Line.alpha(0.4f * a));
        cy += px(12);
        // description (examine text), right stick scrolls
        float dh = bottom - px(56) - cy;
        const std::string* d = Describe_Get(it->id);
        if (HS.descId != it->id) { HS.descId = it->id; HS.descScroll = 0; }
        PushClip(dx + px(10), cy, dw - px(20), dh);
        if (d) {
            float full = TextWrap(FONT_BODY, px(18), 0, 0, dw - px(44), d->c_str(), Theme::Text, 0, 1.2f, true);
            float maxS = fmaxf(0.0f, full - dh + px(10));
            HS.descScroll = fminf(fmaxf(HS.descScroll, 0.0f), maxS);
            TextWrap(FONT_BODY, px(18), dx + px(20), cy - HS.descScroll, dw - px(44), d->c_str(), Theme::Text.alpha(0.92f * a), 0, 1.2f);
            if (maxS > 0) {
                float th2 = dh * dh / full;
                RoundRect(dx + dw - px(14), cy + (dh - th2) * (HS.descScroll / maxS), px(4), th2, px(2), Theme::GoldDim.alpha(a));
            }
        } else {
            Text(FONT_BODY, px(18), dx + px(20), cy, "...", Theme::TextFaint.alpha(a));
        }
        PopClip();
        // primary action
        float ay = bottom - px(44);
        float ax = dx + px(20);
        const char* act = HS.focus == 0 ? "Unequip" : (HS.filterSlot >= 0 || Inv::BestSlot(pl, *it) >= 0) ? "Equip" : Inv::IsContainer(*it) ? "Open" : "Use";
        ax += UIK::Hint(GL_A, act, ax, ay, px(28), a) + px(24);
        ax += UIK::Hint(GL_X, "Actions", ax, ay, px(28), a) + px(24);
        UIK::Hint(GL_RS, "Scroll", ax, ay, px(28), a);
    } else if (HS.focus == 0) {
        int s = GRID[HS.gr][HS.gc];
        TextCentered(FONT_TITLE, px(26), dx + dw * 0.5f, cy + px(40), Inv::SlotName(s), Theme::GoldDim.alpha(a));
        TextCentered(FONT_BODY, px(20), dx + dw * 0.5f, cy + px(84), "Empty slot", Theme::TextFaint.alpha(a));
        UIK::Hint(GL_A, "Choose item", dx + px(28), y + h - px(20) - px(80), px(30), a);
    }
    DrawMenu(x, y, w, h, a);
}

void AbilityBox(float x, float y, float w, const char* name, int v, float a) {
    float h = px(108);
    RoundRect(x, y, w, h, px(12), Col(8, 7, 6, (int)(150 * a)));
    RoundRectBorder(x, y, w, h, px(12), px(1), Theme::Line.alpha(0.4f * a));
    TextCentered(FONT_TITLE, px(18), x + w * 0.5f, y + px(10), name, Theme::GoldDim.alpha(a));
    TextCentered(FONT_TITLE, px(40), x + w * 0.5f, y + px(32), Fmt("%d", v).c_str(), Theme::Text.alpha(a));
    int m = (int)floorf((v - 10) / 2.0f);
    TextCentered(FONT_BODY_BOLD, px(18), x + w * 0.5f, y + px(80), (m >= 0 ? Fmt("+%d", m) : Fmt("%d", m)).c_str(), Theme::TextDim.alpha(a));
}

void StatLine(float x, float y, float w, const char* k, const std::string& v, float a) {
    Text(FONT_BODY, px(22), x, y, k, Theme::TextDim.alpha(a));
    TextRight(FONT_BODY_BOLD, px(22), x + w, y, v.c_str(), Theme::Text.alpha(a));
    RoundRect(x, y + px(34), w, px(1), 0, Theme::Line.alpha(0.25f * a));
}

void DrawCharacter(float x, float y, float w, float h, float a) {
    void* pl = Player();
    if (g_pad.now - HS.lastChar > 0.5) { HS.ch = Inv::Character(pl); HS.lastChar = g_pad.now; }
    const Inv::CharInfo& c = HS.ch;
    if (!c.ok) return;
    // portrait
    float pw = px(260), ph = px(406);
    float pxl = x + px(40), pyl = y + px(28);
    uint32_t tex = pl ? GD::PortraitTex(pl, 'h') : 0;
    if (!tex && pl) tex = GD::PortraitTex(pl, 'l');
    RoundRect(pxl, pyl, pw, ph, px(14), Col(0, 0, 0, (int)(120 * a)));
    if (tex) ImageRounded(tex, pxl, pyl, pw, ph, px(14), Col(255, 255, 255, (int)(255 * a)), 0, 0, 1, 0.78f);
    RoundRectBorder(pxl, pyl, pw, ph, px(14), px(1.5f), Theme::Line.alpha(a));
    // identity
    float ix = pxl + pw + px(40), iw = w - (ix - x) - px(40);
    float iy = y + px(24);
    Text(FONT_TITLE, px(40), ix, iy, c.name.c_str(), Theme::Gold.alpha(a));
    iy += px(56);
    std::string line = c.race + "   ·   " + c.classes;
    TextFit(FONT_BODY, px(24), ix, iy, iw, line.c_str(), Theme::TextDim.alpha(a));
    iy += px(44);
    // level + xp bar
    Text(FONT_BODY_BOLD, px(22), ix, iy, Fmt("Level %d", c.level).c_str(), Theme::Text.alpha(a));
    std::string xpS = std::to_string(c.xp) + " / " + std::to_string(c.xpNext) + " XP";
    TextRight(FONT_BODY, px(20), ix + iw, iy + px(2), xpS.c_str(), Theme::TextDim.alpha(a));
    iy += px(34);
    float xf = c.xpNext > c.xpCur ? (float)(c.xp - c.xpCur) / (float)(c.xpNext - c.xpCur) : 1.0f;
    UIK::Bar(ix, iy, iw, px(12), xf, Col(196, 160, 80), Col(30, 26, 20, (int)(220 * a)));
    if (LevelUpReady()) {
        iy += px(20);
        float lw = Text(FONT_BODY_BOLD, px(20), ix, iy, "Level up available", Theme::Friendly.alpha(a));
        UIK::Hint(GL_X, "Level up", ix + lw + px(24), iy - px(4), px(26), a);
    }
    iy += px(40);
    // abilities
    static const char* AB[6] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
    float bw = (iw - px(14) * 5) / 6;
    for (int i = 0; i < 6; i++) AbilityBox(ix + i * (bw + px(14)), iy, bw, AB[i], c.ab[i], a);
    iy += px(136);
    // combat block
    float cw = (iw - px(40)) * 0.5f;
    StatLine(ix, iy, cw, "Hit points", std::to_string(c.hp) + " / " + std::to_string(c.hpMax), a);
    StatLine(ix + cw + px(40), iy, cw, "Armour class", std::to_string(c.ac), a);
    iy += px(48);
    StatLine(ix, iy, cw, "Base attack", "+" + std::to_string(c.bab), a);
    StatLine(ix + cw + px(40), iy, cw, "Gold", std::to_string(c.gold), a);
    iy += px(60);
    float hw = UIK::Hint(GL_A, "Full character sheet", ix, iy, px(30), a);
    UIK::Hint(GL_Y, "Rest", ix + hw + px(40), iy, px(30), a);
}
void DrawBinding(int slot, float cx, float y, float size, float a) {
    int mod, btn; QB_Binding(slot, mod, btn);
    float gw = size, tw = 0;
    float plus = TextWidth(FONT_BODY_BOLD, size * 0.6f, "+");
    if (mod == 1 || mod == 2) tw = size * 1.45f + plus + px(8) + gw;
    else if (mod == 3) tw = size * 1.45f * 2 + plus * 2 + px(16) + gw;
    else tw = gw;
    float x = cx - tw * 0.5f;
    auto P = [&]() { Text(FONT_BODY_BOLD, size * 0.6f, x + px(2), y + size * 0.15f, "+", Theme::TextDim.alpha(a)); x += plus + px(4); };
    if (mod == 1 || mod == 3) { x += UIK::Glyph(GL_LT, x, y, size, a) + px(2); P(); }
    if (mod == 2 || mod == 3) { x += UIK::Glyph(GL_RT, x, y, size, a) + px(2); P(); }
    UIK::Glyph((PadGlyph)btn, x, y, size, a);
}

void DrawQuickbar(float x, float y, float w, float h, float a) {
    float tile = fminf(px(100), (w - px(80) - px(10) * 11 - px(120)) / 12);
    float gap = px(10);
    float gridW = tile * 12 + gap * 11;
    float gx = x + (w - gridW) * 0.5f + px(50);
    float gy = y + px(30);
    static const char* setN[3] = {"Set I", "Set II", "Set III"};
    // binding header
    for (int s = 0; s < 12; s++) DrawBinding(s, gx + s * (tile + gap) + tile * 0.5f, gy - px(4), px(25), a);
    gy += px(40);
    for (int b = 0; b < 3; b++) {
        float ry = gy + b * (tile + px(18));
        bool act = b == g_ctl.quickBank;
        Text(FONT_TITLE, px(20), gx - px(110), ry + tile * 0.5f - px(14), setN[b], (act ? Theme::Gold : Theme::TextFaint).alpha(a));
        if (act) Circle(gx - px(122), ry + tile * 0.5f, px(4), Theme::Gold.alpha(a));
        for (int s = 0; s < 12; s++) {
            float tx = gx + s * (tile + gap);
            bool sel = b == HS.qbBank && s == HS.qbSlot;
            RoundRect(tx, ry, tile, tile, px(10), Col(8, 7, 6, (int)(170 * a)));
            RoundRectBorder(tx, ry, tile, tile, px(10), px(1), Theme::Line.alpha((act ? 0.55f : 0.3f) * a));
            bool dis = false;
            uint32_t tex = QB_Icon(b, s, &dis);
            if (tex) ImageRounded(tex, tx + tile * 0.1f, ry + tile * 0.1f, tile * 0.8f, tile * 0.8f, px(8), (dis ? Col(120, 120, 120) : Col(255, 255, 255)).alpha(a));
            else if (!QB_Empty(b, s)) TextCentered(FONT_TITLE, px(28), tx + tile * 0.5f, ry + tile * 0.28f, "?", Theme::GoldDim.alpha(a));
            int ms = QB_ModeState(b, s);
            if (ms == 2) RoundRectBorder(tx + px(2), ry + px(2), tile - px(4), tile - px(4), px(9), px(3), Col(255, 196, 70).alpha(a));
            else if (ms == 3) RoundRectBorder(tx + px(2), ry + px(2), tile - px(4), tile - px(4), px(9), px(2), Col(255, 196, 70).alpha((0.4f + 0.4f * sinf((float)g_pad.now * 6.0f)) * a));
            if (sel) {
                float p = 0.6f + 0.4f * sinf((float)g_pad.now * 5.0f);
                RoundRectBorder(tx - px(3), ry - px(3), tile + px(6), tile + px(6), px(12), px(2.5f), Theme::LineStrong.alpha(p * a));
            }
        }
    }
    // details of the selected slot
    float dy = gy + 3 * (tile + px(18)) + px(20);
    RoundRect(x + px(40), dy, w - px(80), px(1), 0, Theme::Line.alpha(0.4f * a));
    dy += px(24);
    std::string nm = QB_Name(HS.qbBank, HS.qbSlot);
    float bx = x + px(60);
    int mod, btn; QB_Binding(HS.qbSlot, mod, btn);
    Text(FONT_TITLE, px(30), bx, dy, nm.empty() ? "Empty slot" : nm.c_str(), (nm.empty() ? Theme::TextFaint : Theme::Gold).alpha(a));
    dy += px(50);
    std::string where = std::string(setN[HS.qbBank]) + ", slot " + std::to_string(HS.qbSlot + 1) + "   -   in play: ";
    float ww = TextWidth(FONT_BODY, px(21), where.c_str());
    Text(FONT_BODY, px(21), bx, dy, where.c_str(), Theme::TextDim.alpha(a));
    DrawBinding(HS.qbSlot, bx + ww + px(60), dy - px(2), px(26), a);
    dy += px(46);
    TextWrap(FONT_BODY, px(19), bx, dy, w - px(160),
             "Assign opens the game's quickbar menu: attack, special attacks, abilities, spells, party commands, actions and "
             "emotes; choose with the stick, A to pick, Y back. "
             "In play, hold LT or RT to reach the extra slots and use the d-pad up/down to switch sets.",
             Theme::TextFaint.alpha(a));
}
}  // namespace

// ---------------------------------------------------------------------------------------------
void Hub_Init() { Inv::Init(); }

bool Hub_Open() { return HS.open; }
bool Hub_ExternalActive() { return HS.extTab >= 0; }
void Hub_SwitchTo(const char* name) {
    for (int i = 0; i < NTABS; i++) if (strcmp(TABS[i].name, name) == 0) { SwitchTab(i); return; }
}

void Hub_Close() {
    ClosePage();
    if (HS.open) Log("hub: close");
    HS.open = false;
    SyncStatus();
    HS.open = false;
    HS.filterSlot = -1;
}

void Hub_Toggle() {
    if (HS.open) { Hub_Close(); return; }
    if (!Player()) return;
    if (TABS[HS.tab].event) HS.tab = 0;
    Log("hub: open tab %d", HS.tab);
    if (g_cfg.devDumpArea) {
        void* pl = Player(); void* area = F<void*>(pl, OFS::Obj_Area);
        Vec3 pp = Pos(pl); Log("player at %.1f %.1f %.1f", pp.x, pp.y, pp.z);
        int n = area ? F<int>(area, OFS::Area_ObjCount) : 0; uint32_t* ids = area ? F<uint32_t*>(area, OFS::Area_ObjIds) : nullptr;
        for (int i = 0; ids && i < n; i++) { void* o = GetObj(ids[i]); if (!o) continue; if (Type(o) != OT_PLACEABLE && Type(o) != OT_DOOR && Type(o) != OT_CREATURE) continue;
            Vec3 p = Pos(o); Log("  obj %08x t%d %.1f %.1f '%s'", ids[i], Type(o), p.x, p.y, GD::ObjName(ids[i]).c_str()); }
    }
    HS.focus = 0;
    SwitchTab(HS.tab);
}

bool Hub_ExternalInput(bool cursorActive) {
    if (HS.extTab < 0) return false;
    if (cursorActive) {
        HS.extSeen = true;
        int dir = g_pad.Pressed(PB_RB) ? 1 : g_pad.Pressed(PB_LB) ? -1 : 0;
        if (dir) {
            // close the game panel and move on
            int ev = TABS[HS.extTab].event;
            if (HS.extTab == HS.tab && TABS[HS.tab].event == 0) ev = 0x42;   // char sheet from the Character tab
            FireEvent(ev);
            int t = HS.tab + dir;
            HS.extTab = -1;
            SwitchTab(t);
            return true;
        }
        if (g_pad.Pressed(PB_START)) { FireEvent(0x12); HS.extTab = -1; return true; }
        return false;
    }
    if (HS.extSeen || g_pad.now - HS.extTime > 1.0) {
        // panel closed by the player: back to the hub tab if it came from a custom one
        bool fromCustom = TABS[HS.tab].event == 0;
        HS.extTab = -1;
        if (fromCustom && HS.extSeen) { HS.open = true; HS.openTime = g_pad.now; }
    }
    return false;
}

bool Hub_Input() {
    if (!HS.open) return false;
    SyncStatus();
    if (!Player()) { Hub_Close(); return false; }
    if (g_ctl.radialOpen) return false;                    // gameplay layer drives the radial menu
    if (g_ctl.inActionTargetMode) { Hub_Close(); return false; }
    if (g_pad.Pressed(PB_START)) { Hub_Close(); return true; }
    if (g_pad.Pressed(PB_RB)) { SwitchTab(HS.tab + 1); return true; }
    if (g_pad.Pressed(PB_LB)) { SwitchTab(HS.tab - 1); return true; }
    if (HS.tab == 0) InventoryInput();
    else if (HS.tab == 1) CharacterInput();
    else if (HS.tab == 2) QuickbarInput();
    else if (TABS[HS.tab].page) TABS[HS.tab].page->input();
    return true;
}

void Hub_Draw() {
    if (!HS.open || g_ctl.radialOpen) return;
    float t = (float)fmin(1.0, (g_pad.now - HS.openTime) / 0.16);
    float a = 1 - (1 - t) * (1 - t);
    R::Rect(0, 0, (float)W(), (float)H(), Col(0, 0, 0, (int)(150 * a)));
    float w = fminf(px(1680), W() - px(60));
    float h = fminf(px(880), H() - px(150));
    float x = (W() - w) * 0.5f, y = (H() - h) * 0.5f - px(30) + (1 - a) * px(20);
    UIK::Panel(x, y, w, h, px(20), a);
    DrawTabs(x, y + px(22), w, a);
    float cy = y + px(86);
    float ch = h - px(86);
    if (HS.tab == 0) DrawInventory(x, cy, w, ch, a);
    else if (HS.tab == 1) DrawCharacter(x, cy, w, ch, a);
    else if (HS.tab == 2) DrawQuickbar(x, cy, w, ch, a);
    else if (TABS[HS.tab].page) TABS[HS.tab].page->draw(x, cy, w, ch, a);
    // toast
    if (g_pad.now - HS.toastTime < 2.0) {
        float ta = (float)fmin(1.0, (2.0 - (g_pad.now - HS.toastTime)) / 0.4) * a;
        float tw = TextWidth(FONT_BODY, px(22), HS.toast.c_str()) + px(40);
        float tx = (W() - tw) * 0.5f, ty = y + h - px(60);
        RoundRect(tx, ty, tw, px(40), px(20), Col(30, 26, 20, (int)(230 * ta)));
        RoundRectBorder(tx, ty, tw, px(40), px(20), px(1), Theme::Line.alpha(ta));
        TextCentered(FONT_BODY, px(22), W() * 0.5f, ty + px(7), HS.toast.c_str(), Theme::Text.alpha(ta));
    }
    // hints
    HintItem hints[12]; int n = 0;
    hints[n++] = {GL_LB, "Tabs"};
    if (TABS[HS.tab].page) {
        n += TABS[HS.tab].page->hints(hints + n);
    } else if (HS.tab == 0 && HS.menu != HubState::M_NONE) {
        hints[n++] = {GL_DPAD, HS.menu == HubState::M_SPLIT ? "Amount" : "Choose"};
        hints[n++] = {GL_A, "Select"};
        hints[n++] = {GL_B, "Back"};
    } else if (HS.tab == 0) {
        hints[n++] = {GL_DPAD, "Move"};
        hints[n++] = {GL_A, "Select"};
        hints[n++] = {GL_X, "Actions"};
        hints[n++] = {GL_Y, "Examine"};
        hints[n++] = {GL_L3, "Sort"};
        hints[n++] = {GL_BACK, HS.grid ? "List" : "Grid"};
    } else if (HS.tab == 1) {
        if (LevelUpReady()) hints[n++] = {GL_X, "Level up"};
        hints[n++] = {GL_A, "Character sheet"};
        hints[n++] = {GL_Y, "Rest"};
    } else if (HS.tab == 2) {
        hints[n++] = {GL_DPAD, "Move"};
        hints[n++] = {GL_A, "Assign"};
        hints[n++] = {GL_X, "Use"};
        hints[n++] = {GL_Y, "Make active"};
        if (!QB_Empty(HS.qbBank, HS.qbSlot)) hints[n++] = {GL_BACK, "Clear"};
    }
    bool hasB = false;
    for (int i = 0; i < n; i++) if (hints[i].g == GL_B) hasB = true;
    if (!hasB) hints[n++] = {GL_B, HS.filterSlot >= 0 ? "Back" : "Close"};
    UI_DrawHints(hints, n, a);
}

// developer: open the hub and run "Use" (or Equip) on the first backpack item whose name contains the text
void Hub_DevUse(const char* want, bool equip) {
    void* pl = Player();
    if (!pl) return;
    std::vector<InvItem> pack; InvItem eq[ES_TOTAL];
    Inv::Gather(pl, pack, eq);
    for (auto& it : pack) {
        std::string l = it.name; for (auto& c : l) c = (char)tolower((unsigned char)c);
        if (l.find(want) == std::string::npos) continue;
        if (!HS.open) Hub_Toggle();
        Log("dev: hub %s %08x '%s'", equip ? "equip" : "use", it.id, it.name.c_str());
        if (equip) Inv::Equip(pl, it, -1);
        else if (!Inv::UseItem(pl, it.id)) { CenterMouse(); Inv::OpenRadial(it.id); }
        return;
    }
    Log("dev: no item matching '%s'", want);
}


std::string Hub_DebugState() {
    if (!HS.open) return "closed";
    char b[96]; snprintf(b, sizeof b, "tab %d menu %d", HS.tab, (int)HS.menu);
    return b;
}
