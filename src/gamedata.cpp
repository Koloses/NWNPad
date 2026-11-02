#include "gamedata.h"
#include "game.h"
#include "text.h"
#include "res.h"
#include "log.h"
#include "twoda.h"
#include "pad.h"
#include <map>
#include <windows.h>
#include <cstring>
#include <cmath>

using namespace G;

namespace {
typedef int (*GetObjectName_t)(void* app, uint32_t id, void* exoStr);
typedef void (*ExoDtor_t)(void*);
GetObjectName_t fGetObjectName = nullptr;
ExoDtor_t fExoDtor = nullptr;
typedef int (*GetLevel_t)(void* stats);
GetLevel_t fGetLevel = nullptr;
typedef int (*LocGet_t)(const void* loc, int lang, void* exoStr, uint8_t gender, bool);
LocGet_t fLocGet = nullptr;
}

namespace GD {
void Init() {
    fGetObjectName = (GetObjectName_t)A(0x1406abfc0);
    fExoDtor = (ExoDtor_t)GetProcAddress(GetModuleHandleA(nullptr), "??1CExoString@@QEAA@XZ");
    fGetLevel = (GetLevel_t)A(0x1407f5d70);
    fLocGet = (LocGet_t)GetProcAddress(GetModuleHandleA(nullptr), "?GetString@CExoLocString@@QEBAHHPEAVCExoString@@E_N@Z");
}

int Language() { void* in = Internal(); return in ? F<int>(in, 0x200) : 0; }

std::string LocStr(const void* loc) {
    if (!loc || !fLocGet) return std::string();
    struct { char* p; uint32_t len, cap; } s = {nullptr, 0, 0};
    fLocGet(loc, Language(), &s, 0, true);
    std::string r = s.p ? G2UClean(s.p, (int)s.len) : std::string();
    if (fExoDtor) fExoDtor(&s);
    return r;
}

std::string ObjName(uint32_t id) {
    void* app = App();
    if (!app || !fGetObjectName) return std::string();
    struct { char* p; uint32_t len, cap; } s = {nullptr, 0, 0};
    fGetObjectName(app, id, &s);
    std::string r = s.p ? G2UClean(s.p, (int)s.len) : std::string();
    if (fExoDtor) fExoDtor(&s);
    return r;
}

std::string PortraitBase(void* cre) {
    if (!cre || Type(cre) != OT_CREATURE) return std::string();
    void* st = F<void*>(cre, OFS::Cre_Stats);
    if (!st) return std::string();
    char ref[17] = {};
    memcpy(ref, (char*)st + 0xb4, 16);
    return std::string(ref);
}

uint32_t PortraitTex(void* cre, char size) {
    std::string b = PortraitBase(cre);
    if (b.empty()) return 0;
    std::string r = b + size;
    uint32_t t = Res_Texture(r.c_str());
    if (!t && size != 'm') t = Res_Texture((b + 'm').c_str());
    if (!t) t = Res_Texture((b + 's').c_str());
    return t;
}

HP CreatureHP(void* cre) {
    HP h;
    if (!cre || Type(cre) != OT_CREATURE) return h;
    void* st = F<void*>(cre, OFS::Cre_Stats);
    if (!st) return h;
    h.cur = F<short>(st, OFS::Stats_CurHP);
    h.temp = F<short>(st, OFS::Stats_TempHP);
    h.max = F<short>(st, OFS::Stats_MaxHP);
    h.known = h.max > 0;
    void* pl = Player();
    uint32_t pid = pl ? Id(pl) : 0;
    h.exact = cre == pl || (pid && MasterId(cre) == pid);
    h.level = F<uint8_t>(st, 0x8e);
    if (h.level > 5) h.level = 0;
    if (h.exact && h.known) {
        h.frac = (float)h.cur / h.max;
        if (h.frac < 0) h.frac = 0;
        if (h.frac > 1) h.frac = 1;
    } else {
        static const float F_[6] = {1.0f, 0.85f, 0.62f, 0.37f, 0.15f, 0.0f};
        h.frac = F_[h.level];
        h.known = true;
    }
    return h;
}

const char* DamageLevelName(int level) {
    static const char* N[6] = {"Uninjured", "Barely injured", "Injured", "Heavily wounded", "Near death", "Dead"};
    return level >= 0 && level < 6 ? N[level] : "";
}

bool IsDead(void* cre) {
    HP h = CreatureHP(cre);
    if (h.exact) return h.known && h.cur <= 0;
    return h.level == 5;
}

int Reaction(void* cre) {
    if (!cre || Type(cre) != OT_CREATURE) return 0;
    uint8_t r = F<uint8_t>(cre, OFS::Cre_Reaction);
    return r == 1 ? 1 : r == 0 ? 0 : 2;
}

uint32_t MasterId(void* cre) { return cre ? F<uint32_t>(cre, 0x270) : 0x7f000000; }

std::vector<void*> PartyMembers(void* player) {
    std::vector<void*> v;
    if (!player) return v;
    void* area = F<void*>(player, OFS::Obj_Area);
    if (!area) return v;
    int n = F<int>(area, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(area, OFS::Area_ObjIds);
    if (!ids || n <= 0 || n > 100000) return v;
    uint32_t pid = Id(player);
    for (int i = 0; i < n; i++) {
        void* o = GetObj(ids[i]);
        if (!o || Type(o) != OT_CREATURE || Id(o) == pid) continue;
        if (MasterId(o) == pid) v.push_back(o);
    }
    return v;
}

int Level(void* cre) {
    if (!cre || Type(cre) != OT_CREATURE || !fGetLevel) return 0;
    void* st = F<void*>(cre, OFS::Cre_Stats);
    return st ? fGetLevel(st) : 0;
}

bool LevelUpReady() {      // CGuiInGame+0x2c: level-up mode (set by the server when enough XP)
    void* in = Internal();
    void* gig = in ? F<void*>(in, OFS::In_GuiInGame) : nullptr;
    return gig && F<int>(gig, 0x2c) != 0;
}

bool IsPaused() {
    typedef int (*fn)(void*, uint8_t);
    void* app = App();
    if (!app) return false;
    return ((fn)A(0x1406ac030))(app, 2) != 0;
}

// Single player: the server's icon list (CNWSEffectListHandler::OnApplyEffectIcon) - CEffectIconObject
// (0xc): icon id u16 +0, flag +8 = applied by something other than worn equipment. The time left comes
// from the icon effects themselves (CNWSObject effect list +0x150 / +0x158, CGameEffect type +8, duration
// type flags +10 & 7, expiry day / time +0x10 / +0x14, integer 0 = icon id) against the world timer.
std::vector<FxIcon> EffectList(void* cre) {
    std::vector<FxIcon> v;
    if (!cre || G::Loading()) return v;
    struct C { std::vector<FxIcon> v; double t = -10; };
    static std::map<uint32_t, C> cache;
    C& c = cache[Id(cre)];
    if (g_pad.now - c.t < 0.25) {
        v = c.v;
        for (auto& f : v) if (f.left >= 0) f.left = fmaxf(0.0f, f.left - (float)(g_pad.now - c.t));
        return v;
    }
    c.t = g_pad.now;
    static HMODULE exe = GetModuleHandleA(nullptr);
    static auto fCre = (void* (*)(void*, uint32_t))GetProcAddress(exe, "?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z");
    static auto fTimer = (void* (*)(void*, uint32_t))GetProcAddress(exe, "?GetActiveTimer@CServerExoApp@@QEAAPEAVCWorldTimer@@I@Z");
    static auto fNow = (void (*)(void*, uint32_t*, uint32_t*))GetProcAddress(exe, "?GetWorldTime@CWorldTimer@@QEAAXPEAI0@Z");
    static auto fDiff = (uint64_t (*)(void*, uint32_t, uint32_t, uint32_t, uint32_t))GetProcAddress(exe, "?GetTimeDifference@CWorldTimer@@QEAA_KIIII@Z");
    static auto fInt = (int (*)(void*, int))GetProcAddress(exe, "?GetInteger@CGameEffect@@QEAAHH@Z");
    static void** am = (void**)GetProcAddress(exe, "?g_pAppManager@@3PEAVCAppManager@@EA");
    const uint16_t TypeIcon = (uint16_t)*(int*)G::A(0x140de7a3c);          // CGameEffect type of EffectIcon()
    const int DurEquipped = *(int*)G::A(0x140de7834);                      // CGameEffect::DURATION_TYPE_EQUIPPED
    const int DurInnate = 4;                                               // CGameEffect::DURATION_TYPE_INNATE
    const uint16_t TypeHaste = 1, TypeSlow = 3, TypeHasteInt = 0x29, TypeSlowInt = 0x2a;   // OnApplyHasteOrSlow
    void* srv = am && *am ? F<void*>(*am, 8) : nullptr;
    uint32_t sid = Id(cre) & 0x7fffffffu;
    void* sc = srv && F<void*>(srv, 8) && fCre ? fCre(srv, sid) : nullptr;
    if (sc) {
        void** e = F<void**>(sc, 0x778); int n = F<int>(sc, 0x780);
        for (int i = 0; e && i < n && i < 64; i++) if (e[i] && Readable(e[i], 0xc)) {
            FxIcon f; f.icon = F<uint16_t>(e[i], 0); f.equipment = F<int>(e[i], 8) == 0;
            v.push_back(f);
        }
        // time left: the longest of the icon's own (not equipment) effects; none if one of them doesn't expire
        void* timer = fTimer ? fTimer(srv, sid) : nullptr;
        uint32_t d = 0, t = 0;
        if (timer && fNow && fDiff && fInt) fNow(timer, &d, &t);
        void** fx = F<void**>(sc, 0x150); int nf = F<int>(sc, 0x158);
        auto ok = [&](int i) { return fx[i] && Readable(fx[i], 0x20); };
        auto type = [&](int i) { return F<uint16_t>(fx[i], 8); };
        auto dur = [&](int i) { return F<uint16_t>(fx[i], 10) & 7; };
        // one effect's contribution: >= 0 seconds left, -1 doesn't run out
        auto left = [&](int i) -> float {
            uint32_t ed = F<uint32_t>(fx[i], 0x10), et = F<uint32_t>(fx[i], 0x14);
            if (!ed && !et) return -1;
            return (float)fDiff(timer, d, t, ed, et) / 1000.0f;
        };
        for (auto& f : v) {
            if (f.equipment || !d || !fx) continue;
            float best = -2;                                                 // -2: nothing seen yet
            bool perm = false, onlyWorn = true;
            for (int i = 0; i < nf && i < 512 && !perm; i++) {
                if (!ok(i) || type(i) != TypeIcon || fInt(fx[i], 0) != f.icon || dur(i) == DurEquipped) continue;
                if (dur(i) == DurInnate && !F<uint32_t>(fx[i], 0x10) && !F<uint32_t>(fx[i], 0x14)) {
                    // made by the engine for a combined effect (haste / slow: the internal effect with the same
                    // id): its time is that of the effects it combines
                    uint64_t id = F<uint64_t>(fx[i], 0);
                    int src = -1;
                    for (int k = 0; k < nf && k < 512; k++)
                        if (ok(k) && F<uint64_t>(fx[k], 0) == id) { if (type(k) == TypeHasteInt) src = TypeHaste; if (type(k) == TypeSlowInt) src = TypeSlow; }
                    if (src < 0) { perm = true; onlyWorn = false; break; }
                    for (int k = 0; k < nf && k < 512; k++) {
                        if (!ok(k) || type(k) != src) continue;
                        if (dur(k) == DurEquipped) continue;
                        onlyWorn = false;
                        float l = left(k);
                        if (l < 0) { perm = true; break; }
                        best = fmaxf(best, l);
                    }
                    continue;
                }
                onlyWorn = false;
                float l = left(i);
                if (l < 0) { perm = true; break; }
                best = fmaxf(best, l);
            }
            if (onlyWorn && best == -2 && !perm) { f.equipment = true; continue; }   // e.g. boots of speed
            f.left = perm || best < 0 ? -1 : best;
        }
    } else if (cre == G::Player()) {
        void* in = G::Internal();
        void* gig = in ? F<void*>(in, OFS::In_GuiInGame) : nullptr;
        void* bar = gig ? F<void*>(gig, 0xb0) : nullptr;                 // CPanelPlayerStateBar
        if (bar && Readable(bar, 0x100) && strstr(G::ClassName(bar), "StateBar")) {
            void** b = F<void**>(bar, 0xf0); int n = F<int>(bar, 0xf8);
            for (int i = 0; b && i < n && i < 64; i++) if (b[i] && Readable(b[i], 0x20)) { FxIcon f; f.icon = (int)F<uint32_t>(b[i], 0x18); v.push_back(f); }
        }
    }
    c.v = v;
    return v;
}

std::vector<FxIcon> EffectIcons(void* cre) {
    std::vector<FxIcon> v;
    for (auto& f : EffectList(cre)) if (!f.equipment) v.push_back(f);
    return v;
}

std::string EffectTimeLeft(float s) {
    if (s < 0) return std::string();
    char b[16];
    int t = (int)ceilf(s);
    if (t < 600) snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    else if (t < 3600) snprintf(b, sizeof b, "%dm", t / 60);
    else snprintf(b, sizeof b, "%dh", t / 3600);
    return b;
}

uint32_t EffectIconTex(int icon) {
    const TwoDA& t = TDA("effecticons");
    const std::string& r = t.Get(icon, "Icon");
    return r.empty() ? 0 : Res_Texture(r.c_str());
}
std::string EffectIconName(int icon) {
    const TwoDA& t = TDA("effecticons");
    int s = t.Int(icon, "StrRef", -1);
    return s >= 0 ? Tlk(s) : std::string();
}
}  // namespace GD
