// NWNPad - gamepad loot window for containers (chests, corpses, bags); replaces the game's container view
#include "loot.h"
#include "guiscan.h"
#include "inv.h"
#include "hub.h"
#include "uikit.h"
#include "render.h"
#include "game.h"
#include "gamedata.h"
#include "pad.h"
#include "config.h"
#include "log.h"
#include "item_details.h"
#include "ui.h"
#include <windows.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cstdio>

using namespace G;
using namespace R;

namespace {
typedef void (*PanelFn)(void*);
PanelFn fHide = nullptr;

struct LootState {
    bool active = false;
    uint32_t container = 0x7f000000;
    std::string name;
    std::vector<InvItem> items;     // the list shown: the container's, or your backpack
    int side = 0;                   // 0 the container, 1 your backpack (to put things in)
    int sel = 0, scroll = 0;
    uint32_t selId = 0;
    double openTime = 0, lastGather = -10;
} LS;

void Refresh(bool force = false) {
    if (!force && g_pad.now - LS.lastGather < 0.2) return;
    LS.lastGather = g_pad.now;
    if (LS.side == 0) Inv::GatherContainer(Player(), LS.items);
    else {
        static InvItem eq[ES_TOTAL];
        Inv::Gather(Player(), LS.items, eq);
    }
    int n = (int)LS.items.size();
    static int lastN = -1; static uint32_t lastC = 0;
    if (n != lastN || LS.container != lastC) {
        lastN = n; lastC = LS.container;
        std::string s; for (auto& it : LS.items) { char b[96]; snprintf(b, sizeof b, " %s(%d)", it.name.c_str(), it.baseItem); s += b; }
        Log("loot: %d items:%s", n, s.c_str());
    }
    if (LS.selId) for (int i = 0; i < n; i++) if (LS.items[i].id == LS.selId) { LS.sel = i; break; }
    if (LS.sel >= n) LS.sel = n - 1;
    if (LS.sel < 0) LS.sel = 0;
}

void UnHidePanel() {
    Gui_ShowPanel(Inv::InventoryPanel());
}

void HidePanel() {
    void* p = Inv::InventoryPanel();
    if (p && fHide && (F<uint32_t>(p, 0xb0) & 2) == 0) fHide(p);
}
}  // namespace

void Loot_Init() {
    fHide = (PanelFn)GetProcAddress(GetModuleHandleA(nullptr), "?Hide@CGuiPanel@@QEAAXXZ");
}

bool Loot_Active() {
    if (!g_cfg.newMenus || !Player() || !UI_PadPresent()) {
        if (LS.active) { LS.active = false; UnHidePanel(); }
        return false;
    }
    uint32_t c = Inv::OpenContainerId();
    bool on = c != 0x7f000000;
    if (on && (!LS.active || c != LS.container)) {
        LS.active = true;
        LS.container = c;
        LS.name = GD::ObjName(c);
        LS.sel = 0; LS.scroll = 0; LS.selId = 0; LS.side = 0;
        LS.openTime = g_pad.now;
        LS.lastGather = -10;
        Hub_Close();
        Log("loot: container %08x '%s'", c, LS.name.c_str());
    } else if (!on && LS.active) {
        LS.active = false;
        Log("loot: closed");
        // the inventory panel the game opened with the container is still up (hidden): close it too
        Inv::CloseContainer();
    }
    return LS.active;
}

void* Loot_Panel() { return Inv::InventoryPanel(); }
bool Loot_KeepHidden(void* p) {
    if (!p || p != Inv::InventoryPanel() || !g_cfg.newMenus || !Player() || !UI_PadPresent()) return false;
    return LS.active || Inv::OpenContainerId() != 0x7f000000;
}

bool Loot_Input() {
    if (!Loot_Active()) return false;
    HidePanel();
    Refresh();
    // left / right: the container or your backpack
    if (g_pad.Pressed(PB_DLEFT) || g_pad.Pressed(PB_DRIGHT) || g_pad.Pressed(PB_LLEFT) || g_pad.Pressed(PB_LRIGHT)) {
        LS.side ^= 1; LS.sel = 0; LS.scroll = 0; LS.selId = 0;
        Refresh(true);
        Log("loot: showing %s", LS.side ? "backpack" : "container");
    }
    int n = (int)LS.items.size();
    if (n > 0) {
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) LS.sel = (LS.sel + n - 1) % n;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) LS.sel = (LS.sel + 1) % n;
        LS.selId = LS.items[LS.sel].id;
        if (g_pad.Pressed(PB_A)) {
            if (LS.side == 0) Inv::TakeItem(LS.items[LS.sel].id);
            else Inv::PutItem(LS.items[LS.sel].id);
            LS.selId = LS.sel + 1 < n ? LS.items[LS.sel + 1].id : (LS.sel > 0 ? LS.items[LS.sel - 1].id : 0);
            LS.lastGather = g_pad.now - 0.1;
        }
        if (g_pad.Pressed(PB_X)) Inv::Examine(Player(), LS.items[LS.sel].id);
    }
    if (g_pad.Pressed(PB_Y) && LS.side == 0) {
        for (auto& it : LS.items) Inv::TakeItem(it.id);
        LS.lastGather = g_pad.now - 0.1;
    }
    if (LS.side == 0 && g_pad.Pressed(PB_RB)) { Inv::ContainerPage(1); LS.lastGather = g_pad.now - 0.1; }
    if (LS.side == 0 && g_pad.Pressed(PB_LB)) { Inv::ContainerPage(-1); LS.lastGather = g_pad.now - 0.1; }
    if (g_pad.Pressed(PB_B) || g_pad.Pressed(PB_START)) Inv::CloseContainer();
    return true;
}

void Loot_Draw() {
    if (!LS.active) return;
    Refresh();
    float t = (float)fmin(1.0, (g_pad.now - LS.openTime) / 0.16);
    float a = 1 - (1 - t) * (1 - t);
    float dw = px(420);
    float w = fminf(px(820) + dw + px(10), W() - px(60));
    float rowH = px(66);
    int n = (int)LS.items.size();
    int maxVis = (int)((H() - px(330)) / rowH);
    if (maxVis < 3) maxVis = 3;
    int vis = n < maxVis ? (n > 0 ? n : 1) : maxVis;
    float h = fmaxf(px(96) + vis * rowH + px(30), n > 0 ? px(520) : 0.0f);
    if (n > 0 && h > px(96) + vis * rowH + px(30)) {      // taller for the details pane: show more rows
        vis = std::min(n, (int)((h - px(126)) / rowH)); if (vis < 1) vis = 1;
    }
    float lw = n > 0 ? w - dw - px(10) : w;
    float x = (W() - w) * 0.5f, y = (H() - h) * 0.5f - px(30) + (1 - a) * px(20);
    UIK::Panel(x, y, w, h, px(18), a);
    // header: container name + count
    // tabs: the container / your backpack
    {
        std::string c = LS.name.empty() ? "Container" : LS.name;
        float tx = x + px(30);
        Text(FONT_TITLE, px(30), tx, y + px(22), c.c_str(), LS.side == 0 ? Theme::Gold.alpha(a) : Theme::TextFaint.alpha(a));
        tx += TextWidth(FONT_TITLE, px(30), c.c_str()) + px(28);
        Text(FONT_TITLE, px(30), tx, y + px(22), "Your pack", LS.side == 1 ? Theme::Gold.alpha(a) : Theme::TextFaint.alpha(a));
    }
    char cnt[32]; snprintf(cnt, sizeof cnt, n == 1 ? "%d item" : "%d items", n);
    TextRight(FONT_BODY, px(20), x + lw - px(30), y + px(30), cnt, Theme::TextFaint.alpha(a));
    RoundRect(x + px(24), y + px(72), lw - px(48), px(1), 0, Theme::Line.alpha(0.5f * a));
    float ly = y + px(86);
    if (LS.sel < LS.scroll) LS.scroll = LS.sel;
    if (LS.sel >= LS.scroll + vis) LS.scroll = LS.sel - vis + 1;
    if (LS.scroll > n - vis) LS.scroll = n - vis;
    if (LS.scroll < 0) LS.scroll = 0;
    PushClip(x + px(10), ly, lw - px(20), vis * rowH);
    for (int i = LS.scroll; i < n && i < LS.scroll + vis + 1; i++) {
        const InvItem& it = LS.items[i];
        float ry = ly + (i - LS.scroll) * rowH;
        bool sel = i == LS.sel;
        UIK::RowSel(x + px(18), ry + px(3), lw - px(36), rowH - px(6), sel);
        Inv::DrawIcon(it, x + px(28), ry + px(7), rowH - px(14), a);
        Col nc = it.identified ? (sel ? Col(255, 244, 222) : Theme::Text) : Col(150, 170, 220);
        float tx = x + px(28) + rowH + px(6);
        TextFit(FONT_BODY, px(24), tx, ry + px(10), lw - (tx - x) - px(120), it.name.c_str(), nc.alpha(a));
        TextFit(FONT_BODY, px(17), tx, ry + px(37), lw - (tx - x) - px(120), it.typeName.c_str(), Theme::TextFaint.alpha(a));
        if (it.stack > 1) { char b[16]; snprintf(b, sizeof b, "x%d", it.stack); TextRight(FONT_BODY_BOLD, px(22), x + lw - px(40), ry + px(20), b, Theme::GoldDim.alpha(a)); }
    }
    if (n == 0) TextCentered(FONT_BODY, px(22), x + w * 0.5f, ly + px(20), LS.side ? "Nothing to put in" : "Empty", Theme::TextFaint.alpha(a));
    PopClip();
    UIK::Scrollbar(x + lw - px(14), ly + px(4), vis * rowH - px(8), LS.scroll, vis, n);
    if (n > 0 && LS.sel < n) {
        static ItemDetailsState ds;
        UI_ItemDetails(LS.items[LS.sel], x + lw, y + px(20), dw - px(20), h - px(40), a, ds);
    }
    HintItem hints[7]; int hn = 0;
    if (n > 0) {
        hints[hn++] = {GL_DPAD, "Choose"};
        hints[hn++] = {GL_A, LS.side ? "Put in" : "Take"};
        if (!LS.side) hints[hn++] = {GL_Y, "Take all"};
        hints[hn++] = {GL_X, "Examine"};
        hints[hn++] = {GL_RS, "Scroll"};
    }
    hints[hn++] = {GL_DRIGHT, LS.side ? "Container" : "Your pack"};
    hints[hn++] = {GL_B, "Close"};
    UI_DrawHints(hints, hn, a);
}
