// NWNPad - item descriptions for the gamepad screens: asks the server for the examine data of an
// item without showing the game's examine window and captures the text the window would display.
#include "describe.h"
#include "game.h"
#include "hooks.h"
#include "inv.h"
#include "text.h"
#include "pad.h"
#include "log.h"
#include <windows.h>
#include <map>
#include <vector>
#include <string>

using namespace G;

namespace {
const uintptr_t VA_Msg_HandleExamineItemData = 0x140798ce0;   // CNWCMessage::HandleServerToPlayerExamineGui_ItemData
const uintptr_t VA_Msg_SendExamine = 0x1407be580;            // CNWCMessage::SendPlayerToServerInput_Examine(msg, id)

typedef int (*Handler_t)(void* msg);
Handler_t o_Handler = nullptr;
typedef bool (*SetText_t)(void* gob, const char* s);
typedef int (*AddLine_t)(void* gob, const char* s);
SetText_t o_SetText = nullptr, o_AddText = nullptr;
AddLine_t o_AddLine = nullptr;
typedef void (*Deactivate_t)(void* panel, int);
Deactivate_t fDeactivate = nullptr;

bool g_capturing = false;
struct Capture { std::vector<std::string> parts; size_t chars = 0; };
std::map<void*, Capture> g_cap;

uint32_t g_pending = 0x7f000000;
double g_pendingTime = -10;
std::map<uint32_t, std::string> g_cache;

}  // namespace
void (*g_textSink)(void* gob, const char* s, bool reset) = nullptr;   // popup text capture
namespace {
void Add(void* gob, const char* s, bool reset, bool newline) {
    if (g_textSink && s && !g_capturing) g_textSink(gob, s, reset || newline);
    if (!g_capturing || !s) return;
    Capture& c = g_cap[gob];
    if (reset) { c.parts.clear(); c.chars = 0; }
    std::string t = G2UClean(s);
    if (newline && !c.parts.empty()) c.parts.push_back("\n");
    c.parts.push_back(t);
    c.chars += t.size();
}
// SetText/AddText call each other internally: only the outermost call is recorded
int g_depth = 0;
bool hk_SetText(void* g, const char* s) { if (!g_depth) Add(g, s, true, false); g_depth++; bool r = o_SetText(g, s); g_depth--; return r; }
bool hk_AddText(void* g, const char* s) { if (!g_depth) Add(g, s, false, false); g_depth++; bool r = o_AddText(g, s); g_depth--; return r; }
int hk_AddLine(void* g, const char* s) { if (!g_depth) Add(g, s, false, true); g_depth++; int r = o_AddLine(g, s); g_depth--; return r; }

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

int hk_Handler(void* msg) {
    bool silent = g_pending != 0x7f000000 && g_pad.now - g_pendingTime < 3.0;
    if (!silent) return o_Handler(msg);
    void* before = TopModal();
    g_cap.clear();
    g_capturing = true;
    int r = o_Handler(msg);
    g_capturing = false;
    // the biggest text block is the description scroll
    std::string best; size_t bestN = 0;
    for (auto& kv : g_cap) {
        if (kv.second.chars <= bestN) continue;
        bestN = kv.second.chars;
        best.clear();
        for (auto& p : kv.second.parts) best += p;
    }
    // trim
    while (!best.empty() && (best.back() == '\n' || best.back() == ' ')) best.pop_back();
    size_t st = best.find_first_not_of("\n ");
    best = st == std::string::npos ? std::string() : best.substr(st);
    g_cache[g_pending] = best;
    Log("description for %08x: %d chars", g_pending, (int)best.size());
    g_pending = 0x7f000000;
    // close the examine window the handler opened (never drawn: same frame)
    void* after = TopModal();
    if (after && after != before && fDeactivate) fDeactivate(after, 0);
    return r;
}
}  // namespace

void Describe_InstallHooks() {
    HMODULE exe = GetModuleHandleA(nullptr);
    auto va = [&](const char* name) -> uintptr_t {
        void* p = (void*)GetProcAddress(exe, name);
        return p ? (uintptr_t)p - G::base + 0x140000000ULL : 0;
    };
    fDeactivate = (Deactivate_t)GetProcAddress(exe, "?Deactivate@CGuiModalPanel@@QEAAXH@Z");
    uintptr_t a;
    if ((a = va("?SetText@StringGob@@UEAA_NPEBD@Z"))) o_SetText = (SetText_t)Hook_Create(a, (void*)hk_SetText, "StringGob::SetText");
    if ((a = va("?AddText@StringGob@@UEAA_NPEBD@Z"))) o_AddText = (SetText_t)Hook_Create(a, (void*)hk_AddText, "StringGob::AddText");
    if ((a = va("?AddLine@StringGob@@UEAAHPEBD@Z"))) o_AddLine = (AddLine_t)Hook_Create(a, (void*)hk_AddLine, "StringGob::AddLine");
    o_Handler = (Handler_t)Hook_Create(VA_Msg_HandleExamineItemData, (void*)hk_Handler, "ExamineGui_ItemData");
}

const std::string* Describe_Get(uint32_t itemId) {
    auto it = g_cache.find(itemId);
    if (it != g_cache.end()) return &it->second;
    if (!o_Handler || !o_SetText || !o_AddLine) return nullptr;
    // one request at a time; resend after a timeout
    if (g_pending == itemId && g_pad.now - g_pendingTime < 3.0) return nullptr;
    if (g_pending != 0x7f000000 && g_pad.now - g_pendingTime < 3.0) return nullptr;
    void* m = Inv::Message();
    if (!m) return nullptr;
    g_pending = itemId;
    g_pendingTime = g_pad.now;
    typedef void (*fn)(void*, uint32_t);
    ((fn)A(VA_Msg_SendExamine))(m, itemId);
    return nullptr;
}

void Describe_Forget(uint32_t itemId) { g_cache.erase(itemId); }
void Describe_Clear() { g_cache.clear(); }
