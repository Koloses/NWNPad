// NWNPad - gamepad merchant window (replaces the game's store + inventory panels while trading)
#include "store.h"
#include "guiscan.h"
#include "inv.h"
#include "hub.h"
#include "uikit.h"
#include "render.h"
#include "game.h"
#include "gamedata.h"
#include "res.h"
#include "pad.h"
#include "config.h"
#include "log.h"
#include "item_details.h"
#include "ui.h"
#include <windows.h>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>

using namespace G;
using namespace R;

namespace {
const uintptr_t VA_App_GetWorldStore = 0x1406a9990;    // (app) -> CNWCStore*
const uintptr_t VA_Msg_StoreBuy = 0x1407c2710;         // (msg, item, x, y, repository)
const uintptr_t VA_Msg_StoreSell = 0x1407c2800;        // (msg, item)
const uintptr_t VA_Msg_StoreClose = 0x1407c2550;       // (msg)
const uintptr_t VA_Store_IsRestrictedBuy = 0x1408402f0; // (store, baseItem)
const uintptr_t VA_PanelInv_HandleXButton = 0x140854e80;
// CNWCStore (Win)
const size_t ST_Panel = 0x170, ST_Open = 0x178, ST_MarkDown = 0x17c, ST_MarkUp = 0x17e, ST_MarkDownStolen = 0x180;
const size_t ST_BlackMarket = 0x184, ST_MaxBuy = 0x18c, ST_Gold = 0x190;
// CPanelStore (Win)
const size_t PS_Box = 0xf0, PS_Page = 0x2c0, PS_NumPages = 0xb18, PS_Tab = 0xb1c, PS_TabNames = 0xd34;
// CGuiMultiInvbox (Win)
const size_t MB_Cols = 0x190, MB_Rows = 0x194, MB_Ids = 0x1a0;

typedef void (*PanelFn)(void*);
PanelFn fHide = nullptr;
typedef void (*Deactivate_t)(void*, int);
Deactivate_t fDeactivate = nullptr;
typedef void (*CreateWrite_t)(void*, unsigned, unsigned, int);
typedef void (*WriteByte_t)(void*, uint8_t, int, int);
typedef int (*GetWrite_t)(void*, uint8_t**, unsigned*);
CreateWrite_t fCreateWrite = nullptr;
WriteByte_t fWriteByte = nullptr;
GetWrite_t fGetWrite = nullptr;

struct StoreState {
    bool active = false;
    void* store = nullptr;
    std::string name;
    int col = 0;                 // 0 store, 1 backpack
    int sel[2] = {0, 0}, scroll[2] = {0, 0};
    uint32_t selId[2] = {0, 0};
    int tab = 0, page = 0;
    std::vector<InvItem> wares, pack;
    InvItem eq[ES_TOTAL];
    double openTime = 0, lastGather = -10;
    std::string toast; double toastTime = -10;
    uint32_t keeper = 0x7f000000;
} SS;

void* WorldStore() {
    if (!App()) return nullptr;
    typedef void* (*fn)(void*);
    return ((fn)A(VA_App_GetWorldStore))(App());
}
void* StorePanel() { return SS.store ? F<void*>(SS.store, ST_Panel) : nullptr; }

void SendByteMsg(uint8_t minor, uint8_t value) {
    void* m = Inv::Message();
    if (!m || !fCreateWrite || !fWriteByte || !fGetWrite) return;
    fCreateWrite(m, 1, 0xffffffff, 1);
    fWriteByte(m, value, 8, 1);
    uint8_t* data = nullptr; unsigned size = 1;
    if (fGetWrite(m, &data, &size)) {
        typedef void (*fn)(void*, uint8_t, uint8_t, uint8_t*, unsigned);
        ((fn)A(0x1407c10c0))(m, 7, minor, data, size);
    }
}

int NumTabs() { return 5; }
int NumPages() { void* p = StorePanel(); int n = p ? F<int>(p, PS_NumPages) : 1; return n < 1 ? 1 : n > 99 ? 1 : n; }

std::string TabName(int t) {
    static const char* fallback[] = {"Armour", "Weapons", "Potions", "Jewellery", "Other"};
    void* p = StorePanel();
    if (p && Readable((char*)p + PS_TabNames, 24)) {
        uint32_t ref = F<uint32_t>(p, PS_TabNames + t * 4);
        if (ref && ref < 0x01000000) {
            std::string s = Tlk(ref);
            if (!s.empty() && s.size() < 40) return s;
        }
    }
    return fallback[t % 5];
}

int BuyPrice(const InvItem& it) {
    if (!SS.store) return 0;
    int mu = F<short>(SS.store, ST_MarkUp);
    if (it.cost == 0) return 0;
    int p = (int)((mu / 100.0) * it.cost);
    return p < 1 ? 1 : p;
}

// returns -1 if the store will not buy it
int SellPrice(const InvItem& it, bool* cantAfford = nullptr) {
    if (!SS.store) return -1;
    if (!it.stolen || F<int>(SS.store, ST_BlackMarket)) {
        typedef int (*fn)(void*, int);
        if (((fn)A(VA_Store_IsRestrictedBuy))(SS.store, it.baseItem)) return -1;
    } else return -1;
    int md = it.stolen ? F<uint8_t>(SS.store, ST_MarkDownStolen) : F<uint8_t>(SS.store, ST_MarkDown);
    int p = it.cost ? (int)((md / 100.0) * it.cost) : 0;
    if (it.cost && p < 1) p = 1;
    int mx = F<int>(SS.store, ST_MaxBuy);
    if (mx != -1 && mx <= p) p = mx;
    uint32_t gold = F<uint32_t>(SS.store, ST_Gold);
    if (cantAfford) *cantAfford = gold != 0xffffffff && (uint32_t)p > gold;
    return p;
}

void GatherWares() {
    SS.wares.clear();
    void* p = StorePanel();
    if (!p || !Readable(p, PS_Box + MB_Ids + 8)) return;
    char* box = (char*)p + PS_Box;
    int cols = F<int>(box, MB_Cols), rows = F<int>(box, MB_Rows);
    uint32_t* ids = F<uint32_t*>(box, MB_Ids);
    if (!ids || cols <= 0 || rows <= 0 || cols * rows > 4096 || !Readable(ids, cols * rows * 4)) return;
    for (int i = 0; i < cols * rows; i++) {
        uint32_t id = ids[i];
        if (id == 0x7f000000 || id == 0) continue;
        InvItem it;
        if (Inv::Fill(id, it)) { it.gx = i % cols; it.gy = i / cols; SS.wares.push_back(it); }
    }
}

void Refresh(bool force = false) {
    if (!force && g_pad.now - SS.lastGather < 0.2) return;
    SS.lastGather = g_pad.now;
    GatherWares();
    Inv::Gather(Player(), SS.pack, SS.eq);
    for (int c = 0; c < 2; c++) {
        auto& v = c ? SS.pack : SS.wares;
        int n = (int)v.size();
        if (SS.selId[c]) for (int i = 0; i < n; i++) if (v[i].id == SS.selId[c]) { SS.sel[c] = i; break; }
        if (SS.sel[c] >= n) SS.sel[c] = n - 1;
        if (SS.sel[c] < 0) SS.sel[c] = 0;
    }
}

void HidePanels() {
    if (!fHide) return;
    void* p = StorePanel();
    if (p && (F<uint32_t>(p, 0xb0) & 2) == 0) fHide(p);
    void* ip = Inv::InventoryPanel();
    if (ip && (F<uint32_t>(ip, 0xb0) & 2) == 0) fHide(ip);
}

void Toast(const std::string& s) { SS.toast = s; SS.toastTime = g_pad.now; }

std::string Gp(int v) { char b[32]; snprintf(b, sizeof b, "%d gp", v); return b; }
}  // namespace

void Store_Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    fHide = (PanelFn)GetProcAddress(exe, "?Hide@CGuiPanel@@QEAAXXZ");
    fDeactivate = (Deactivate_t)GetProcAddress(exe, "?Deactivate@CGuiPanel@@QEAAXH@Z");
    fCreateWrite = (CreateWrite_t)GetProcAddress(exe, "?CreateWriteMessage@CNWMessage@@QEAAXIIH@Z");
    fWriteByte = (WriteByte_t)GetProcAddress(exe, "?WriteBYTE@CNWMessage@@QEAAXEHH@Z");
    fGetWrite = (GetWrite_t)GetProcAddress(exe, "?GetWriteMessage@CNWMessage@@QEAAHPEAPEAEPEAI@Z");
}

bool Store_Active() {
    if (!g_cfg.newMenus || !Player() || !UI_PadPresent()) {
        if (SS.active) {
            SS.active = false;
            Gui_ShowPanel(StorePanel());
            Gui_ShowPanel(Inv::InventoryPanel());
            SS.store = nullptr;
        }
        return false;
    }
    void* st = WorldStore();
    bool on = st && F<int>(st, ST_Open) != 0 && F<void*>(st, ST_Panel) != nullptr;
    if (on && !SS.active) {
        SS.active = true;
        SS.store = st;
        SS.col = 0;
        SS.sel[0] = SS.sel[1] = 0; SS.scroll[0] = SS.scroll[1] = 0;
        SS.selId[0] = SS.selId[1] = 0;
        SS.page = 0;
        SS.tab = F<int>(StorePanel(), PS_Tab);
        if (SS.tab < 0 || SS.tab > 4) SS.tab = 0;
        SS.openTime = g_pad.now;
        SS.lastGather = -10;
        SS.name = GD::ObjName(F<uint32_t>(st, OFS::Obj_Id));
        if (SS.name.empty() || SS.name.size() > 40) SS.name = "Merchant";
        Hub_Close();
        Log("store: open (tab %d, markup %d, markdown %d, gold %d)", SS.tab, F<short>(st, ST_MarkUp), F<uint8_t>(st, ST_MarkDown), F<int>(st, ST_Gold));
    } else if (!on && SS.active) {
        SS.active = false;
        SS.store = nullptr;
        Log("store: closed");
        // the game opened the inventory next to the store: close it as well
        void* ip = Inv::InventoryPanel();
        if (ip) { typedef void (*fn)(void*); ((fn)A(VA_PanelInv_HandleXButton))(ip); }
    }
    if (on) SS.store = st;
    return SS.active;
}

bool Store_KeepHidden(void* p) {
    if (!p || !g_cfg.newMenus || !Player() || !UI_PadPresent()) return false;
    void* st = WorldStore();
    if (!st || F<int>(st, ST_Open) == 0 || !F<void*>(st, ST_Panel)) return false;
    return p == F<void*>(st, ST_Panel) || p == Inv::InventoryPanel();
}

bool Store_PanelHandled(void* panel) {
    if (!panel || !Store_Active()) return false;
    return panel == StorePanel() || panel == Inv::InventoryPanel();
}

bool Store_Input() {
    if (!Store_Active()) return false;
    HidePanels();
    Refresh();
    void* pl = Player();
    auto& v = SS.col ? SS.pack : SS.wares;
    int n = (int)v.size();
    if (n > 0) {
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) SS.sel[SS.col] = (SS.sel[SS.col] + n - 1) % n;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) SS.sel[SS.col] = (SS.sel[SS.col] + 1) % n;
        SS.selId[SS.col] = v[SS.sel[SS.col]].id;
    }
    if (g_pad.Pressed(PB_DLEFT) || g_pad.Pressed(PB_LLEFT)) SS.col = 0;
    if (g_pad.Pressed(PB_DRIGHT) || g_pad.Pressed(PB_LRIGHT)) SS.col = 1;
    if (n > 0 && g_pad.Pressed(PB_A)) {
        const InvItem& it = v[SS.sel[SS.col]];
        if (SS.col == 0) {
            int price = BuyPrice(it);
            if (price > Inv::Gold(pl)) Toast("Not enough gold");
            else {
                void* m = Inv::Message();
                typedef void (*fn)(void*, uint32_t, uint8_t, uint8_t, uint32_t);
                if (m) ((fn)A(VA_Msg_StoreBuy))(m, it.id, 0xff, 0xff, 0x7f000000);
                Log("store: buy %08x '%s' %d gp", it.id, it.name.c_str(), price);
                Toast("Bought " + it.name);
            }
        } else {
            bool poor = false;
            int price = SellPrice(it, &poor);
            if (price < 0) Toast("The merchant will not buy that");
            else if (poor) Toast("The merchant cannot afford that");
            else {
                void* m = Inv::Message();
                typedef void (*fn)(void*, uint32_t);
                if (m) ((fn)A(VA_Msg_StoreSell))(m, it.id);
                Log("store: sell %08x '%s' %d gp", it.id, it.name.c_str(), price);
                Toast("Sold " + it.name);
            }
        }
        SS.lastGather = g_pad.now - 0.1;
    }
    if (n > 0 && g_pad.Pressed(PB_Y)) Inv::Examine(pl, v[SS.sel[SS.col]].id);
    // store tabs / pages
    int dt = g_pad.Pressed(PB_RB) ? 1 : g_pad.Pressed(PB_LB) ? -1 : 0;
    if (dt) {
        SS.tab = (SS.tab + dt + NumTabs()) % NumTabs();
        SS.page = 0;
        SS.sel[0] = 0; SS.selId[0] = 0; SS.scroll[0] = 0;
        if (void* p = StorePanel()) { F<int>(p, PS_Tab) = SS.tab; F<int>(p, PS_Page) = 0; }
        SendByteMsg(4, (uint8_t)SS.tab);       // Store_Open_Panel
        SS.lastGather = g_pad.now - 0.05;
    }
    int dp = g_pad.Pressed(PB_RT) ? 1 : g_pad.Pressed(PB_LT) ? -1 : 0;
    if (dp && NumPages() > 1) {
        SS.page = (SS.page + dp + NumPages()) % NumPages();
        SS.sel[0] = 0; SS.selId[0] = 0; SS.scroll[0] = 0;
        if (void* p = StorePanel()) F<int>(p, PS_Page) = SS.page;   // the item box only accepts the current page
        SendByteMsg(5, (uint8_t)SS.page);      // Store_Open_Page
        SS.lastGather = g_pad.now - 0.05;
    }
    if (g_pad.Pressed(PB_B) || g_pad.Pressed(PB_START)) {
        // same as the panel's close button: deactivating the panel tells the server we left
        void* p = StorePanel();
        Log("store: close requested");
        if (p && fDeactivate) fDeactivate(p, 0);
        else if (void* m = Inv::Message()) { typedef void (*fn)(void*); ((fn)A(VA_Msg_StoreClose))(m); }
    }
    return true;
}

static void DrawList(int c, float x, float y, float w, float h, float a) {
    auto& v = c ? SS.pack : SS.wares;
    float rowH = px(64);
    int vis = (int)(h / rowH); if (vis < 1) vis = 1;
    int n = (int)v.size();
    int& sel = SS.sel[c];
    int& scroll = SS.scroll[c];
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + vis) scroll = sel - vis + 1;
    if (scroll > n - vis) scroll = n - vis;
    if (scroll < 0) scroll = 0;
    bool focus = SS.col == c;
    RoundRect(x, y, w, h, px(12), Col(8, 7, 6, (int)(120 * a)));
    if (focus) RoundRectBorder(x, y, w, h, px(12), px(1), Theme::Line.alpha(0.5f * a));
    PushClip(x, y, w, h);
    int myGold = Inv::Gold(Player());
    for (int i = scroll; i < n && i < scroll + vis + 1; i++) {
        const InvItem& it = v[i];
        float ry = y + (i - scroll) * rowH;
        bool s = focus && i == sel;
        UIK::RowSel(x + px(4), ry + px(3), w - px(8), rowH - px(6), s);
        Inv::DrawIcon(it, x + px(12), ry + px(7), rowH - px(14), a);
        float tx = x + px(12) + rowH + px(4);
        Col nc = it.identified ? (s ? Col(255, 244, 222) : Theme::Text) : Col(150, 170, 220);
        std::string nm = it.name;
        if (it.stack > 1) { char b[16]; snprintf(b, sizeof b, "  x%d", it.stack); nm += b; }
        TextFit(FONT_BODY, px(22), tx, ry + px(9), w - (tx - x) - px(120), nm.c_str(), nc.alpha(a));
        TextFit(FONT_BODY, px(16), tx, ry + px(35), w - (tx - x) - px(120), it.typeName.c_str(), Theme::TextFaint.alpha(a));
        // price
        std::string pr; Col pc = Theme::Gold;
        if (c == 0) {
            int p = BuyPrice(it);
            pr = Gp(p);
            if (p > myGold) pc = Theme::Hostile;
        } else {
            bool poor = false;
            int p = SellPrice(it, &poor);
            if (p < 0) { pr = "—"; pc = Theme::TextFaint; }
            else { pr = Gp(p); if (poor) pc = Theme::Hostile; }
        }
        TextRight(FONT_BODY_BOLD, px(20), x + w - px(18), ry + px(20), pr.c_str(), pc.alpha(a));
    }
    if (n == 0) TextCentered(FONT_BODY, px(20), x + w * 0.5f, y + px(24), c ? "Nothing to sell" : "Nothing on this page", Theme::TextFaint.alpha(a));
    PopClip();
    UIK::Scrollbar(x + w - px(8), y + px(6), h - px(12), scroll, vis, n);
}

void Store_Draw() {
    if (!SS.active) return;
    Refresh();
    float t = (float)fmin(1.0, (g_pad.now - SS.openTime) / 0.16);
    float a = 1 - (1 - t) * (1 - t);
    R::Rect(0, 0, (float)W(), (float)H(), Col(0, 0, 0, (int)(130 * a)));
    float w = fminf(px(1760), W() - px(60));
    float h = fminf(px(860), H() - px(150));
    float x = (W() - w) * 0.5f, y = (H() - h) * 0.5f - px(30) + (1 - a) * px(20);
    UIK::Panel(x, y, w, h, px(20), a);
    float dw = px(400);
    float colW = (w - px(36 * 2 + 30 * 2) - dw) * 0.5f;
    float lx = x + px(36), rx = lx + colW + px(30);
    // headers
    float hy = y + px(22);
    Text(FONT_TITLE, px(28), lx, hy, SS.name.c_str(), Theme::Gold.alpha(a));
    float gs = px(26);
    if (NumPages() > 1) {
        char pg[32]; snprintf(pg, sizeof pg, "%d / %d", SS.page + 1, NumPages());
        float pw = TextWidth(FONT_BODY, px(19), pg);
        float gw = gs * 1.45f;
        float px0 = lx + colW - (gw * 2 + pw + px(20));
        UIK::Glyph(GL_LT, px0, hy + px(4), gs, a);
        Text(FONT_BODY, px(19), px0 + gw + px(10), hy + px(6), pg, Theme::TextDim.alpha(a));
        UIK::Glyph(GL_RT, px0 + gw + px(20) + pw, hy + px(4), gs, a);
    }
    // store categories (fit into the column)
    float ty = hy + px(46);
    {
        float gw = gs * 1.45f;
        float avail = colW - 2 * gw - px(24);
        float fs = px(19), gap = px(22);
        std::string names[5]; float total = 0;
        for (int k = 0; k < 6; k++) {
            total = 0;
            for (int i = 0; i < NumTabs(); i++) { if (k == 0) names[i] = TabName(i); total += TextWidth(FONT_BODY_BOLD, fs, names[i].c_str()) + gap; }
            if (total <= avail) break;
            fs *= 0.9f; gap *= 0.85f;
        }
        float tx = lx;
        UIK::Glyph(GL_LB, tx, ty, gs, a); tx += gw + px(12);
        for (int i = 0; i < NumTabs(); i++) {
            float tw = TextWidth(FONT_BODY_BOLD, fs, names[i].c_str());
            bool on = i == SS.tab;
            if (on) RoundRect(tx - px(8), ty - px(2), tw + px(16), px(30), px(15), Theme::Sel0.alpha(a));
            Text(FONT_BODY_BOLD, fs, tx, ty + px(2) + (px(19) - fs) * 0.5f, names[i].c_str(), (on ? Col(255, 240, 210) : Theme::TextFaint).alpha(a));
            tx += tw + gap;
        }
        UIK::Glyph(GL_RB, lx + colW - gw, ty, gs, a);
    }
    // right header: your backpack + gold
    Text(FONT_TITLE, px(28), rx, hy, "Your backpack", Theme::Gold.alpha(a));
    {
        std::string g = Gp(Inv::Gold(Player()));
        float gw = TextWidth(FONT_BODY_BOLD, px(22), g.c_str());
        float gx = rx + colW - gw;
        Circle(gx - px(16), hy + px(16), px(9), Col(222, 180, 70, (int)(255 * a)));
        Text(FONT_BODY_BOLD, px(22), gx, hy + px(4), g.c_str(), Theme::Gold.alpha(a));
        if (SS.store) {
            int sg = F<int>(SS.store, ST_Gold);
            std::string m = sg == -1 ? std::string("Merchant gold: unlimited") : "Merchant gold: " + Gp(sg);
            TextRight(FONT_BODY, px(18), rx + colW, ty + px(4), m.c_str(), Theme::TextFaint.alpha(a));
        }
    }
    float ly = ty + px(46);
    float lh = y + h - ly - px(24);
    DrawList(0, lx, ly, colW, lh, a);
    DrawList(1, rx, ly, colW, lh, a);
    {
        float dx = rx + colW + px(30);
        Text(FONT_TITLE, px(28), dx, hy, "Details", Theme::Gold.alpha(a));
        auto& v = SS.col ? SS.pack : SS.wares;
        int si = SS.sel[SS.col];
        if (si >= 0 && si < (int)v.size()) {
            static ItemDetailsState ds;
            std::string extra;
            if (SS.col == 0) extra = "Price " + Gp(BuyPrice(v[si]));
            else { bool poor = false; int p = SellPrice(v[si], &poor); extra = p < 0 ? std::string("Not wanted here") : "Sells for " + Gp(p); }
            UI_ItemDetails(v[si], dx, ly, dw, lh, a, ds, extra);
        }
    }
    // toast
    if (g_pad.now - SS.toastTime < 2.0) {
        float ta = (float)fmin(1.0, (2.0 - (g_pad.now - SS.toastTime)) / 0.4) * a;
        float tw = TextWidth(FONT_BODY, px(22), SS.toast.c_str()) + px(40);
        float tx2 = (W() - tw) * 0.5f, ty2 = y + h - px(60);
        RoundRect(tx2, ty2, tw, px(40), px(20), Col(30, 26, 20, (int)(230 * ta)));
        RoundRectBorder(tx2, ty2, tw, px(40), px(20), px(1), Theme::Line.alpha(ta));
        TextCentered(FONT_BODY, px(22), W() * 0.5f, ty2 + px(7), SS.toast.c_str(), Theme::Text.alpha(ta));
    }
    HintItem hints[8]; int hn = 0;
    hints[hn++] = {GL_DPAD, "Choose"};
    hints[hn++] = {GL_A, SS.col == 0 ? "Buy" : "Sell"};
    hints[hn++] = {GL_Y, "Examine"};
    hints[hn++] = {GL_RS, "Scroll"};
    hints[hn++] = {GL_LB, "Category"};
    if (NumPages() > 1) hints[hn++] = {GL_LT, "Page"};
    hints[hn++] = {GL_B, "Leave"};
    UI_DrawHints(hints, hn, a);
}
