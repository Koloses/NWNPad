// NWNPad - hub "Spells" page: spells per class and level, cast, put on the quickbar, prepare
#include "hub_pages.h"
#include "hub.h"
#include "render.h"
#include "game.h"
#include "gamedata.h"
#include "twoda.h"
#include "res.h"
#include "inv.h"
#include "quickbar.h"
#include "control.h"
#include "pad.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

using namespace G;
using namespace R;

namespace {
// ---- game data (Win) -------------------------------------------------------------------------
// CNWCCreatureStats: number of classes +0x69, class info array +0x148 (0x150 each)
// class info: known spells CExoArrayList<uint32>[10] at +0 (0x10 each), memorized
//   CExoArrayList<CNWCStats_Spell*>[10] at +0xa0, spells per day left uint8[10] at +0x140,
//   class id +0x14d, class level +0x14e
// CNWCStats_Spell: id +0, ready +4, metamagic +8, domain +0x10
const uintptr_t VA_QuickButton_ActionCastSpell = 0x140858790;   // reads the spell menu id at this+0x110
const uintptr_t VA_QuickButton_SetSpellMenuID = 0x1408678d0;
const uintptr_t VA_Msg_SpellMemorize = 0x1407c2370;            // (msg, class idx, spell, slot, domain level, meta)
const uintptr_t VA_Msg_SendPlayerToServerMessage = 0x1407c10c0;

struct SpellInfo { std::string name, desc, icon, school; int innate = 0; std::vector<int> sub; bool ok = false; };
struct ClassInfo { int idx, id, level; std::string name; bool prepared; std::string column; };
struct Entry { int id, level, meta, ready, total; bool domain; };
struct Slot { int id, meta; bool ready, domain; };

char* Stats() { void* pl = Player(); return pl ? F<char*>(pl, OFS::Cre_Stats) : nullptr; }
char* CI(int idx) { char* s = Stats(); return s ? s + 0x148 + idx * 0x150 : nullptr; }

const SpellInfo& Spell(int id) {
    static std::map<int, SpellInfo> cache;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    SpellInfo& s = cache[id];
    const TwoDA& t = TDA("spells");
    if (!t.ok() || id < 0 || id >= t.Rows()) return s;
    int nm = t.Int(id, "Name");
    if (nm < 0) return s;
    s.ok = true;
    s.name = Tlk((uint32_t)nm);
    if (s.name.empty()) s.name = t.Get(id, "Label");
    int d = t.Int(id, "SpellDesc");
    if (d >= 0) s.desc = Tlk((uint32_t)d);
    s.icon = t.Get(id, "IconResRef");
    static const char* SCH = "ACDEVINTG";
    static const char* SCHN[] = {"Abjuration", "Conjuration", "Divination", "Enchantment", "Evocation", "Illusion", "Necromancy", "Transmutation", "Universal"};
    const std::string& sc = t.Get(id, "School");
    if (!sc.empty()) { const char* p = strchr(SCH, sc[0]); if (p) s.school = SCHN[p - SCH]; }
    s.innate = t.Int(id, "Innate", 0);
    for (int k = 1; k <= 5; k++) {
        char c[16]; snprintf(c, sizeof c, "SubRadSpell%d", k);
        int v = t.Int(id, c, 0);
        if (v > 0) s.sub.push_back(v);
    }
    return s;
}

std::vector<ClassInfo> Classes() {
    std::vector<ClassInfo> v;
    char* st = Stats();
    if (!st) return v;
    int n = F<uint8_t>(st, 0x69);
    if (n > 8) n = 8;
    const TwoDA& ct = TDA("classes");
    for (int i = 0; i < n; i++) {
        char* ci = CI(i);
        int id = F<uint8_t>(ci, 0x14d);
        if (id == 0xff) continue;
        if (ct.Int(id, "SpellCaster", 0) == 0) continue;
        ClassInfo c;
        c.idx = i; c.id = id; c.level = F<uint8_t>(ci, 0x14e);
        int nm = ct.Int(id, "Name");
        c.name = nm >= 0 ? Tlk((uint32_t)nm) : ct.Get(id, "Label");
        c.prepared = ct.Int(id, "MemorizesSpells", 0) != 0;
        c.column = ct.Get(id, "SpellTableColumn");
        if (c.column.empty()) {
            switch (id) { case 1: c.column = "Bard"; break; case 2: c.column = "Cleric"; break; case 3: c.column = "Druid"; break;
                          case 6: c.column = "Paladin"; break; case 7: c.column = "Ranger"; break; default: c.column = "Wiz_Sorc"; }
        }
        v.push_back(c);
    }
    return v;
}

std::vector<int> Known(int idx, int lvl) {
    std::vector<int> v;
    char* ci = CI(idx);
    if (!ci) return v;
    uint32_t* arr = F<uint32_t*>(ci, lvl * 0x10);
    int n = F<int>(ci, lvl * 0x10 + 8);
    if (!arr || n <= 0 || n > 500) return v;
    for (int i = 0; i < n; i++) v.push_back((int)arr[i]);
    return v;
}

std::vector<Slot> Slots(int idx, int lvl) {
    std::vector<Slot> v;
    char* ci = CI(idx);
    if (!ci) return v;
    void** arr = F<void**>(ci, 0xa0 + lvl * 0x10);
    int n = F<int>(ci, 0xa8 + lvl * 0x10);
    if (n <= 0 || n > 100) return v;
    for (int i = 0; i < n; i++) {
        Slot s{-1, 0, false, false};
        char* e = arr ? (char*)arr[i] : nullptr;
        if (e && Readable(e, 0x14)) {
            s.id = F<int>(e, 0); s.ready = F<int>(e, 4) != 0; s.meta = F<uint8_t>(e, 8); s.domain = F<int>(e, 0x10) != 0;
        }
        v.push_back(s);
    }
    return v;
}

int PerDayLeft(int idx, int lvl) { char* ci = CI(idx); return ci ? F<uint8_t>(ci, 0x140 + lvl) : 0; }

std::vector<Entry> Castable(const ClassInfo& c, int lvl) {
    std::vector<Entry> v;
    if (c.prepared) {
        for (const Slot& s : Slots(c.idx, lvl)) {
            if (s.id < 0) continue;
            auto it = std::find_if(v.begin(), v.end(), [&](const Entry& e) { return e.id == s.id && e.meta == s.meta && e.domain == s.domain; });
            if (it == v.end()) { v.push_back({s.id, lvl, s.meta, s.ready ? 1 : 0, 1, s.domain}); }
            else { it->total++; if (s.ready) it->ready++; }
        }
    } else {
        int left = PerDayLeft(c.idx, lvl);
        for (int id : Known(c.idx, lvl)) v.push_back({id, lvl, 0, left, left, false});
    }
    std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return Spell(a.id).name < Spell(b.id).name; });
    return v;
}

// spells a prepared caster can put in a slot of this level
std::vector<int> Preparable(const ClassInfo& c, int lvl) {
    std::vector<int> v = Known(c.idx, lvl);         // wizards: the spellbook
    if (v.empty()) {                                 // divine casters: every spell of the class at this level
        const TwoDA& t = TDA("spells");
        int col = t.Col(c.column.c_str());
        if (col >= 0)
            for (int i = 0; i < t.Rows(); i++)
                if (t.Int(i, col, -1) == lvl && t.Int(i, "UserType", 1) == 1 && Spell(i).ok) v.push_back(i);
    }
    std::sort(v.begin(), v.end(), [](int a, int b) { return Spell(a).name < Spell(b).name; });
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

uint64_t MenuId(int spell, int lvl, int clsIdx, int meta) {
    return (uint64_t)(spell & 0xffff) | ((uint64_t)(lvl & 0xf) << 24) | ((uint64_t)(clsIdx & 3) << 28) |
           ((uint64_t)((clsIdx >> 2) & 1) << 38) | ((uint64_t)(meta & 0x3f) << 31);
}

void Cast(uint64_t menuId) {
    alignas(16) static char fake[0x200];
    memset(fake, 0, sizeof fake);
    F<uint64_t>(fake, 0x110) = menuId;
    typedef void (*fn)(void*);
    ((fn)A(VA_QuickButton_ActionCastSpell))(fake);
}

void Memorize(int clsIdx, int spell, int slot) {
    void* m = Inv::Message();
    if (!m) return;
    typedef void (*fn)(void*, uint8_t, uint32_t, uint8_t, uint8_t, uint8_t);
    ((fn)A(VA_Msg_SpellMemorize))(m, (uint8_t)clsIdx, (uint32_t)spell, (uint8_t)slot, 0, 0);
}

void UnMemorize(int clsIdx, int lvl, int slot) {
    void* m = Inv::Message();
    if (!m) return;
    HMODULE exe = GetModuleHandleA(nullptr);
    typedef void (*Create_t)(void*, uint32_t, uint32_t, int);
    typedef void (*WriteByte_t)(void*, uint8_t, int, int);
    typedef int (*GetWrite_t)(void*, uint8_t**, uint32_t*);
    static Create_t fCreate = (Create_t)GetProcAddress(exe, "?CreateWriteMessage@CNWMessage@@QEAAXIIH@Z");
    static WriteByte_t fByte = (WriteByte_t)GetProcAddress(exe, "?WriteBYTE@CNWMessage@@QEAAXEHH@Z");
    static GetWrite_t fGet = (GetWrite_t)GetProcAddress(exe, "?GetWriteMessage@CNWMessage@@QEAAHPEAPEAEPEAI@Z");
    if (!fCreate || !fByte || !fGet) return;
    fCreate(m, 3, 0xffffffff, 1);
    fByte(m, (uint8_t)clsIdx, 8, 1);
    fByte(m, (uint8_t)lvl, 8, 1);
    fByte(m, (uint8_t)slot, 8, 1);
    uint8_t* data = nullptr; uint32_t len = 3;
    if (fGet(m, &data, &len)) {
        typedef int (*send_t)(void*, uint8_t, uint8_t, uint8_t*, uint32_t);
        ((send_t)A(VA_Msg_SendPlayerToServerMessage))(m, 6, 0x11, data, len);
    }
}

// ---- page state ------------------------------------------------------------------------------
enum Overlay { OV_NONE, OV_QUICKBAR, OV_SUB, OV_PREPARE };
struct State {
    int cls = 0, lvl = 1, sel = 0, scroll = 0;
    bool prepMode = false;
    Overlay ov = OV_NONE;
    int ovSel = 0, ovScroll = 0;
    int qbBank = 0, qbSlot = 0;
    uint64_t pendingMenu = 0;      // spell waiting for a quickbar slot / sub-spell choice
    bool subForQuickbar = false;
    std::vector<int> ovList;
    float descScroll = 0; int descFor = -1;
} S;

std::vector<ClassInfo> g_cls;
double g_lastRead = -10;

void ReadClasses() {
    if (g_pad.now - g_lastRead < 0.5) return;
    g_lastRead = g_pad.now;
    g_cls = Classes();
    if (S.cls >= (int)g_cls.size()) S.cls = 0;
}

const ClassInfo* Cur() { return S.cls < (int)g_cls.size() ? &g_cls[S.cls] : nullptr; }

int LevelCount(const ClassInfo& c, int lvl) {
    if (c.prepared) return (int)Slots(c.idx, lvl).size();
    return (int)Known(c.idx, lvl).size();
}

void FirstLevelWithSpells() {
    const ClassInfo* c = Cur();
    if (!c) return;
    if (LevelCount(*c, S.lvl) > 0) return;
    for (int l = 0; l < 10; l++) if (LevelCount(*c, l) > 0) { S.lvl = l; return; }
}

void Open() {
    g_lastRead = -10;
    ReadClasses();
    S.ov = OV_NONE;
    FirstLevelWithSpells();
}

void StartCast(const ClassInfo& c, const Entry& e) {
    if (e.ready <= 0) { Hub_Toast("No uses left - rest to recover"); return; }
    const SpellInfo& si = Spell(e.id);
    if (!si.sub.empty()) {
        S.ov = OV_SUB; S.ovSel = 0; S.ovScroll = 0; S.ovList = si.sub; S.subForQuickbar = false;
        S.pendingMenu = MenuId(e.id, e.level, c.idx, e.meta);
        return;
    }
    Log("spells: cast %d (%s) level %d class %d meta %d", e.id, si.name.c_str(), e.level, c.idx, e.meta);
    Hub_Close();
    Cast(MenuId(e.id, e.level, c.idx, e.meta));
}

void StartQuickbar(const ClassInfo& c, const Entry& e) {
    S.pendingMenu = MenuId(e.id, e.level, c.idx, e.meta);
    S.ov = OV_QUICKBAR;
    S.qbBank = g_ctl.quickBank;
    // first empty slot of the active set
    S.qbSlot = 0;
    for (int k = 0; k < 36; k++) {
        int b = (g_ctl.quickBank + k / 12) % 3, i = k % 12;
        if (QB_Empty(b, i)) { S.qbBank = b; S.qbSlot = i; break; }
    }
}

void OverlayInput() {
    const ClassInfo* c = Cur();
    if (g_pad.Pressed(PB_B)) { S.ov = OV_NONE; return; }
    if (S.ov == OV_QUICKBAR) {
        if (g_pad.Repeat(PB_DLEFT) || g_pad.Repeat(PB_LLEFT)) S.qbSlot = (S.qbSlot + 11) % 12;
        if (g_pad.Repeat(PB_DRIGHT) || g_pad.Repeat(PB_LRIGHT)) S.qbSlot = (S.qbSlot + 1) % 12;
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) S.qbBank = (S.qbBank + 2) % 3;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) S.qbBank = (S.qbBank + 1) % 3;
        if (g_pad.Pressed(PB_A)) {
            void* b = Control_QuickButton(S.qbBank, S.qbSlot);
            if (b) {
                typedef void (*fn)(void*, uint64_t);
                ((fn)A(VA_QuickButton_SetSpellMenuID))(b, S.pendingMenu);
                Log("spells: quickbar %d/%d <- menu %llx", S.qbBank, S.qbSlot, (unsigned long long)S.pendingMenu);
                Hub_Toast("Added to the quickbar");
            }
            S.ov = OV_NONE;
        }
        return;
    }
    int n = (int)S.ovList.size();
    if (n > 0) {
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) S.ovSel = (S.ovSel + n - 1) % n;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) S.ovSel = (S.ovSel + 1) % n;
    }
    if (!g_pad.Pressed(PB_A) || n == 0 || !c) return;
    int pick = S.ovList[S.ovSel];
    if (S.ov == OV_SUB) {
        uint64_t m = (S.pendingMenu & ~0xffffull) | (uint64_t)(pick & 0xffff);
        S.ov = OV_NONE;
        Log("spells: cast sub-spell %d", pick);
        Hub_Close();
        Cast(m);
    } else if (S.ov == OV_PREPARE) {
        Memorize(c->idx, pick, S.sel);
        Log("spells: memorize %d in class %d level %d slot %d", pick, c->idx, S.lvl, S.sel);
        Hub_Toast("Prepared " + Spell(pick).name + " - rest to ready it");
        S.ov = OV_NONE;
        g_lastRead = -10;
    }
}

void Input() {
    ReadClasses();
    if (S.ov != OV_NONE) { OverlayInput(); return; }
    if (g_pad.Pressed(PB_B)) { if (S.prepMode) S.prepMode = false; else Hub_Close(); return; }
    const ClassInfo* c = Cur();
    if (!c) return;
    if (g_pad.Pressed(PB_Y) && g_cls.size() > 1) { S.cls = (S.cls + 1) % (int)g_cls.size(); S.sel = 0; S.prepMode = false; FirstLevelWithSpells(); }
    if (g_pad.Repeat(PB_RT) || g_pad.Repeat(PB_DRIGHT)) { S.lvl = (S.lvl + 1) % 10; S.sel = 0; }
    if (g_pad.Repeat(PB_LT) || g_pad.Repeat(PB_DLEFT)) { S.lvl = (S.lvl + 9) % 10; S.sel = 0; }
    if (g_pad.Pressed(PB_BACK) && c->prepared) { S.prepMode = !S.prepMode; S.sel = 0; }
    int n;
    std::vector<Entry> ent;
    std::vector<Slot> slots;
    if (S.prepMode) { slots = Slots(c->idx, S.lvl); n = (int)slots.size(); }
    else { ent = Castable(*c, S.lvl); n = (int)ent.size(); }
    if (n > 0) {
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) S.sel = (S.sel + n - 1) % n;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) S.sel = (S.sel + 1) % n;
    }
    if (S.sel >= n) S.sel = n > 0 ? n - 1 : 0;
    if (fabsf(g_pad.ry) > 0.2f) S.descScroll -= g_pad.ry * px(700.0f) * (float)g_pad.dt;
    if (n == 0) return;
    if (S.prepMode) {
        const Slot& s = slots[S.sel];
        if (g_pad.Pressed(PB_A)) {
            if (s.domain) { Hub_Toast("Domain slots keep their domain spell"); return; }
            S.ovList = Preparable(*c, S.lvl);
            if (S.ovList.empty()) { Hub_Toast("No spells of this level to prepare"); return; }
            S.ov = OV_PREPARE; S.ovSel = 0; S.ovScroll = 0;
            for (int i = 0; i < (int)S.ovList.size(); i++) if (S.ovList[i] == s.id) S.ovSel = i;
        }
        if (g_pad.Pressed(PB_X) && s.id >= 0 && !s.domain) {
            UnMemorize(c->idx, S.lvl, S.sel);
            Log("spells: clear class %d level %d slot %d", c->idx, S.lvl, S.sel);
            Hub_Toast("Slot cleared");
            g_lastRead = -10;
        }
    } else {
        const Entry& e = ent[S.sel];
        if (g_pad.Pressed(PB_A)) StartCast(*c, e);
        else if (g_pad.Pressed(PB_X)) StartQuickbar(*c, e);
    }
}

// ---- drawing ---------------------------------------------------------------------------------
const char* LevelName(int l) {
    static const char* N[] = {"Cantrips", "Level 1", "Level 2", "Level 3", "Level 4", "Level 5", "Level 6", "Level 7", "Level 8", "Level 9"};
    return N[l];
}

void SpellIcon(int id, float x, float y, float s, float a, bool dim = false) {
    const SpellInfo& si = Spell(id);
    RoundRect(x, y, s, s, px(8), Col(0, 0, 0, (int)(110 * a)));
    uint32_t tex = si.icon.empty() ? 0 : Res_Texture(si.icon.c_str());
    if (tex) ImageRounded(tex, x + s * 0.06f, y + s * 0.06f, s * 0.88f, s * 0.88f, px(6), (dim ? Col(110, 110, 110) : Col(255, 255, 255)).alpha(a));
}

std::string MetaName(int meta) {
    std::string r;
    static const struct { int bit; const char* n; } M[] = {{1, "Empowered"}, {2, "Extended"}, {4, "Maximized"}, {8, "Quickened"}, {16, "Silent"}, {32, "Still"}};
    for (auto& m : M) if (meta & m.bit) { if (!r.empty()) r += ", "; r += m.n; }
    return r;
}

void DrawDetails(int id, float x, float y, float w, float h, float a, const std::string& extra) {
    const SpellInfo& si = Spell(id);
    float is = px(84);
    SpellIcon(id, x, y, is, a);
    float tx = x + is + px(18), tw = w - is - px(18);
    float ty = y + px(2);
    ty += TextWrap(FONT_TITLE, px(24), tx, ty, tw, si.name.c_str(), Theme::Gold.alpha(a)) + px(4);
    if (!si.school.empty()) { Text(FONT_BODY, px(18), tx, ty, si.school.c_str(), Theme::TextDim.alpha(a)); ty += px(26); }
    if (!extra.empty()) { TextFit(FONT_BODY_BOLD, px(17), tx, ty, tw, extra.c_str(), Theme::Friendly.alpha(a)); ty += px(24); }
    float cy = fmaxf(y + is + px(16), ty + px(6));
    RoundRect(x, cy, w, px(1), 0, Theme::Line.alpha(0.4f * a));
    cy += px(12);
    float dh = y + h - cy - px(56);
    if (S.descFor != id) { S.descFor = id; S.descScroll = 0; }
    PushClip(x - px(4), cy, w + px(8), dh);
    float full = TextWrap(FONT_BODY, px(18), 0, 0, w - px(16), si.desc.c_str(), Theme::Text, 0, 1.2f, true);
    float maxS = fmaxf(0.0f, full - dh + px(10));
    S.descScroll = fminf(fmaxf(S.descScroll, 0.0f), maxS);
    TextWrap(FONT_BODY, px(18), x, cy - S.descScroll, w - px(16), si.desc.c_str(), Theme::Text.alpha(0.92f * a), 0, 1.2f);
    PopClip();
    if (maxS > 0) {
        float t2 = dh * dh / full;
        RoundRect(x + w - px(5), cy + (dh - t2) * (S.descScroll / maxS), px(4), t2, px(2), Theme::GoldDim.alpha(a));
    }
}

void DrawOverlay(float x, float y, float w, float h, float a) {
    R::Rect(x, y, w, h, Col(0, 0, 0, (int)(140 * a)));
    if (S.ov == OV_QUICKBAR) {
        float tile = px(78), gap = px(10);
        float ow = tile * 12 + gap * 11 + px(80), oh = px(200) + 3 * (tile + px(14));
        float ox = x + (w - ow) * 0.5f, oy = y + (h - oh) * 0.5f;
        UIK::Panel(ox, oy, ow, oh, px(16), a);
        Text(FONT_TITLE, px(26), ox + px(40), oy + px(26), "Add to quickbar", Theme::Gold.alpha(a));
        const SpellInfo& si = Spell((int)(S.pendingMenu & 0xffff));
        TextRight(FONT_BODY, px(20), ox + ow - px(40), oy + px(32), si.name.c_str(), Theme::TextDim.alpha(a));
        static const char* setN[3] = {"I", "II", "III"};
        float gy = oy + px(84);
        for (int b = 0; b < 3; b++) {
            float ry = gy + b * (tile + px(14));
            Text(FONT_TITLE, px(20), ox + px(14), ry + tile * 0.5f - px(13), setN[b], (b == S.qbBank ? Theme::Gold : Theme::TextFaint).alpha(a));
            for (int s = 0; s < 12; s++) {
                float tx = ox + px(40) + s * (tile + gap);
                RoundRect(tx, ry, tile, tile, px(8), Col(8, 7, 6, (int)(200 * a)));
                RoundRectBorder(tx, ry, tile, tile, px(8), px(1), Theme::Line.alpha(0.35f * a));
                uint32_t tex = QB_Icon(b, s);
                if (tex) ImageRounded(tex, tx + tile * 0.1f, ry + tile * 0.1f, tile * 0.8f, tile * 0.8f, px(6), Col(255, 255, 255, (int)(170 * a)));
                if (b == S.qbBank && s == S.qbSlot) {
                    float p = 0.6f + 0.4f * sinf((float)g_pad.now * 5.0f);
                    RoundRectBorder(tx - px(3), ry - px(3), tile + px(6), tile + px(6), px(10), px(2.5f), Theme::LineStrong.alpha(p * a));
                }
            }
        }
        std::string cur = QB_Name(S.qbBank, S.qbSlot);
        std::string line = cur.empty() ? "Empty slot" : "Replaces: " + cur;
        Text(FONT_BODY, px(19), ox + px(40), oy + oh - px(56), line.c_str(), Theme::TextDim.alpha(a));
        float hx = ox + ow - px(40) - UIK::HintWidth(GL_A, "Place", px(28)) - UIK::HintWidth(GL_B, "Cancel", px(28)) - px(24);
        hx += UIK::Hint(GL_A, "Place", hx, oy + oh - px(60), px(28), a) + px(24);
        UIK::Hint(GL_B, "Cancel", hx, oy + oh - px(60), px(28), a);
        return;
    }
    // list overlays (prepare / sub-spell)
    float ow = fminf(px(900), w - px(80)), oh = h - px(80);
    float ox = x + (w - ow) * 0.5f, oy = y + px(40);
    UIK::Panel(ox, oy, ow, oh, px(16), a);
    Text(FONT_TITLE, px(26), ox + px(30), oy + px(22), S.ov == OV_SUB ? "Choose a version" : "Prepare a spell", Theme::Gold.alpha(a));
    float lw = ow * 0.48f, lx = ox + px(20), ly = oy + px(72), lh = oh - px(90);
    float rowH = px(52);
    int n = (int)S.ovList.size();
    int vis = (int)(lh / rowH); if (vis < 1) vis = 1;
    if (S.ovSel < S.ovScroll) S.ovScroll = S.ovSel;
    if (S.ovSel >= S.ovScroll + vis) S.ovScroll = S.ovSel - vis + 1;
    PushClip(lx, ly, lw, lh);
    for (int i = S.ovScroll; i < n && i < S.ovScroll + vis + 1; i++) {
        float ry = ly + (i - S.ovScroll) * rowH;
        bool sel = i == S.ovSel;
        UIK::RowSel(lx, ry + px(2), lw, rowH - px(4), sel);
        SpellIcon(S.ovList[i], lx + px(8), ry + px(6), rowH - px(12), a);
        TextFit(FONT_BODY, px(21), lx + rowH + px(10), ry + px(12), lw - rowH - px(20), Spell(S.ovList[i]).name.c_str(), (sel ? Col(255, 244, 222) : Theme::Text).alpha(a));
    }
    PopClip();
    UIK::Scrollbar(lx + lw - px(6), ly, lh, S.ovScroll, vis, n);
    if (n > 0) DrawDetails(S.ovList[S.ovSel], lx + lw + px(30), ly, ow - lw - px(70), lh, a, "");
}

void Draw(float x, float y, float w, float h, float a) {
    ReadClasses();
    float px0 = x + px(36);
    if (g_cls.empty()) {
        TextCentered(FONT_TITLE, px(28), x + w * 0.5f, y + h * 0.4f, "No spellcasting classes", Theme::TextFaint.alpha(a));
        return;
    }
    const ClassInfo& c = g_cls[S.cls];
    // header: class, level tabs
    float hy = y + px(16);
    char cl[96]; snprintf(cl, sizeof cl, "%s %d", c.name.c_str(), c.level);
    float cw = Text(FONT_TITLE, px(26), px0, hy + px(6), cl, Theme::Gold.alpha(a));
    if (g_cls.size() > 1) UIK::Hint(GL_Y, "Class", px0 + cw + px(16), hy + px(6), px(26), a);
    float tabsX = px0 + px(390);
    float tabW = (x + w - px(36) - tabsX - px(80)) / 10;
    UIK::Glyph(GL_LT, tabsX - px(48), hy + px(6), px(28), a);
    for (int l = 0; l < 10; l++) {
        float tx = tabsX + l * tabW;
        bool on = l == S.lvl;
        int cnt = LevelCount(c, l);
        RoundRect(tx + px(3), hy, tabW - px(6), px(44), px(10), on ? Theme::Sel1.alpha(0.85f * a) : Col(8, 7, 6, (int)(110 * a)));
        if (on) RoundRectBorder(tx + px(3), hy, tabW - px(6), px(44), px(10), px(1), Theme::LineStrong.alpha(0.8f * a));
        const char* lab = l == 0 ? "0" : LevelName(l) + 6;
        Col tc = on ? Theme::Gold : cnt ? Theme::Text : Theme::TextFaint;
        TextCentered(FONT_TITLE, px(22), tx + tabW * 0.5f, hy + px(9), lab, tc.alpha(a));
        if (cnt) Circle(tx + tabW * 0.5f, hy + px(40), px(2.5f), (on ? Theme::Gold : Theme::GoldDim).alpha(a));
    }
    UIK::Glyph(GL_RT, tabsX + 10 * tabW + px(12), hy + px(6), px(28), a);
    float by = hy + px(64);
    // left: list
    float dw = px(440);
    float lx = px0, lw = w - dw - px(36) * 2 - px(30), ly = by + px(40), lh = y + h - px(20) - ly;
    std::string title = std::string(LevelName(S.lvl)) + (S.prepMode ? "  ·  Prepare" : "");
    Text(FONT_TITLE, px(22), lx, by, title.c_str(), Theme::GoldDim.alpha(a));
    if (!c.prepared) {
        char b[64]; snprintf(b, sizeof b, "%d casts left today", PerDayLeft(c.idx, S.lvl));
        TextRight(FONT_BODY, px(20), lx + lw, by + px(2), b, Theme::TextDim.alpha(a));
    } else {
        std::vector<Slot> sl = Slots(c.idx, S.lvl);
        int rd = 0; for (auto& s : sl) if (s.id >= 0 && s.ready) rd++;
        char b[64]; snprintf(b, sizeof b, "%d of %d slots ready", rd, (int)sl.size());
        TextRight(FONT_BODY, px(20), lx + lw, by + px(2), b, Theme::TextDim.alpha(a));
    }
    RoundRect(lx, ly, lw, lh, px(12), Col(8, 7, 6, (int)(120 * a)));
    float rowH = px(62);
    int vis = (int)(lh / rowH); if (vis < 1) vis = 1;
    std::vector<Entry> ent;
    std::vector<Slot> slots;
    int n;
    if (S.prepMode) { slots = Slots(c.idx, S.lvl); n = (int)slots.size(); }
    else { ent = Castable(c, S.lvl); n = (int)ent.size(); }
    if (S.sel >= n) S.sel = n > 0 ? n - 1 : 0;
    if (S.sel < S.scroll) S.scroll = S.sel;
    if (S.sel >= S.scroll + vis) S.scroll = S.sel - vis + 1;
    if (S.scroll > n - vis) S.scroll = n - vis;
    if (S.scroll < 0) S.scroll = 0;
    PushClip(lx, ly, lw, lh);
    for (int i = S.scroll; i < n && i < S.scroll + vis + 1; i++) {
        float ry = ly + (i - S.scroll) * rowH;
        bool sel = i == S.sel;
        UIK::RowSel(lx + px(4), ry + px(3), lw - px(8), rowH - px(6), sel);
        float ix = lx + px(14), is = rowH - px(16);
        if (S.prepMode) {
            const Slot& s = slots[i];
            if (s.id >= 0) {
                SpellIcon(s.id, ix, ry + px(8), is, a, !s.ready);
                std::string nm = Spell(s.id).name;
                std::string mm = MetaName(s.meta);
                if (!mm.empty()) nm = mm + " " + nm;
                TextFit(FONT_BODY, px(23), ix + is + px(14), ry + px(9), lw - is - px(200), nm.c_str(), (sel ? Col(255, 244, 222) : Theme::Text).alpha(a));
                const char* st = s.domain ? "Domain" : s.ready ? "Ready" : "Rest to ready";
                Text(FONT_BODY, px(17), ix + is + px(14), ry + px(36), st, (s.ready ? Theme::Friendly : Theme::TextFaint).alpha(a));
            } else {
                RoundRectBorder(ix, ry + px(8), is, is, px(8), px(1), Theme::Line.alpha(0.3f * a));
                Text(FONT_BODY, px(22), ix + is + px(14), ry + px(17), "Empty slot", Theme::TextFaint.alpha(a));
            }
            char sn[16]; snprintf(sn, sizeof sn, "%d", i + 1);
            TextRight(FONT_BODY, px(18), lx + lw - px(22), ry + px(20), sn, Theme::TextFaint.alpha(a));
        } else {
            const Entry& e = ent[i];
            bool dim = e.ready <= 0;
            SpellIcon(e.id, ix, ry + px(8), is, a, dim);
            std::string nm = Spell(e.id).name;
            std::string mm = MetaName(e.meta);
            TextFit(FONT_BODY, px(23), ix + is + px(14), ry + (mm.empty() && !e.domain ? px(17) : px(9)), lw - is - px(170), nm.c_str(),
                    (dim ? Theme::TextFaint : sel ? Col(255, 244, 222) : Theme::Text).alpha(a));
            if (!mm.empty() || e.domain) Text(FONT_BODY, px(17), ix + is + px(14), ry + px(36), (e.domain ? (mm.empty() ? std::string("Domain") : mm + ", domain") : mm).c_str(), Theme::TextDim.alpha(a));
            char u[32];
            if (c.prepared) snprintf(u, sizeof u, "%d / %d", e.ready, e.total); else snprintf(u, sizeof u, "%d", e.ready);
            TextRight(FONT_BODY_BOLD, px(22), lx + lw - px(22), ry + px(17), u, (dim ? Theme::TextFaint : Theme::GoldDim).alpha(a));
        }
    }
    if (n == 0) {
        const char* msg = S.prepMode ? "No spell slots at this level" : c.prepared ? "Nothing prepared at this level" : "No spells known at this level";
        TextCentered(FONT_BODY, px(21), lx + lw * 0.5f, ly + px(30), msg, Theme::TextFaint.alpha(a));
        if (!S.prepMode && c.prepared && LevelCount(c, S.lvl) > 0)
            UIK::Hint(GL_BACK, "Prepare spells", lx + lw * 0.5f - UIK::HintWidth(GL_BACK, "Prepare spells", px(28)) * 0.5f, ly + px(80), px(28), a);
    }
    PopClip();
    UIK::Scrollbar(lx + lw - px(8), ly + px(6), lh - px(12), S.scroll, vis, n);
    // right: details
    float dx = lx + lw + px(30);
    Text(FONT_TITLE, px(22), dx, by, "Details", Theme::GoldDim.alpha(a));
    RoundRect(dx, ly, dw, lh, px(12), Col(8, 7, 6, (int)(120 * a)));
    int id = -1; std::string extra;
    if (S.prepMode && n > 0 && slots[S.sel].id >= 0) id = slots[S.sel].id;
    if (!S.prepMode && n > 0) {
        id = ent[S.sel].id;
        char b[64];
        if (c.prepared) snprintf(b, sizeof b, "%d of %d prepared ready", ent[S.sel].ready, ent[S.sel].total);
        else snprintf(b, sizeof b, "%d casts left at this level", ent[S.sel].ready);
        extra = b;
    }
    if (id >= 0) DrawDetails(id, dx + px(18), ly + px(18), dw - px(36), lh - px(18), a, extra);
    // actions
    float ay = ly + lh - px(46), ax = dx + px(20);
    if (S.prepMode) {
        if (n > 0) {
            ax += UIK::Hint(GL_A, "Choose spell", ax, ay, px(28), a) + px(20);
            if (slots[S.sel].id >= 0) ax += UIK::Hint(GL_X, "Clear", ax, ay, px(28), a) + px(20);
        }
    } else if (n > 0) {
        ax += UIK::Hint(GL_A, "Cast", ax, ay, px(28), a) + px(20);
        ax += UIK::Hint(GL_X, "Quickbar", ax, ay, px(28), a) + px(20);
        UIK::Hint(GL_RS, "Scroll", ax, ay, px(28), a);
    }
    if (S.ov != OV_NONE) DrawOverlay(x, y, w, h, a);
}

int Hints(HintItem* o) {
    int n = 0;
    if (S.ov != OV_NONE) {
        o[n++] = {GL_DPAD, "Choose"};
        o[n++] = {GL_A, S.ov == OV_QUICKBAR ? "Place" : "Select"};
        o[n++] = {GL_B, "Cancel"};
        return n;
    }
    o[n++] = {GL_LT, "Level"};
    o[n++] = {GL_A, S.prepMode ? "Choose" : "Cast"};
    const ClassInfo* c = Cur();
    if (c && c->prepared) o[n++] = {GL_BACK, S.prepMode ? "Spell list" : "Prepare"};
    if (g_cls.size() > 1) o[n++] = {GL_Y, "Class"};
    if (S.prepMode) o[n++] = {GL_B, "Back"};
    return n;
}
}  // namespace

const HubPage PAGE_SPELLS = {Open, Input, Draw, Hints, nullptr};
