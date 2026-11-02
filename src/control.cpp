// NWNPad - twin-stick gameplay controls
#include "control.h"
#include "navcursor.h"
#include "quickbar.h"
#include "inv.h"
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
#include "questmark.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <windows.h>
#include <cmath>
#include <vector>
#include <set>
#include <algorithm>

ControlState g_ctl;

using namespace G;

// ---------------------------------------------------------------------------------------------
// extra game entry points used here
namespace {
const uintptr_t VA_QuickButton_HandleLeftButton = 0x14085c8f0;
const uintptr_t VA_CGuiMan_HandleKeyEvent = 0x1401f5270;
const uintptr_t VA_CGuiMan_SetMousePosition = 0x1401f6870;
const uintptr_t VA_RButtonDown = 0x1406f10e0;
const uintptr_t VA_ActionTarget_LMouseDown = 0x14083be00;
const uintptr_t VA_WalkPlayerToPoint = 0x1406f3650;   // (internal, x, y, z, flags, target id, ...)

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
static double g_quickSlotTime = -10;
static std::string g_giveName;
static double g_giveStart = 0;
void Control_ClickQuickSlot(int bank, int slot) {
    void* b = Control_QuickButton(bank, slot);
    if (!b) return;
    Log("quickslot bank %d slot %d", bank, slot);
    QB_NoteUsed(bank, slot);
    typedef void (*fn)(void*);
    ((fn)A(VA_QuickButton_HandleLeftButton))(b);
    g_quickSlotTime = g_pad.now;      // a target mode that starts now goes to the current target
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
    float yaw = CameraYaw() * 0.01745329f;              // degrees
    fx = -sinf(yaw); fy = cosf(yaw);
}

static bool IsHostile(void* o) {
    if (Type(o) != OT_CREATURE) return false;
    uint8_t r = F<uint8_t>(o, OFS::Cre_Reaction);
    return r >= 2;
}
static bool CreatureDead(void* o) {
    void* st = F<void*>(o, OFS::Cre_Stats);
    if (!st) return false;
    if (F<uint8_t>(st, 0x8e) == 5) return true;          // damage level "dead" (all creatures)
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
        // corpses get in the way in a fight: their loot is in a bag, or reachable with the pause pointer
        if (t == OT_CREATURE && id != pid && !g_ctl.pauseCursor && CreatureDead(o)) continue;
        if (g_ctl.giveItem && (t != OT_CREATURE || id == pid || CreatureDead(o))) continue;     // giving: the living only
        Vec3 p = Pos(o);
        float dx = p.x - pp.x, dy = p.y - pp.y, dz = p.z - pp.z;
        float d = sqrtf(dx * dx + dy * dy);
        if (fabsf(dz) > 4.0f) continue;
        bool hostile = IsHostile(o);
        float range = (t == OT_CREATURE) ? g_cfg.targetRange : g_cfg.interactRange + 2.0f;
        if (g_ctl.pauseCursor) range = 45.0f;            // the pointer can reach anything on screen
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
// While playing with the pad nothing is hovered with the (hidden, parked) mouse: the soft target is,
// and in conversations / menus nothing - no name and health plate over the hero standing under the
// parked mouse. The pad cursor (generic panels) keeps the real mouse behaviour.
bool Control_HoverOverride(void** obj) {
    void* pl = Player();
    if (!g_cfg.enabled || !pl || !g_pad.connected) return false;
    if (g_pad.now - g_ctl.lastPadUse > 30.0 || Cursor_Active()) return false;
    void* o = g_padGameplay && g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr;
    *obj = o == pl ? nullptr : o;
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
// paused: a pointer over the world (left stick), like the mouse in the classic interface
static Vec3 g_curWorld;
static bool g_curWorldOk = false;

// ground point under a screen position: solve WorldToScreen(p) = (sx, sy) on the hero's height plane
static bool ScreenToGround(void* player, float tx, float ty, Vec3& out) {
    Vec3 p = Pos(player);
    for (int it = 0; it < 16; it++) {
        float sx, sy;
        if (!WorldToScreen(p, sx, sy)) return false;
        float ex = tx - sx, ey = ty - sy;
        if (ex * ex + ey * ey < 1e-7f) break;
        const float h = 0.5f;
        Vec3 px_ = p; px_.x += h; Vec3 py_ = p; py_.y += h;
        float ax, ay, bx, by;
        if (!WorldToScreen(px_, ax, ay) || !WorldToScreen(py_, bx, by)) return false;
        float j11 = (ax - sx) / h, j21 = (ay - sy) / h, j12 = (bx - sx) / h, j22 = (by - sy) / h;
        float det = j11 * j22 - j12 * j21;
        if (fabsf(det) < 1e-9f) return false;
        float dx = (j22 * ex - j12 * ey) / det, dy = (-j21 * ex + j11 * ey) / det;
        float l = Len2(dx, dy);
        if (l > 8.0f) { dx *= 8.0f / l; dy *= 8.0f / l; }
        p.x += dx; p.y += dy;
    }
    Vec3 pp = Pos(player);
    if (Len2(p.x - pp.x, p.y - pp.y) > 60.0f) return false;
    out = p;
    return true;
}

static bool g_curWas = false;
static void PauseCursor(void* player) {
    bool& was = g_curWas;
    if (!was) {   // start at the hero
        float sx, sy; Vec3 p = Pos(player); p.z += 1.0f;
        if (WorldToScreen(p, sx, sy)) { g_ctl.curX = sx; g_ctl.curY = sy; }
        else { g_ctl.curX = 0.5f; g_ctl.curY = 0.5f; }
    }
    was = true;
    float m = Len2(g_pad.lx, g_pad.ly);
    bool moved = false;
    if (m > 0.0f) {
        float sp = 0.75f * (0.25f + 0.75f * m) * (float)g_pad.dt;       // screen heights per second
        float aspect = GuiH() > 0 ? (float)GuiW() / GuiH() : 1.6f;
        g_ctl.curX += g_pad.lx / fmaxf(m, 1e-3f) * m * sp / aspect;
        g_ctl.curY -= g_pad.ly / fmaxf(m, 1e-3f) * m * sp;
        g_ctl.curX = fminf(fmaxf(g_ctl.curX, 0.01f), 0.99f);
        g_ctl.curY = fminf(fmaxf(g_ctl.curY, 0.01f), 0.99f);
        moved = true;
    }
    // object under the pointer (creatures by their chest, others by their base)
    static uint32_t hover = 0;
    if (moved || !hover) {
        float bestD = 1e9f; uint32_t best = 0;
        float W_ = (float)GuiW(), H_ = (float)GuiH();
        for (auto& c : g_cands) {
            if (c.id == Id(player)) continue;
            Vec3 p = Pos(c.obj); p.z += c.type == OT_CREATURE ? 1.0f : 0.4f;
            float sx, sy;
            if (!WorldToScreen(p, sx, sy)) continue;
            float dx = (sx - g_ctl.curX) * W_, dy = (sy - g_ctl.curY) * H_;
            float d = sqrtf(dx * dx + dy * dy);
            float r = (c.type == OT_CREATURE ? 0.075f : 0.05f) * H_;
            if (d < r && d < bestD) { bestD = d; best = c.id; }
        }
        hover = best;
        if (moved || best) { g_ctl.softTarget = best; g_ctl.locked = best != 0; g_lockTime = g_pad.now; }
    }
    g_curWorldOk = ScreenToGround(player, g_ctl.curX, g_ctl.curY, g_curWorld);
    g_ctl.curGround = g_curWorldOk && !g_ctl.softTarget;
}

static bool WalkToPointer() {
    void* in = Internal();
    if (!in || !g_curWorldOk) return false;
    Log("walk to (%.1f %.1f %.1f)", g_curWorld.x, g_curWorld.y, g_curWorld.z);
    typedef int (*fn)(void*, float, float, float, int, uint32_t, int);
    ((fn)A(VA_WalkPlayerToPoint))(in, g_curWorld.x, g_curWorld.y, g_curWorld.z, 0, 0x7f000000u, 0);
    return true;
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

// what an item power in target mode may be used on: spells.2da TargetType of the spell it casts
// (1 self, 2 creature, 4 ground, 8 item, 0x10 door, 0x20 placeable, 0x40 trigger); -1 not known (spells
// from the spellbook, other powers). The item property mode is the one with its handler at +0xf8.
static int ActionTargetMask(void* at) {
    if (!at || F<uintptr_t>(at, 0xf8) != A(0x1407039b0)) return -1;
    void* it = Inv::ItemById(F<uint32_t>(at, 0x150));
    int idx = F<uint8_t>(at, 0x154);                            // low byte: the property (byte 2: its sub-entry)
    int n = it ? F<int>(it, 0x1f4) : 0;
    if (idx < 0 || idx >= n) return -1;
    const uint8_t* p = F<const uint8_t*>(it, 0x1f8) + idx * 0x30;
    if (F<uint16_t>(p, 0) != 15) return -1;                      // cast spell
    int spell = TDA("iprp_spells").Int(F<uint16_t>(p, 2), "SpellIndex", -1);
    const std::string& t = TDA("spells").Get(spell, "TargetType");
    if (t.empty()) return -1;
    int mask = (int)strtol(t.c_str(), nullptr, 0);
    // a helpful spell that can go on a creature (invisibility, haste, cure ...) is meant for one, not for the
    // chest or door that happens to be the target
    if (TDA("spells").Int(spell, "HostileSetting", 0) == 0 && (mask & 3)) mask &= ~(0x10 | 0x20 | 0x40);
    return mask;
}
static bool TargetFits(int mask, void* obj) {
    if (mask < 0 || !obj) return true;
    switch (Type(obj)) {
    case 5: return (mask & 0x02) != 0;
    case 6: return (mask & 0x08) != 0;
    case 7: return (mask & 0x40) != 0;
    case 9: return (mask & 0x20) != 0;
    case 10: return (mask & 0x10) != 0;
    }
    return true;
}

static void ActionTargetControl(void* player, void* at) {
    typedef void (*fn)(void*);
    auto On = [&](void* obj, Vec3 p, int kind) { WithHitOverride(obj, p, kind, [&] { ((fn)A(VA_ActionTarget_LMouseDown))(at); }); };
    int mask = ActionTargetMask(at);
    // used from a quick slot with a target selected: straight at that target, or on yourself when the
    // power can't go on it (a ring of invisibility with a chest in front of you)
    if (g_pad.now - g_quickSlotTime < 0.6) {
        void* obj = g_ctl.softTarget && g_ctl.softTarget != Id(player) ? GetObj(g_ctl.softTarget) : nullptr;
        if (obj && TargetFits(mask, obj)) {
            g_quickSlotTime = -10;
            Log("quick slot on current target %08x", Id(obj));
            On(obj, Pos(obj), 2);
            return;
        }
        if (mask >= 0 && (mask & 1)) {
            g_quickSlotTime = -10;
            Log("quick slot on yourself (targets %#x)", mask);
            On(player, Pos(player), 2);
            return;
        }
    }
    // choose target: soft target, or ground point ahead of the hero
    if (g_pad.Pressed(PB_A)) {
        void* obj = g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr;
        if (!TargetFits(mask, obj)) obj = nullptr;
        if (!obj && mask >= 0 && !(mask & 4) && (mask & 1)) { Log("action target on yourself (targets %#x)", mask); On(player, Pos(player), 2); return; }
        Vec3 p = obj ? Pos(obj) : (g_ctl.pauseCursor && g_curWorldOk) ? g_curWorld : GroundPointAhead(player, 4.0f);
        Log("action target confirm obj=%08x", obj ? Id(obj) : 0);
        On(obj, p, obj ? 2 : 1);
    } else if (g_pad.Pressed(PB_B)) {
        GuiKey(0x12, 1);
    } else if (g_pad.Pressed(PB_Y)) {
        On(player, Pos(player), 2);                               // target self
    }
}

// ---- giving items ------------------------------------------------------------------------------
void Control_StartGive(uint32_t item, const char* name) {
    g_ctl.giveItem = item; g_giveName = name ? name : ""; g_giveStart = g_pad.now;
    Log("give %08x: choose who", item);
}
const char* Control_GiveName() { return g_giveName.c_str(); }
// as dragging the item onto a creature in the world: potions and healer's kits are used on your own
// associates (they drink it), anything else opens a barter with the creature
static void GiveTo(void* player, void* target, uint32_t itemId) {
    void* it = Inv::ItemById(itemId);
    if (!it || !target) return;
    int base = F<int>(it, 0x40);
    uint32_t tid = Id(target);
    bool mine = F<int>(target, 0x274) != 0 && F<uint32_t>(target, 0x270) == Id(player);
    if (mine && (base == 49 || base == 104)) {
        float pos[3] = {0, 0, 0};
        Log("give: %08x uses %08x", tid, itemId);
        typedef void (*fn)(void*, uint32_t, uint32_t, uint32_t, float*);
        ((fn)A(0x140805740))(player, itemId, 0u | (0xffu << 16), tid, pos);
        return;
    }
    void* msg = Inv::Message();
    if (!msg) return;
    Log("give: barter with %08x offering %08x", tid, itemId);
    typedef void (*fb)(void*, uint32_t, uint32_t);
    ((fb)A(0x1407ba3f0))(msg, tid, itemId);
}
static void GiveControl(void* player) {
    if (!Inv::ItemById(g_ctl.giveItem)) { g_ctl.giveItem = 0; return; }
    if (g_pad.now - g_giveStart < 0.25) return;           // the press that chose "Give"
    if (g_pad.Pressed(PB_B)) { Log("give cancelled"); g_ctl.giveItem = 0; return; }
    if (g_pad.Pressed(PB_A)) {
        void* t = g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr;
        if (!t || Type(t) != OT_CREATURE || t == player || CreatureDead(t)) return;
        GiveTo(player, t, g_ctl.giveItem);
        g_ctl.giveItem = 0;
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
    if (Control_UIWantsInput()) { g_moving = false; g_aArmed = false; return; }
    if (!player) { g_moving = false; return; }
    g_padGameplay = g_pad.now - g_ctl.lastPadUse < 30.0;

    // --- aim / move direction from left stick (camera relative)
    float fx, fy; CamForward(player, fx, fy);
    float rxv = fy, ryv = -fx;              // camera right vector
    g_moveMag = Len2(g_pad.lx, g_pad.ly);
    // paused, or a spell / item waiting for its target: the left stick moves a pointer over the world
    bool paused = (GD::IsPaused() || g_ctl.inActionTargetMode || g_ctl.giveItem) && !g_ctl.radialOpen;
    if (paused && !g_ctl.pauseCursor) g_ctl.pauseCursor = true;
    if (!paused && g_ctl.pauseCursor && !g_ctl.radialOpen) { g_ctl.pauseCursor = false; g_ctl.locked = false; g_curWas = false; }
    if (g_ctl.pauseCursor) g_moveMag = 0.0f;
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
        // the game adds 75 (turn/tilt) and 7.5 (zoom) per frame while a camera key is held, so the keys turn
        // faster the higher the frame rate; the stick does the same (full stick = key speed x setting), with a
        // curve for fine control near the centre
        const float k = 1.0f;
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
    if (g_ctl.pauseCursor) PauseCursor(player);
    else UpdateSoftTarget(player);
    if (g_pad.Pressed(PB_RB)) CycleTarget(player, +1);
    // LB: tap = previous target, hold + right stick = camera tilt
    {
        static bool lbTilt = false;
        if (g_pad.Pressed(PB_LB)) lbTilt = false;
        if (g_pad.Down(PB_LB) && fabsf(g_pad.ry) > 0.3f) lbTilt = true;
        if (g_pad.Released(PB_LB) && !lbTilt && !g_pad.Down(PB_RB)) CycleTarget(player, -1);
    }
    if (g_pad.Pressed(PB_RS)) { g_ctl.locked = false; }
    if (g_ctl.pauseCursor && (g_pad.Pressed(PB_RB) || g_pad.Released(PB_LB)) && g_ctl.softTarget) {
        if (void* o = GetObj(g_ctl.softTarget)) {   // the pointer follows the cycled target
            Vec3 p = Pos(o); p.z += Type(o) == OT_CREATURE ? 1.0f : 0.4f;
            float sx, sy;
            if (WorldToScreen(p, sx, sy)) { g_ctl.curX = sx; g_ctl.curY = sy; }
        }
    }
    ApplyHilite();

    // --- giving an item: A gives it to the creature under the pointer, B cancels
    if (g_ctl.giveItem) { g_aArmed = false; GiveControl(player); return; }

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
        else if (g_ctl.pauseCursor) WalkToPointer();
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
