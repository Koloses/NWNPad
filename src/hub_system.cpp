// NWNPad - hub "System" page: save / load lists, rest, options and exit (driving the game's options panel)
#include "hub_pages.h"
#include "hub.h"
#include "render.h"
#include "game.h"
#include "gamedata.h"
#include "res.h"
#include "text.h"
#include "pad.h"
#include "log.h"
#include <windows.h>
#include <shlobj.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using namespace G;
using namespace R;

namespace {
const uintptr_t VA_GuiInGame_HandleOptionsButton = 0x1406f5d30;   // opens / closes COptionsPanel (GIG+0xd8)
const uintptr_t VA_App_SavingAllowed = 0x1406a9aa0;              // 0 = multiplayer client (no save / load)
const uintptr_t VA_SaveGameList_Ctor = 0x140813550;              // CSaveGameList::CSaveGameList (reads SAVES:, sorted)
const uintptr_t VA_Internal_SendSaveGame = 0x1406dc910;          // (internal, number, name, module, overwrite dir)
const uintptr_t VA_Internal_SendLoadGame = 0x1406dc8f0;          // (internal, number, name, file, 0)

// COptionsPanel button callbacks (Win): {object, function} pairs filled in PostAttachmentInitialize
const size_t OP_SaveChar = 0x418, OP_Video = 0x500, OP_Controls = 0x6d0, OP_Exit = 0x7b8, OP_Game = 0x988;

enum Act { A_RESUME, A_QUICKSAVE, A_SAVE, A_LOAD, A_REST, A_SAVECHAR, A_GAME, A_VIDEO, A_CONTROLS, A_EXIT };
struct Entry { Act act; const char* label; const char* desc; };
const Entry ENTRIES[] = {
    {A_RESUME, "Resume", "Back to the game."},
    {A_QUICKSAVE, "Quick save", "Save to the quick save slot right away."},
    {A_SAVE, "Save game", "Save your progress to a new slot or over an older save."},
    {A_LOAD, "Load game", "Load a saved game. Progress since the last save is lost."},
    {A_REST, "Rest", "Rest to recover hit points and spells. Not possible with enemies nearby."},
    {A_SAVECHAR, "Save character", "Export your character so it can be used in other modules."},
    {A_GAME, "Game options", "Difficulty, feedback, sound, interface and gameplay settings."},
    {A_VIDEO, "Video", "Resolution, display mode, graphics quality and UI scaling."},
    {A_CONTROLS, "Keyboard", "Keyboard bindings (the gamepad layout is fixed by NWNPad)."},
    {A_EXIT, "Exit to main menu", "Leave the current game. You will be asked to confirm."},
};
const int N = sizeof(ENTRIES) / sizeof(ENTRIES[0]);

int g_sel = 0;
bool g_waiting = false;          // a game panel opened from here is up
double g_actTime = 0;

// ---- save list ---------------------------------------------------------------------------------
struct Save { std::string dir, name, nameRaw, file; int number = 0; int y = 0, mo = 0, d = 0, h = 0, mi = 0; uint32_t thumb = 0; bool thumbTried = false; };
std::vector<Save> g_saves;
enum Mode { M_MAIN, M_SAVE, M_LOAD } g_mode = M_MAIN;
int g_lsel = 0, g_lscroll = 0;
int g_confirm = -1;
std::string g_saveRoot;

struct ExoS { char* p; uint32_t len, cap; };
ExoS MkS(const std::string& s) { ExoS e; e.p = (char*)s.c_str(); e.len = (uint32_t)s.size(); e.cap = e.len + 1; return e; }

void ReadSaves() {
    g_saves.clear();
    char* lst = (char*)calloc(1, 0x40);       // leaked on purpose: the list's destructor is not exported
    if (!lst) return;
    typedef void* (*ctor_t)(void*);
    ((ctor_t)A(VA_SaveGameList_Ctor))(lst);
    char* names = F<char*>(lst, 0); int nn = F<int>(lst, 8);
    char* infos = F<char*>(lst, 0x10); int ni = F<int>(lst, 0x18);
    int* order = F<int*>(lst, 0x20); int no = F<int>(lst, 0x28);
    for (int i = 0; i < no && order; i++) {
        int k = order[i];
        if (k < 0 || k >= nn || k >= ni) continue;
        Save s;
        char* ns = names + (size_t)k * 0x10;
        const char* raw = F<char*>(ns, 0);
        std::string dirRaw = raw ? std::string(raw, F<uint32_t>(ns, 8)) : std::string();
        size_t p = dirRaw.find(" - ");
        if (p == std::string::npos) continue;
        s.number = atoi(dirRaw.substr(0, p).c_str());
        s.nameRaw = dirRaw.substr(p + 3);
        s.name = G2UClean(s.nameRaw.c_str(), (int)s.nameRaw.size());
        s.dir = dirRaw;
        char* fi = infos + (size_t)k * 0x20;
        const char* fr = F<char*>(fi, 0);
        s.file = fr ? std::string(fr, F<uint32_t>(fi, 8)) : std::string();
        if (s.file.size() >= 4 && _stricmp(s.file.c_str() + s.file.size() - 4, ".sav") == 0) s.file.resize(s.file.size() - 4);
        s.y = F<uint16_t>(fi, 0x10); s.mo = F<uint16_t>(fi, 0x12); s.d = F<uint16_t>(fi, 0x14);
        s.h = F<uint16_t>(fi, 0x16); s.mi = F<uint16_t>(fi, 0x18);
        g_saves.push_back(s);
    }
    std::stable_sort(g_saves.begin(), g_saves.end(), [](const Save& a, const Save& b) {
        long long ka = ((((long long)a.y * 13 + a.mo) * 32 + a.d) * 24 + a.h) * 60 + a.mi;
        long long kb = ((((long long)b.y * 13 + b.mo) * 32 + b.d) * 24 + b.h) * 60 + b.mi;
        return ka > kb;
    });
    Log("system: %d saves", (int)g_saves.size());
    if (g_saveRoot.empty()) {
        char docs[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) g_saveRoot = std::string(docs) + "\\Neverwinter Nights\\saves\\";
    }
}

uint32_t Thumb(Save& s) {
    if (!s.thumbTried && !g_saveRoot.empty()) {
        s.thumbTried = true;
        s.thumb = Res_TextureFile((g_saveRoot + s.dir + "\\screen.tga").c_str());
    }
    return s.thumb;
}

std::string SafeName(const std::string& n) {
    std::string r;
    for (char c : n) if (!strchr("\\/:*?\"<>|", c) && (unsigned char)c >= 32) r += c;
    while (!r.empty() && (r.back() == ' ' || r.back() == '.')) r.pop_back();
    return r.empty() ? std::string("Save") : r;
}

std::string AreaRaw() {
    void* pl = Player(); void* ar = pl ? F<void*>(pl, OFS::Obj_Area) : nullptr;
    if (!ar || !F<char*>(ar, 0x238)) return std::string();
    return std::string(F<char*>(ar, 0x238), F<uint32_t>(ar, 0x240));
}
std::string AreaName() { std::string r = AreaRaw(); return G2UClean(r.c_str(), (int)r.size()); }

std::string ModuleName() {
    void* m = Module();
    std::string s;
    if (m) { const char* p = F<char*>(m, 0x18); if (p) s.assign(p, F<uint32_t>(m, 0x20)); }
    return s.empty() ? std::string("GAME") : s;
}

void SaveTo(int idx) {          // idx -1 = new slot
    void* in = Internal();
    if (!in) return;
    std::string name, over;
    int number;
    if (idx < 0) {
        int mx = 1;
        for (auto& s : g_saves) mx = std::max(mx, s.number);
        number = mx + 1;
        SYSTEMTIME t; GetLocalTime(&t);
        char ts[32]; snprintf(ts, sizeof ts, " %02d-%02d %02d.%02d", t.wDay, t.wMonth, t.wHour, t.wMinute);
        name = SafeName(AreaRaw());
        if (name.size() > 28) name.resize(28);
        name += ts;
    } else {
        number = g_saves[idx].number;
        name = g_saves[idx].nameRaw;
        over = g_saves[idx].dir;
    }
    std::string mod = ModuleName();
    ExoS a = MkS(name), b = MkS(mod), c = MkS(over);
    Log("system: save #%d '%s' over '%s'", number, name.c_str(), over.c_str());
    typedef int (*fn)(void*, uint32_t, ExoS*, ExoS*, ExoS*);
    ((fn)A(VA_Internal_SendSaveGame))(in, (uint32_t)number, &a, &b, &c);
}

void LoadFrom(int idx) {
    void* in = Internal();
    if (!in || idx < 0 || idx >= (int)g_saves.size()) return;
    const Save& s = g_saves[idx];
    ExoS a = MkS(s.nameRaw), b = MkS(s.file);
    Log("system: load #%d '%s' file '%s'", s.number, s.nameRaw.c_str(), s.file.c_str());
    typedef int (*fn)(void*, uint32_t, ExoS*, ExoS*, int);
    ((fn)A(VA_Internal_SendLoadGame))(in, (uint32_t)s.number, &a, &b, 0);
}

// ---- options panel -----------------------------------------------------------------------------
void* GIG() { void* in = Internal(); return in ? F<void*>(in, OFS::In_GuiInGame) : nullptr; }
void* OptionsPanel() { void* g = GIG(); return g ? F<void*>(g, 0xd8) : nullptr; }
bool SavingAllowed() {
    if (!App()) return false;
    typedef int (*fn)(void*);
    return ((fn)A(VA_App_SavingAllowed))(App()) != 0;
}
bool Enabled(Act a) {
    if (a == A_SAVE || a == A_LOAD || a == A_QUICKSAVE) return SavingAllowed();
    return true;
}

void* GuiMan() {
    static void** p = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?g_pGuiMan@@3PEAVCGuiMan@@EA");
    return p ? *p : nullptr;
}
void* TopModal() {
    void* gm = GuiMan();
    if (!gm) return nullptr;
    int n = F<int>(gm, 0x90);
    void** arr = F<void**>(gm, 0x88);
    return (n > 0 && arr) ? arr[n - 1] : nullptr;
}

bool CallButton(size_t fnOff) {
    void* g = GIG();
    if (!g) return false;
    void* p = OptionsPanel();
    if (!p) {
        typedef void (*fn)(void*);
        ((fn)A(VA_GuiInGame_HandleOptionsButton))(g);
        p = OptionsPanel();
    }
    if (!p || !Readable((char*)p + fnOff - 8, 16)) return false;
    void* obj = F<void*>(p, fnOff - 8);
    void* f = F<void*>(p, fnOff);
    if (!f) return false;
    typedef void (*cb)(void*);
    ((cb)f)(obj ? obj : p);
    return true;
}

void Do(Act a) {
    if (!Enabled(a)) { Hub_Toast("Not available in a multiplayer game"); return; }
    size_t off = 0;
    switch (a) {
    case A_RESUME: Hub_Close(); return;
    case A_QUICKSAVE: Hub_Close(); Hub_FireEvent(0x47); return;
    case A_SAVE: ReadSaves(); g_mode = M_SAVE; g_lsel = 0; g_lscroll = 0; g_confirm = -1; return;
    case A_LOAD: ReadSaves(); g_mode = M_LOAD; g_lsel = 0; g_lscroll = 0; g_confirm = -1; return;
    case A_REST: Hub_Close(); Hub_FireEvent(0x6b); return;
    case A_SAVECHAR: off = OP_SaveChar; break;
    case A_GAME: off = OP_Game; break;
    case A_VIDEO: off = OP_Video; break;
    case A_CONTROLS: off = OP_Controls; break;
    case A_EXIT: off = OP_Exit; break;
    }
    Log("system: %s", ENTRIES[g_sel].label);
    Hub_Close();
    if (CallButton(off)) { g_waiting = true; g_actTime = g_pad.now; }
    if (a == A_SAVECHAR) { g_waiting = false; Hub_Toast("Character saved"); }
}

void Open() { g_mode = M_MAIN; g_confirm = -1; }

void ListInput() {
    int extra = g_mode == M_SAVE ? 1 : 0;        // "new save" row
    int n = (int)g_saves.size() + extra;
    if (g_pad.Pressed(PB_B)) { if (g_confirm >= 0) g_confirm = -1; else g_mode = M_MAIN; return; }
    int old = g_lsel;
    if (n > 0) {
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) g_lsel = (g_lsel + n - 1) % n;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) g_lsel = (g_lsel + 1) % n;
        if (g_pad.Repeat(PB_LT)) g_lsel = std::max(0, g_lsel - 6);
        if (g_pad.Repeat(PB_RT)) g_lsel = std::min(n - 1, g_lsel + 6);
    }
    if (old != g_lsel) g_confirm = -1;
    if (!g_pad.Pressed(PB_A) || n == 0) return;
    int idx = g_lsel - extra;
    if (g_mode == M_SAVE) {
        if (idx < 0) { SaveTo(-1); Hub_Close(); return; }
        if (g_confirm == g_lsel) { SaveTo(idx); Hub_Close(); return; }
        g_confirm = g_lsel;
    } else {
        if (g_confirm == g_lsel) { Hub_Close(); LoadFrom(idx); return; }
        g_confirm = g_lsel;
    }
}

void Input() {
    if (g_mode != M_MAIN) { ListInput(); return; }
    if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) g_sel = (g_sel + N - 1) % N;
    if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) g_sel = (g_sel + 1) % N;
    if (g_pad.Pressed(PB_A)) Do(ENTRIES[g_sel].act);
    if (g_pad.Pressed(PB_B)) Hub_Close();
}

void DrawList(float x, float y, float w, float h, float a) {
    float lx = x + px(36), ly = y + px(18);
    float lw = w * 0.52f;
    Text(FONT_TITLE, px(24), lx, ly, g_mode == M_SAVE ? "Save game" : "Load game", Theme::Gold.alpha(a));
    char cnt[48]; snprintf(cnt, sizeof cnt, "%d saves", (int)g_saves.size());
    TextRight(FONT_BODY, px(20), lx + lw, ly + px(4), cnt, Theme::TextFaint.alpha(a));
    ly += px(44);
    float lh = y + h - px(20) - ly;
    RoundRect(lx, ly, lw, lh, px(12), Col(8, 7, 6, (int)(120 * a)));
    int extra = g_mode == M_SAVE ? 1 : 0;
    int n = (int)g_saves.size() + extra;
    float rowH = px(64);
    int vis = std::max(1, (int)(lh / rowH));
    if (g_lsel < g_lscroll) g_lscroll = g_lsel;
    if (g_lsel >= g_lscroll + vis) g_lscroll = g_lsel - vis + 1;
    PushClip(lx, ly, lw, lh);
    for (int i = g_lscroll; i < n && i < g_lscroll + vis + 1; i++) {
        float ry = ly + (i - g_lscroll) * rowH;
        bool sel = i == g_lsel;
        UIK::RowSel(lx + px(4), ry + px(3), lw - px(8), rowH - px(6), sel);
        if (i < extra) {
            float cx = lx + px(34), cy = ry + rowH * 0.5f;
            Circle(cx, cy, px(13), Theme::GoldDim.alpha(a));
            RoundRect(cx - px(7), cy - px(1.5f), px(14), px(3), px(1), Col(20, 14, 6, (int)(255 * a)));
            RoundRect(cx - px(1.5f), cy - px(7), px(3), px(14), px(1), Col(20, 14, 6, (int)(255 * a)));
            Text(FONT_BODY, px(23), lx + px(62), ry + px(18), "New save", (sel ? Col(255, 244, 222) : Theme::Text).alpha(a));
            continue;
        }
        const Save& s = g_saves[i - extra];
        bool special = s.number <= 1;
        TextFit(FONT_BODY, px(23), lx + px(22), ry + px(8), lw - px(240), s.name.c_str(), (sel ? Col(255, 244, 222) : special ? Theme::TextDim : Theme::Text).alpha(a));
        char d[64]; snprintf(d, sizeof d, "%02d.%02d.%04d  %02d:%02d", s.d, s.mo, s.y, s.h, s.mi);
        Text(FONT_BODY, px(17), lx + px(22), ry + px(37), d, Theme::TextFaint.alpha(a));
        if (g_confirm == i) {
            const char* q = g_mode == M_SAVE ? "A again to overwrite" : "A again to load";
            float pulse = 0.6f + 0.4f * sinf((float)g_pad.now * 6.0f);
            TextRight(FONT_BODY_BOLD, px(19), lx + lw - px(20), ry + px(21), q, Theme::Gold.alpha(pulse * a));
        }
    }
    if (n == 0) TextCentered(FONT_BODY, px(21), lx + lw * 0.5f, ly + px(30), "No saved games", Theme::TextFaint.alpha(a));
    PopClip();
    UIK::Scrollbar(lx + lw - px(8), ly + px(6), lh - px(12), g_lscroll, vis, n);
    // preview
    float rx = lx + lw + px(40), rw = x + w - px(40) - rx, ry = y + px(62);
    int idx = g_lsel - extra;
    if (idx >= 0 && idx < (int)g_saves.size()) {
        Save& s = g_saves[idx];
        uint32_t t = Thumb(s);
        float tw = rw, th = rw * 0.75f;
        if (th > h * 0.55f) { th = h * 0.55f; tw = th / 0.75f; }
        RoundRect(rx, ry, tw, th, px(12), Col(0, 0, 0, (int)(150 * a)));
        if (t) ImageRounded(t, rx, ry, tw, th, px(12), Col(255, 255, 255, (int)(255 * a)));
        else TextCentered(FONT_BODY, px(20), rx + tw * 0.5f, ry + th * 0.5f - px(12), "No preview", Theme::TextFaint.alpha(a));
        RoundRectBorder(rx, ry, tw, th, px(12), px(1), Theme::Line.alpha(0.5f * a));
        ry += th + px(20);
        ry += TextWrap(FONT_TITLE, px(26), rx, ry, rw, s.name.c_str(), Theme::Gold.alpha(a)) + px(8);
        char d[64]; snprintf(d, sizeof d, "Saved %02d.%02d.%04d at %02d:%02d", s.d, s.mo, s.y, s.h, s.mi);
        Text(FONT_BODY, px(20), rx, ry, d, Theme::TextDim.alpha(a));
        ry += px(40);
        const char* note = g_mode == M_LOAD ? "Progress since your last save will be lost." : "This save will be replaced.";
        Text(FONT_BODY, px(19), rx, ry, note, Theme::TextFaint.alpha(a));
    } else if (idx < 0) {
        Text(FONT_TITLE, px(26), rx, ry, "New save", Theme::Gold.alpha(a));
        ry += px(48);
        std::string nm = "Named after the current area: " + AreaName();
        TextWrap(FONT_BODY, px(20), rx, ry, rw, nm.c_str(), Theme::TextDim.alpha(a), 0, 1.3f);
    }
}

void Draw(float x, float y, float w, float h, float a) {
    if (g_mode != M_MAIN) { DrawList(x, y, w, h, a); return; }
    float lw = fminf(px(560), w * 0.4f);
    float lx = x + px(60), ly = y + px(26);
    float rowH = fminf(px(60), (h - px(50)) / N);
    for (int i = 0; i < N; i++) {
        const Entry& e = ENTRIES[i];
        float ry = ly + i * rowH;
        bool sel = i == g_sel, en = Enabled(e.act);
        UIK::RowSel(lx, ry + px(3), lw, rowH - px(6), sel);
        Col c = !en ? Theme::TextFaint : sel ? Col(255, 244, 222) : Theme::Text;
        float fs = px(25);
        Text(FONT_TITLE, fs, lx + px(26), ry + (rowH - LineHeight(FONT_TITLE, fs)) * 0.5f, e.label, c.alpha(a));
        if (i == 1 || i == 4 || i == 8) RoundRect(lx + px(16), ry + rowH - px(1), lw - px(32), px(1), 0, Theme::Line.alpha(0.25f * a));
    }
    // right: where you are + what the entry does
    float rx = lx + lw + px(70), rw = x + w - px(60) - rx;
    float ry = ly + px(10);
    void* pl = Player();
    if (pl) {
        uint32_t tex = GD::PortraitTex(pl, 'm');
        if (tex) ImageRounded(tex, rx, ry, px(96), px(150), px(10), Col(255, 255, 255, (int)(255 * a)), 0, 0, 1, 0.78f);
        float tx = rx + px(120);
        Text(FONT_TITLE, px(30), tx, ry + px(6), GD::ObjName(Id(pl)).c_str(), Theme::Gold.alpha(a));
        char lv[48]; snprintf(lv, sizeof lv, "Level %d", GD::Level(pl));
        Text(FONT_BODY, px(21), tx, ry + px(52), lv, Theme::TextDim.alpha(a));
        Text(FONT_BODY, px(21), tx, ry + px(84), AreaName().c_str(), Theme::TextDim.alpha(a));
        if (GD::IsPaused()) Text(FONT_BODY_BOLD, px(19), tx, ry + px(118), "Paused", Theme::GoldDim.alpha(a));
    }
    ry += px(200);
    RoundRect(rx, ry, rw, px(1), 0, Theme::Line.alpha(0.4f * a));
    ry += px(24);
    const Entry& e = ENTRIES[g_sel];
    Text(FONT_TITLE, px(28), rx, ry, e.label, Theme::Gold.alpha(a));
    ry += px(48);
    TextWrap(FONT_BODY, px(21), rx, ry, rw, Enabled(e.act) ? e.desc : "Not available in a multiplayer game.", Theme::Text.alpha(0.9f * a), 0, 1.25f);
}

int Hints(HintItem* o) {
    int n = 0;
    o[n++] = {GL_DPAD, "Choose"};
    if (g_mode == M_MAIN) { o[n++] = {GL_A, "Select"}; return n; }
    o[n++] = {GL_A, g_mode == M_SAVE ? "Save" : "Load"};
    o[n++] = {GL_B, "Back"};
    return n;
}
}  // namespace

const HubPage PAGE_SYSTEM = {Open, Input, Draw, Hints, nullptr};

// the game's options panel stays (hidden) behind the options / exit screens; when that
// screen closes and the panel would show again, close it and come back to this page
void HubSystem_Update() {
    if (!g_waiting) return;
    if (g_pad.now - g_actTime < 0.3) return;
    void* p = OptionsPanel();
    if (!p) {
        g_waiting = false;
        if (Player()) Hub_ShowTab("System");
        return;
    }
    bool visible = (F<uint32_t>(p, 0xb0) & 2) == 0;
    if (visible && TopModal() == p) {
        g_waiting = false;
        Log("system: options panel back, closing it");
        typedef void (*fn)(void*);
        ((fn)A(VA_GuiInGame_HandleOptionsButton))(GIG());   // toggles it closed
        if (Player()) Hub_ShowTab("System");
    }
}
