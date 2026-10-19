#include "gamedata.h"
#include "game.h"
#include "text.h"
#include "res.h"
#include "log.h"
#include <windows.h>
#include <cstring>

using namespace G;

namespace {
typedef int (*GetObjectName_t)(void* app, uint32_t id, void* exoStr);
typedef void (*ExoDtor_t)(void*);
GetObjectName_t fGetObjectName = nullptr;
ExoDtor_t fExoDtor = nullptr;
typedef int (*GetLevel_t)(void* stats);
GetLevel_t fGetLevel = nullptr;
}

namespace GD {
void Init() {
    fGetObjectName = (GetObjectName_t)A(0x1406a9630);
    fExoDtor = (ExoDtor_t)GetProcAddress(GetModuleHandleA(nullptr), "??1CExoString@@QEAA@XZ");
    fGetLevel = (GetLevel_t)A(0x1407f4870);
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
    return h;
}

bool IsDead(void* cre) {
    HP h = CreatureHP(cre);
    return h.known && h.cur <= 0;
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

bool IsPaused() {
    typedef int (*fn)(void*, uint8_t);
    void* app = App();
    if (!app) return false;
    return ((fn)A(0x1406a96a0))(app, 2) != 0;
}
}  // namespace GD
