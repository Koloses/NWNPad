// NWNPad - twin-stick gameplay controls
#include "control.h"
#include "hub_pages.h"
#include "twoda.h"
#include "guiscan.h"
#include "game.h"
#include "hooks.h"
#include "pad.h"
#include "config.h"
#include "log.h"
#include "hub.h"
#include "gamedata.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <windows.h>
#include <cmath>
#include <vector>
#include <algorithm>

ControlState g_ctl;

using namespace G;

// ---------------------------------------------------------------------------------------------
// extra game entry points used here
namespace {
const uintptr_t VA_QuickButton_HandleLeftButton = 0x14085b350;
const uintptr_t VA_CGuiMan_HandleKeyEvent = 0x1401f3630;
const uintptr_t VA_CGuiMan_SetMousePosition = 0x1401f4c30;
const uintptr_t VA_RButtonDown = 0x1406ee750;
const uintptr_t VA_ActionTarget_LMouseDown = 0x14083a860;

void** p_g_pGuiMan = nullptr;
int* p_radialDepth = nullptr;
float (*p_GetGUIScale)() = nullptr;

void* GuiMan() { return p_g_pGuiMan ? *p_g_pGuiMan : nullptr; }
int GuiKey(int key, int down) {
    void* gm = GuiMan();
    if (!gm) return 0;
    typedef int (*fn)(void*, unsigned, unsigned);
    return ((fn)A(VA_CGuiMan_HandleKeyEvent))(gm, key, down);
}
void SetMouse(int x, int y) {
    void* gm = GuiMan();
    if (!gm) return;
    typedef void (*fn)(void*, int, int);
    ((fn)A(VA_CGuiMan_SetMousePosition))(gm, x, y);
}
int GuiW() { void* gm = GuiMan(); return gm ? F<int>(gm, 0xb8) : 1280; }
int GuiH() { void* gm = GuiMan(); return gm ? F<int>(gm, 0xbc) : 720; }

// object captured by the gui manager (radial menu / action target)
void* Captured() { void* gm = GuiMan(); return gm ? F<void*>(gm, 0x58) : nullptr; }
uint32_t GuiObjType(void* o) { return VCall<uint32_t (*)(void*)>(o, 0x20)(o); }
const uint32_t TYPE_RADIAL = 0x524d6e75, TYPE_ACTIONTARGET = 0x41635467;

float Len2(float x, float y) { return sqrtf(x * x + y * y); }
}

// ---------------------------------------------------------------------------------------------
struct Cand {
    void* obj; uint32_t id; uint8_t type;
    float dist, dot; bool hostile; float score; float sx, sy;
};

static std::vector<Cand> g_cands;
static uint32_t g_hilited = 0;
static float g_moveX = 0, g_moveY = 0, g_moveMag = 0;    // desired world move dir
static bool g_moving = false, g_movingRun = false;
static double g_lastDriveSend = 0;
static float g_aimX = 1, g_aimY = 0;
static bool g_aHeldForRadial = false;
static bool g_aArmed = false;      // A went down in plain gameplay (not in a menu / dialogue / radial / targeting)
static double g_lockTime = 0;

void Control_Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    p_g_pGuiMan = (void**)GetProcAddress(exe, "?g_pGuiMan@@3PEAVCGuiMan@@EA");
    p_radialDepth = (int*)GetProcAddress(exe, "?static_nMenuDepth@CRadialMenu@@1HA");
    p_GetGUIScale = (float (*)())GetProcAddress(exe, "?GetGUIScale@CAurora@@SAMXZ");
    Log("guiman %p radialDepth %p guiscale %p", p_g_pGuiMan, p_radialDepth, (void*)p_GetGUIScale);
}

void* Control_QuickButton(int bank, int slot) {
    void* in = Internal(); if (!in) return nullptr;
    void* gig = F<void*>(in, OFS::In_GuiInGame); if (!gig) return nullptr;
    void* qb = F<void*>(gig, 0x60); if (!qb) return nullptr;
    return (char*)qb + 0xf0 + (size_t)(bank * 12 + slot) * 0x2c8;
}
void Control_ClickQuickSlot(int bank, int slot) {
    void* b = Control_QuickButton(bank, slot);
    if (!b) return;
    Log("quickslot bank %d slot %d", bank, slot);
    typedef void (*fn)(void*);
    ((fn)A(VA_QuickButton_HandleLeftButton))(b);
}

// camera forward direction on the ground plane
static void CamForward(void* player, float& fx, float& fy) {
    void* cam = Camera();
    fx = 0; fy = 1;
    if (!cam || !player) return;
    Vec3 cp = F<Vec3>(cam, 0xb0);
    Vec3 pp = Pos(player);
    float dx = pp.x - cp.x, dy = pp.y - cp.y;
    float l = Len2(dx, dy);
    if (l > 0.05f) { fx = dx / l; fy = dy / l; return; }
    float yaw = CameraYaw();
    fx = cosf(yaw); fy = sinf(yaw);
}

static bool IsHostile(void* o) {
    if (Type(o) != OT_CREATURE) return false;
    uint8_t r = F<uint8_t>(o, OFS::Cre_Reaction);
    return r >= 2;
}
static bool CreatureDead(void* o) {
    void* st = F<void*>(o, OFS::Cre_Stats);
    if (!st) return false;
    short mx = F<short>(st, OFS::Stats_MaxHP), cur = F<short>(st, OFS::Stats_CurHP);
    return mx > 0 && cur <= 0;
}

static void GatherCandidates(void* player, bool includeSelf) {
    g_cands.clear();
    void* area = F<void*>(player, OFS::Obj_Area);
    if (!area) return;
    int n = F<int>(area, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(area, OFS::Area_ObjIds);
    if (!ids || n <= 0 || n > 100000) return;
    Vec3 pp = Pos(player);
    uint32_t pid = Id(player);
    for (int i = 0; i < n; i++) {
        void* o = GetObj(ids[i]);
        if (!o) continue;
        uint8_t t = Type(o);
        if (t != OT_CREATURE && t != OT_ITEM && t != OT_PLACEABLE && t != OT_DOOR) continue;
        if (!F<uint8_t>(o, OFS::Obj_Visible)) continue;
        if (!F<int>(o, OFS::Obj_Selectable)) continue;
        uint32_t id = Id(o);
        if (id == pid && !includeSelf) continue;
        Vec3 p = Pos(o);
        float dx = p.x - pp.x, dy = p.y - pp.y, dz = p.z - pp.z;
        float d = sqrtf(dx * dx + dy * dy);
        if (fabsf(dz) > 4.0f) continue;
        bool hostile = IsHostile(o);
        float range = (t == OT_CREATURE) ? g_cfg.targetRange : g_cfg.interactRange + 2.0f;
        if (id != pid && d > range) continue;
        Cand c{};
        c.obj = o; c.id = id; c.type = t; c.dist = d; c.hostile = hostile;
        c.dot = d > 0.01f ? (dx * g_aimX + dy * g_aimY) / d : 1.0f;
        g_cands.push_back(c);
    }
}

static Cand* FindCand(uint32_t id) {
    for (auto& c : g_cands) if (c.id == id) return &c;
    return nullptr;
}

static void UpdateSoftTarget(void* player) {
    // scoring: prefer what is in front of the aim direction, then close
    Cand* best = nullptr;
    for (auto& c : g_cands) {
        if (c.id == Id(player)) { c.score = 1e9f; continue; }
        float angPen = 1.0f + 2.5f * (1.0f - c.dot);       // 1 (ahead) .. 6 (behind)
        float s = c.dist * angPen;
        if (c.type == OT_CREATURE && c.hostile) s *= 0.75f;
        if (c.type == OT_CREATURE && CreatureDead(c.obj)) s *= 1.6f;
        if (c.type != OT_CREATURE && c.dist > g_cfg.interactRange) s *= 3.0f;
        if (c.dot < -0.2f && c.dist > 2.5f) s *= 4.0f;
        c.score = s;
        if (!best || s < best->score) best = &c;
    }
    if (g_ctl.locked) {
        Cand* cur = FindCand(g_ctl.softTarget);
        if (cur && cur->dist < g_cfg.targetRange * 1.5f) return;
        g_ctl.locked = false;
    }
    Cand* cur = FindCand(g_ctl.softTarget);
    if (cur && best && cur != best && best->score > cur->score * 0.7f) best = cur;   // hysteresis
    g_ctl.softTarget = best ? best->id : 0;
}

static void CycleTarget(void* player, int dir) {
    // order candidates by screen-space angle around the player, relative to the camera
    float fx, fy; CamForward(player, fx, fy);
    Vec3 pp = Pos(player);
    struct E { uint32_t id; float a; };
    std::vector<E> v;
    for (auto& c : g_cands) {
        if (c.id == Id(player)) continue;
        Vec3 p = Pos(c.obj);
        float dx = p.x - pp.x, dy = p.y - pp.y;
        float fwd = dx * fx + dy * fy, right = dx * fy - dy * fx;
        v.push_back({c.id, atan2f(right, fwd)});
    }
    if (v.empty()) return;
    std::sort(v.begin(), v.end(), [](const E& a, const E& b) { return a.a < b.a; });
    int idx = -1;
    for (size_t i = 0; i < v.size(); i++) if (v[i].id == g_ctl.softTarget) idx = (int)i;
    if (idx < 0) idx = dir > 0 ? 0 : (int)v.size() - 1;
    else idx = (idx + dir + (int)v.size()) % (int)v.size();
    g_ctl.softTarget = v[idx].id;
    g_ctl.locked = true;
    g_lockTime = g_pad.now;
}

static bool g_padGameplay = false;
bool Control_HoverOverride(void** obj) {
    if (!g_cfg.enabled || !g_padGameplay || !Player()) return false;
    *obj = g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr;
    return true;
}

static void ApplyHilite() {
    return;   // hover feedback is produced by the game itself (see Control_HoverOverride)
    uint32_t want = g_ctl.padActive ? g_ctl.softTarget : 0;
    if (want == g_hilited) {
        if (want) { void* o = GetObj(want); if (o) VCall<void (*)(void*, int)>(o, OFS::V_MouseOver)(o, 1); }
        return;
    }
    if (g_hilited) {
        void* o = GetObj(g_hilited);
        if (o) VCall<void (*)(void*)>(o, OFS::V_MouseOff)(o);
    }
    g_hilited = want;
    if (want) {
        void* o = GetObj(want);
        if (o) VCall<void (*)(void*, int)>(o, OFS::V_MouseOver)(o, 1);
    }
}

// run a game action with the mouse hit-test redirected to `obj` (or ground point)
template <class Fn> static void WithHitOverride(void* obj, const Vec3& p, int code, Fn fn) {
    g_hitOverride.active = true; g_hitOverride.obj = obj; g_hitOverride.code = code;
    g_hitOverride.x = p.x; g_hitOverride.y = p.y; g_hitOverride.z = p.z;
    fn();
    g_hitOverride.active = false;
}

static void PlaceMouseAt(void* obj) {
    int w = GuiW(), h = GuiH();
    int mx = w / 2, my = h / 2;
    if (obj) {
        Vec3 p = Pos(obj); p.z += 1.0f;
        float sx, sy;
        if (WorldToScreen(p, sx, sy) && sx > 0.05f && sx < 0.95f && sy > 0.05f && sy < 0.95f) {
            mx = (int)(sx * w); my = (int)((1.0f - sy) * h);     // GUI coords are y-up
        }
    }
    SetMouse(mx, my);
}

static void OpenRadialOn(void* obj) {
    void* in = Internal();
    if (!in || !obj) return;
    PlaceMouseAt(obj);
    Log("radial on %08x", Id(obj));
    WithHitOverride(obj, Pos(obj), 2, [&] {
        typedef void (*fn)(void*);
        ((fn)A(VA_RButtonDown))(in);
    });
}

static Vec3 GroundPointAhead(void* player, float dist) {
    Vec3 p = Pos(player);
    p.x += g_aimX * dist; p.y += g_aimY * dist;
    return p;
}

// ---------------------------------------------------------------------------------------------
static int OctantKey(int o) {
    static const int k[8] = {0x36, 0x37, 0x34, 0x31, 0x30, 0x2f, 0x32, 0x35};
    return k[o & 7];
}

static void RadialControl(void* radial) {
    float x = g_pad.lx, y = g_pad.ly;
    if (Len2(g_pad.rx, g_pad.ry) > Len2(x, y)) { x = g_pad.rx; y = g_pad.ry; }
    float m = Len2(x, y);
    int cx = F<int>(radial, 0x7dc), cy = F<int>(radial, 0x7e0);
    float scale = p_GetGUIScale ? p_GetGUIScale() : 1.0f;
    static int lastOct = -1;
    if (m > 0.5f) {
        float r = 100.0f * scale;
        SetMouse(cx + (int)(x / m * r), cy + (int)(y / m * r));
        float a = atan2f(y, x);
        float o = (1.5707963f - a) / 6.2831853f * 8.0f;
        if (o < 0) o += 8.0f;
        lastOct = ((int)(o + 0.5f)) & 7;
    } else {
        SetMouse(cx, cy);
        lastOct = -1;
    }
    if (g_pad.Pressed(PB_A) && !g_aHeldForRadial) {
        if (lastOct >= 0) { Log("radial select octant %d", lastOct); GuiKey(OctantKey(lastOct), 1); }
    }
    if (g_pad.Released(PB_A) && g_aHeldForRadial) {
        // the radial was opened by holding A: releasing A over an entry selects it
        g_aHeldForRadial = false;
        if (lastOct >= 0) { Log("radial select octant %d (release)", lastOct); GuiKey(OctantKey(lastOct), 1); }
    }
    if (g_pad.Pressed(PB_B)) GuiKey(0x12, 1);
    if (g_pad.Pressed(PB_Y)) GuiKey(0x33, 1);     // centre = back to parent menu
}

static void ActionTargetControl(void* player, void* at) {
    // choose target: soft target, or ground point ahead of the hero
    if (g_pad.Pressed(PB_A)) {
        void* obj = g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr;
        Vec3 p = obj ? Pos(obj) : GroundPointAhead(player, 4.0f);
        Log("action target confirm obj=%08x", obj ? Id(obj) : 0);
        WithHitOverride(obj, p, obj ? 2 : 1, [&] {
            typedef void (*fn)(void*);
            ((fn)A(VA_ActionTarget_LMouseDown))(at);
        });
    } else if (g_pad.Pressed(PB_B)) {
        GuiKey(0x12, 1);
    } else if (g_pad.Pressed(PB_Y)) {
        // target self
        WithHitOverride(player, Pos(player), 2, [&] {
            typedef void (*fn)(void*);
            ((fn)A(VA_ActionTarget_LMouseDown))(at);
        });
    }
}

static void QuickSlots() {
    int bank = g_ctl.quickBank;
    bool lt = g_pad.Down(PB_LT), rt = g_pad.Down(PB_RT);
    struct M { uint32_t btn; int slot; };
    if (lt && rt) {
        if (g_pad.Pressed(PB_A)) Control_ClickQuickSlot(bank, 11);
        return;
    }
    if (lt) {
        const M m[] = {{PB_X, 3}, {PB_Y, 4}, {PB_B, 5}, {PB_A, 6}};
        for (auto& e : m) if (g_pad.Pressed(e.btn)) Control_ClickQuickSlot(bank, e.slot);
        return;
    }
    if (rt) {
        const M m[] = {{PB_X, 7}, {PB_Y, 8}, {PB_B, 9}, {PB_A, 10}};
        for (auto& e : m) if (g_pad.Pressed(e.btn)) Control_ClickQuickSlot(bank, e.slot);
        return;
    }
    const M m[] = {{PB_X, 0}, {PB_Y, 1}, {PB_B, 2}};
    for (auto& e : m) if (g_pad.Pressed(e.btn)) Control_ClickQuickSlot(bank, e.slot);
}

// developer: "<InjectFile>.cmd" holding "target <name part>" locks the soft target (automated tests)
static void DevCommands(void* player) {
    if (!g_cfg.injectFile[0]) return;
    static double next = 0;
    if (g_pad.now < next) return;
    next = g_pad.now + 0.25;
    char path[300]; snprintf(path, sizeof path, "%s.cmd", g_cfg.injectFile);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[200] = {};
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    DeleteFileA(path);
    for (char* p = line; *p; p++) if (*p == '\r' || *p == '\n') *p = 0;
    if (strncmp(line, "probe", 5) == 0 && player) {
        Vec3 p = Pos(player); float sx, sy;
        WorldToScreen(p, sx, sy);
        Log("dev: probe player (%.1f %.1f %.1f) -> sx %.3f sy %.3f", p.x, p.y, p.z, sx, sy);
        if (void* t = g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr) {
            Vec3 q = Pos(t); WorldToScreen(q, sx, sy);
            Log("dev: probe target (%.1f %.1f %.1f) -> sx %.3f sy %.3f", q.x, q.y, q.z, sx, sy);
        }
        return;
    }
    if (strncmp(line, "ev ", 3) == 0) {       // fire a game input event (hex)
        int ev = (int)strtol(line + 3, nullptr, 16);
        Log("dev: event %#x", ev);
        HandleInputEvent(ev, 1); HandleInputEvent(ev, 0);
        return;
    }
    if (strncmp(line, "panel", 5) == 0) {     // dump the top modal panel's controls
        static void** gm = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?g_pGuiMan@@3PEAVCGuiMan@@EA");
        void* m = gm ? *gm : nullptr;
        int n = m ? F<int>(m, 0x90) : 0;
        void** arr = m ? F<void**>(m, 0x88) : nullptr;
        Log("dev: %d modal panels, captured %p", n, m ? F<void*>(m, 0x58) : nullptr);
        for (void* p : Gui_LivePanels()) Gui_Dump(p);
        // other panels the GUI manager knows (list at +0x30 / count +0x38 if present)
        for (size_t off = 0x10; m && off < 0x100; off += 0x10) {
            void** a = F<void**>(m, off); int c = F<int>(m, off + 8);
            if (!a || c <= 0 || c > 64 || !Readable(a, c * 8)) continue;
            std::string names;
            for (int i = 0; i < c; i++) { names += ClassName(a[i]); names += " "; }
            Log("dev: gm+%#x [%d] %s", (int)off, c, names.c_str());
        }
        return;
    }
    if (strncmp(line, "gig", 3) == 0) {       // CGuiInGame members that are objects
        void* in = Internal(); void* gig = in ? F<void*>(in, OFS::In_GuiInGame) : nullptr;
        for (int off = 8; gig && off <= 0x400; off += 8) {
            void* p = F<void*>(gig, off);
            if (!p || ((uintptr_t)p & 7) || !Readable(p, 0xc0)) continue;
            const char* cn = ClassName(p);
            if (cn[0]) Log("dev: GIG+%#x %s flags %x", off, cn, F<uint32_t>(p, 0xb0));
        }
        return;
    }
    if (strncmp(line, "levelup", 7) == 0) {   // ask the server to start a level-up
        typedef void (*fn)();
        ((fn)A(0x1406f5c20))();
        Log("dev: level up requested");
        return;
    }
    if (strncmp(line, "click ", 6) == 0) {    // click a control of the top modal panel by layout name
        static void** gm = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?g_pGuiMan@@3PEAVCGuiMan@@EA");
        void* m = gm ? *gm : nullptr;
        int n = m ? F<int>(m, 0x90) : 0;
        void** arr = m ? F<void**>(m, 0x88) : nullptr;
        const auto& lp = Gui_LivePanels();
        for (int i = (int)lp.size() - 1; i >= 0; i--) { void* c = Gui_Find(lp[i], line + 6); if (c) { Gui_Click(c); break; } }
        (void)arr; (void)n;
        return;
    }
    if (strncmp(line, "tda ", 4) == 0) {      // log one 2DA row: tda <resref> <row>
        char name[32] = {}; int row = 0;
        if (sscanf(line + 4, "%31s %d", name, &row) == 2) {
            const TwoDA& t = TDA(name);
            std::string out;
            if (row < 0) { for (auto& c : t.Cols()) out += c + " "; Log("dev: %s cols = %s", name, out.c_str()); return; }
            for (int c = 0; c < 64; c++) { const std::string& v = t.Get(row, c); out += (v.empty() ? std::string("*") : v) + " "; }
            Log("dev: %s[%d] = %s", name, row, out.c_str());
        }
        return;
    }
    if (strncmp(line, "journal", 7) == 0) {
        void* in = Internal(); void* j = in ? F<void*>(in, 0x70) : nullptr;
        Log("dev: journal %p done %d active %d", j, j ? F<int>(j, 8) : -1, j ? F<int>(j, 0x30) : -1);
        return;
    }
    if (!player) return;
    if (strncmp(line, "use ", 4) == 0) {      // default action on the first object matching the name
        std::string want = line + 4;
        for (auto& c : want) c = (char)tolower((unsigned char)c);
        void* area = F<void*>(player, OFS::Obj_Area);
        int n = area ? F<int>(area, OFS::Area_ObjCount) : 0;
        uint32_t* ids = area ? F<uint32_t*>(area, OFS::Area_ObjIds) : nullptr;
        for (int i = 0; ids && i < n; i++) {
            std::string l = GD::ObjName(ids[i]); for (auto& c : l) c = (char)tolower((unsigned char)c);
            void* o = GetObj(ids[i]);
            if (o && l.find(want) != std::string::npos) { Log("dev: use %08x", ids[i]); DefaultAction(o); return; }
        }
        return;
    }
    if (strncmp(line, "target ", 7) == 0) {
        std::string want = line + 7;
        for (auto& c : want) c = (char)tolower((unsigned char)c);
        void* area = F<void*>(player, OFS::Obj_Area);
        int n = area ? F<int>(area, OFS::Area_ObjCount) : 0;
        uint32_t* ids = area ? F<uint32_t*>(area, OFS::Area_ObjIds) : nullptr;
        for (int i = 0; ids && i < n; i++) {
            std::string nm = GD::ObjName(ids[i]);
            std::string l = nm; for (auto& c : l) c = (char)tolower((unsigned char)c);
            if (l.find(want) != std::string::npos) {
                g_ctl.softTarget = ids[i]; g_ctl.locked = true; g_lockTime = g_pad.now;
                Log("dev: target %08x '%s'", ids[i], nm.c_str());
                return;
            }
        }
        Log("dev: no target matching '%s'", want.c_str());
    }
}

// ---------------------------------------------------------------------------------------------
void Control_PreInput(void* in) {
    Pad_Update();
    if (!g_cfg.enabled) return;
    if (g_pad.held || g_pad.lx || g_pad.ly || g_pad.rx || g_pad.ry) { g_ctl.padActive = true; g_ctl.lastPadUse = g_pad.now; }
    if (!g_pad.connected) { g_moving = false; Control_UIWantsInput(); return; }   // keeps HUD / panels in sync

    void* player = Player();
    void* cap = Captured();
    uint32_t capType = cap ? GuiObjType(cap) : 0;
    g_ctl.radialOpen = capType == TYPE_RADIAL;
    g_ctl.inActionTargetMode = capType == TYPE_ACTIONTARGET;

    g_padGameplay = false;
    DevCommands(player);
    if (Control_UIWantsInput()) { g_moving = false; g_aArmed = false; return; }
    if (!player) { g_moving = false; return; }
    g_padGameplay = g_pad.now - g_ctl.lastPadUse < 30.0;

    // --- aim / move direction from left stick (camera relative)
    float fx, fy; CamForward(player, fx, fy);
    float rxv = fy, ryv = -fx;              // camera right vector
    g_moveMag = Len2(g_pad.lx, g_pad.ly);
    // after a radial menu or targeting the left stick is still pushed from choosing the entry:
    // ignore it until it returns to the centre, otherwise walking cancels the chosen action
    {
        static bool wasMenu = false, lock = false;
        bool menu = g_ctl.radialOpen || g_ctl.inActionTargetMode;
        if (menu) lock = true;
        else if (wasMenu || lock) lock = g_moveMag > 0.15f;
        wasMenu = menu;
        if (lock && !menu) g_moveMag = 0.0f;
    }
    if (g_moveMag > 0.0f && !g_ctl.radialOpen) {
        float wx = fx * g_pad.ly + rxv * g_pad.lx;
        float wy = fy * g_pad.ly + ryv * g_pad.lx;
        float l = Len2(wx, wy);
        g_moveX = wx / l; g_moveY = wy / l;
        g_aimX = g_moveX; g_aimY = g_moveY;
        g_moving = true;
    } else {
        g_moving = false;
        Vec3 o = F<Vec3>(player, OFS::Obj_Orient);
        float l = Len2(o.x, o.y);
        if (l > 0.01f) { g_aimX = o.x / l; g_aimY = o.y / l; }
    }

    // --- radial menu open: sticks pick the entry
    if (g_ctl.radialOpen) { g_aArmed = false; RadialControl(cap); return; }

    // --- camera (right stick)
    if (!g_ctl.radialOpen) {
        float rx = g_cfg.invertCamX ? -g_pad.rx : g_pad.rx;
        float ry = g_cfg.invertCamY ? -g_pad.ry : g_pad.ry;
        // the game adds 75 (turn/tilt) and 7.5 (zoom) per frame while a camera key is held;
        // full stick = key speed x setting, with a curve for fine control near the centre
        float k = (float)g_pad.dt * 60.0f;
        auto curve = [](float v) { return v * (0.35f + 0.65f * fabsf(v)); };
        if (g_pad.Down(PB_LB)) {
            F<float>(in, OFS::In_CamPitch) += curve(ry) * 75.0f * g_cfg.camPitchSpeed * k;
        } else {
            F<float>(in, OFS::In_CamZoom) += -curve(ry) * 7.5f * g_cfg.camZoomSpeed * k;
        }
        F<float>(in, OFS::In_CamYaw) += -curve(rx) * 75.0f * g_cfg.camYawSpeed * k;
    }

    // --- rumble when the hero gets hurt
    {
        static uint32_t lastId = 0; static int lastHp = -1;
        GD::HP hp = GD::CreatureHP(player);
        if (Id(player) != lastId) { lastId = Id(player); lastHp = -1; }
        if (hp.known) {
            if (lastHp >= 0 && hp.cur < lastHp && hp.max > 0) {
                float f = (float)(lastHp - hp.cur) / (float)hp.max;
                float k = f * 4.0f; if (k > 1.0f) k = 1.0f;
                Pad_Rumble(0.25f + 0.55f * k, 0.15f + 0.6f * k, 0.12 + 0.2 * k);
            }
            lastHp = hp.cur;
        }
    }
    // --- targeting
    {
        static uint32_t lastArea = 0;
        void* ar = F<void*>(player, OFS::Obj_Area);
        uint32_t aid = ar ? F<uint32_t>(ar, OFS::Area_Id) : 0;
        if (aid != lastArea) { lastArea = aid; Log("area changed %08x", aid); }
    }
    GatherCandidates(player, g_ctl.inActionTargetMode);
    UpdateSoftTarget(player);
    if (g_pad.Pressed(PB_RB)) CycleTarget(player, +1);
    // LB: tap = previous target, hold + right stick = camera tilt
    {
        static bool lbTilt = false;
        if (g_pad.Pressed(PB_LB)) lbTilt = false;
        if (g_pad.Down(PB_LB) && fabsf(g_pad.ry) > 0.3f) lbTilt = true;
        if (g_pad.Released(PB_LB) && !lbTilt && !g_pad.Down(PB_RB)) CycleTarget(player, -1);
    }
    if (g_pad.Pressed(PB_RS)) { g_ctl.locked = false; }
    ApplyHilite();

    // --- action target mode (spell / item / feat waiting for a target)
    if (g_ctl.inActionTargetMode) { g_aArmed = false; ActionTargetControl(player, cap); return; }

    // --- interact / attack / context radial
    if (g_pad.Pressed(PB_A)) g_aArmed = true;
    if (!g_aArmed) g_aHeldForRadial = g_pad.Down(PB_A);      // a press that began elsewhere does nothing here
    if (g_pad.Down(PB_A) && !g_pad.Down(PB_LT) && !g_pad.Down(PB_RT)) {
        if (g_pad.Held(PB_A) > 0.45 && !g_aHeldForRadial) {
            g_aHeldForRadial = true;
            void* t = g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr;
            if (t) OpenRadialOn(t);
            else { SetMouse(GuiW() / 2, GuiH() / 2); HandleInputEvent(0x3e, 1); HandleInputEvent(0x3e, 0); }
        }
    }
    if (g_pad.Released(PB_A) && g_aArmed && !g_aHeldForRadial && !g_pad.Down(PB_LT) && !g_pad.Down(PB_RT)) {
        void* t = g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr;
        if (t) { Log("default action on %08x type %d", Id(t), Type(t)); DefaultAction(t); }
    }
    if (g_pad.Released(PB_A)) { g_aHeldForRadial = false; g_aArmed = false; }

    QuickSlots();

    // --- d-pad
    if (g_pad.Pressed(PB_DUP)) g_ctl.quickBank = (g_ctl.quickBank + 2) % 3;
    if (g_pad.Pressed(PB_DDOWN)) g_ctl.quickBank = (g_ctl.quickBank + 1) % 3;
    if (g_pad.Pressed(PB_DLEFT)) { SetMouse(GuiW() / 2, GuiH() / 2); HandleInputEvent(0x3e, 1); HandleInputEvent(0x3e, 0); }
    if (g_pad.Pressed(PB_DRIGHT)) { HandleInputEvent(0x60, 1); HandleInputEvent(0x60, 0); }   // pause
    if (g_pad.Pressed(PB_BACK)) Hub_ShowTab("Map");     // hub map page
    if (g_pad.Pressed(PB_START)) Hub_Toggle();                                                // hub menu
}

void Control_PostInput(void*) {}
void Control_SetMouse(int x, int y) { SetMouse(x, y); }
void Control_GuiSize(int& w, int& h) { w = GuiW(); h = GuiH(); }
int Control_GuiKey(int key, int down) { return GuiKey(key, down); }

// ---------------------------------------------------------------------------------------------
// movement: drives the game's own keyboard drive mode with an analog direction
void Control_Drive(void* in, void (*orig)(void*)) {
    void* player = Player();
    static bool wasMoving = false;
    if (!g_cfg.enabled || !player) { wasMoving = false; orig(in); return; }
    bool moving = g_moving && g_pad.connected;
    if (moving) {
        bool run = g_moveMag >= g_cfg.walkThreshold;
        if (!wasMoving) {
            HandleInputEvent(run ? 0x5a : 0x59, 1);   // starts drive timers like a key press
            g_movingRun = run;
        }
        Vec3 cur = F<Vec3>(player, OFS::Obj_Orient);
        float dot = cur.x * g_moveX + cur.y * g_moveY;
        bool turn = dot < 0.9985f;                       // ~3 degrees
        if (turn || run != g_movingRun) {
            Vec3 d = {g_moveX, g_moveY, 0};
            SetOrientation(player, d);
            if (g_pad.now - g_lastDriveSend > 0.05 || run != g_movingRun) {
                F<int>(in, OFS::In_DriveDirty) = 1;
                g_lastDriveSend = g_pad.now;
            }
        }
        F<int>(in, OFS::In_DriveFwd) = run ? 0 : 1;
        F<int>(in, OFS::In_DriveFwdRun) = run ? 1 : 0;
        g_movingRun = run;
        void* opts = F<void*>(in, 0);
        int saved = 0;
        if (opts && !run) { saved = F<int>(opts, 4); F<int>(opts, 4) = 0; }
        orig(in);
        if (opts && !run) F<int>(opts, 4) = saved;
        wasMoving = true;
        return;
    }
    if (wasMoving) {
        wasMoving = false;
        F<int>(in, OFS::In_DriveFwd) = 0;
        F<int>(in, OFS::In_DriveFwdRun) = 0;
        HandleInputEvent(0x5a, 0);      // clears and sends AbortDriveControl
    }
    orig(in);
}
