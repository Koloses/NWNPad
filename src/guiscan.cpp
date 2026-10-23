// NWNPad - generic access to the game's classic GUI panels: names of the controls a panel loaded from
// its layout (CGuiObject::ImportLayoutData), their texts (StringGob) and their click callbacks.
#include "guiscan.h"
#include "game.h"
#include "hooks.h"
#include "text.h"
#include "log.h"
#include "dialog.h"
#include "charpanel.h"
#include "popup.h"
#include "loot.h"
#include "store.h"
#include "rest.h"
#include <windows.h>
#include <map>
#include <vector>
#include <string>
#include <cstring>

using namespace G;

namespace {
const uintptr_t VA_ImportLayoutData = 0x1401fc470;   // (object, panel, name)
typedef int (*Import_t)(void* obj, void* panel, const char* name);
Import_t o_Import = nullptr;
std::map<void*, std::vector<GuiControl>> g_panels;
typedef const char* (*GetLine_t)(void* gob, int line);
typedef int (*GetCount_t)(void* gob);
GetLine_t fLine = nullptr;
GetCount_t fCount = nullptr;

typedef void (*PActivate_t)(void*);
typedef void (*PDeactivate_t)(void*, int);
PActivate_t o_PActivate = nullptr;
PDeactivate_t o_PDeactivate = nullptr;
std::vector<void*> g_live;      // activated panels, newest last
typedef void (*PanelFn)(void*);
PanelFn fHide = nullptr, o_UnHide = nullptr;

// panels the pad UI draws itself must never reach the screen, not even for the single frame between the
// game showing them and our per-frame update hiding them again (that frame reads as a flash / blink)
bool KeepHidden(void* p) {
    return Dialog_KeepHidden(p) || CharPanel_KeepHidden(p) || Popup_KeepHidden(p) || Loot_KeepHidden(p) ||
           Store_KeepHidden(p) || Rest_KeepHidden(p);
}
void hk_UnHide(void* p) {
    if (p && KeepHidden(p)) {
        static void* last = nullptr;
        if (p != last) { Log("kept %s hidden", ClassName(p)); last = p; }
        return;
    }
    o_UnHide(p);
}
void hk_PActivate(void* p) {
    o_PActivate(p);
    if (p && KeepHidden(p)) Gui_HidePanel(p);
    for (size_t i = 0; i < g_live.size(); i++) if (g_live[i] == p) { g_live.erase(g_live.begin() + i); break; }
    g_live.push_back(p);
}
void hk_PDeactivate(void* p, int code) {
    for (size_t i = 0; i < g_live.size(); i++) if (g_live[i] == p) { g_live.erase(g_live.begin() + i); break; }
    g_panels.erase(p);
    o_PDeactivate(p, code);
}

int hk_Import(void* obj, void* panel, const char* name) {
    int r = o_Import(obj, panel, name);
    if (panel && name) {
        auto& v = g_panels[panel];
        bool found = false;
        for (auto& c : v) if (c.obj == obj) { c.name = name; found = true; }
        if (!found) v.push_back({obj, name});
        if (v.size() > 400) v.erase(v.begin());
    }
    return r;
}
}  // namespace

void Gui_InstallHooks() {
    HMODULE exe = GetModuleHandleA(nullptr);
    fLine = (GetLine_t)GetProcAddress(exe, "?GetTextLine@StringGob@@UEAAPEBDH@Z");
    fCount = (GetCount_t)GetProcAddress(exe, "?GetTextLineCount@StringGob@@UEAAHXZ");
    o_Import = (Import_t)Hook_Create(VA_ImportLayoutData, (void*)hk_Import, "CGuiObject::ImportLayoutData");
    o_PActivate = (PActivate_t)Hook_Create(0x1401fa230, (void*)hk_PActivate, "CGuiPanel::Activate");
    o_PDeactivate = (PDeactivate_t)Hook_Create(0x1401fa830, (void*)hk_PDeactivate, "CGuiPanel::Deactivate");
    fHide = (PanelFn)GetProcAddress(exe, "?Hide@CGuiPanel@@QEAAXXZ");
    void* uh = (void*)GetProcAddress(exe, "?UnHide@CGuiPanel@@QEAAXXZ");
    if (uh) o_UnHide = (PanelFn)Hook_Create((uintptr_t)uh - (uintptr_t)exe + 0x140000000ull, (void*)hk_UnHide, "CGuiPanel::UnHide");
}

// the game may show a panel again through paths other than UnHide: hide it again before the next frame
void Gui_EnforceHidden() {
    static void* last = nullptr;
    for (void* p : g_live) {
        if (!Readable(p, 0xc0) || (F<uint32_t>(p, 0xb0) & 2) != 0 || !KeepHidden(p)) continue;
        if (p != last) { Log("hid %s again", ClassName(p)); last = p; }
        Gui_HidePanel(p);
    }
}
void Gui_HidePanel(void* p) {
    if (p && fHide && (F<uint32_t>(p, 0xb0) & 2) == 0) fHide(p);
}
void Gui_ShowPanel(void* p) {         // straight to the game, past the keep-hidden check
    if (p && o_UnHide && (F<uint32_t>(p, 0xb0) & 2) != 0) o_UnHide(p);
}

void Gui_Forget(void* panel) { g_panels.erase(panel); }

const std::vector<GuiControl>* Gui_Controls(void* panel) {
    auto it = g_panels.find(panel);
    return it == g_panels.end() ? nullptr : &it->second;
}

void* Gui_Find(void* panel, const char* name) {
    auto it = g_panels.find(panel);
    if (it == g_panels.end()) return nullptr;
    for (auto& c : it->second) if (_stricmp(c.name.c_str(), name) == 0) return c.obj;
    return nullptr;
}

std::string Gui_GobText(void* gob) {
    if (!gob || !fLine || !fCount || !Readable(gob, 16)) return std::string();
    if (strcmp(ClassName(gob), "StringGob") != 0) return std::string();
    int n = fCount(gob);
    std::string r;
    for (int i = 0; i < n && i < 400; i++) {
        const char* l = fLine(gob, i);
        if (!l) continue;
        if (!r.empty()) r += "\n";
        r += l;
    }
    return G2UClean(r.c_str(), (int)r.size());
}

// the first StringGob referenced from the object's memory (labels, text buttons, list rows)
void* Gui_FindGob(void* obj, size_t maxOff) {
    if (!obj) return nullptr;
    for (size_t off = 8; off < maxOff; off += 8) {
        if (!Readable((char*)obj + off, 8)) break;
        void* p = F<void*>(obj, off);
        if (!p || ((uintptr_t)p & 7)) continue;
        if (strcmp(ClassName(p), "StringGob") == 0) return p;
    }
    return nullptr;
}

std::string Gui_Text(void* obj) { return Gui_GobText(Gui_FindGob(obj, 0x300)); }

bool Gui_Click(void* button) {
    if (!button || !Readable(button, 0xc0)) return false;
    const char* cn = ClassName(button);
    typedef void (*cb0)(void*);
    typedef void (*cb1)(void*, int);
    void** cur = nullptr;
    static void** s_cur = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?s_pCurrentEventObject@CGuiObject@@1PEAV1@EA");
    cur = s_cur;
    void* prev = cur ? *cur : nullptr;
    Log("gui: click %s '%s'", cn, Gui_Name(button).c_str());
    if (cur) *cur = button;
    bool ok = false;
    void* ts = Readable((char*)button + 0xa8, 8) ? F<void*>(button, 0xa8) : nullptr;
    bool radio = false;
    if (ts && !((uintptr_t)ts & 7) && Readable(ts, 0x30)) {
        void** arr = F<void**>(ts, 0x18); int n = F<int>(ts, 0x20); int idx = F<int>(button, 0xa0);
        radio = arr && n > 0 && n < 4000 && idx >= 0 && idx < n && Readable(arr + idx, 8) && arr[idx] == button;
    }
    if (radio) {
        typedef void (*tab_t)(void*, int);
        static tab_t fTab = (tab_t)GetProcAddress(GetModuleHandleA(nullptr), "?HandleTabClick@CGuiTabSet@@IEAAXH@Z");
        if (fTab) { fTab(ts, F<int>(button, 0xa0)); ok = true; }
    } else if (strstr(cn, "TimerButton")) {
        void* o = F<void*>(button, 0xd0); void* f = F<void*>(button, 0xd8);
        if (f) { ((cb0)f)(o); ok = true; }
    } else if (strstr(cn, "Checkbox") || strstr(cn, "RadioButton") || strstr(cn, "TabButton")) {
        void* o = F<void*>(button, 0x80); void* f = F<void*>(button, 0x88);
        if (f) { ((cb1)f)(o, F<int>(button, 0x90) == 0); ok = true; }
    } else {
        void* o = F<void*>(button, 0x90); void* f = F<void*>(button, 0x98);
        if (f) { ((cb0)f)(o); ok = true; }
    }
    if (cur) *cur = prev;
    return ok;
}

std::string Gui_Name(void* obj) {
    for (auto& kv : g_panels) for (auto& c : kv.second) if (c.obj == obj) return c.name;
    return std::string();
}

bool Gui_Enabled(void* obj) { return obj && (F<uint32_t>(obj, 8) & 1) == 0; }

void Gui_Dump(void* panel) {
    Log("gui: panel %p %s", panel, ClassName(panel));
    auto it = g_panels.find(panel);
    if (it == g_panels.end()) {
        Log("gui:   (no controls recorded)");
        int k = 0;
        for (auto& t : Gui_Texts(panel, 0x1000)) { std::string x = t; for (auto& ch : x) if (ch == '\n') ch = '|'; Log("gui:   text %d '%s'", k++, x.substr(0, 100).c_str()); }
        for (size_t so : Gui_SubObjects(panel, 0x1000)) {
            void* o = (char*)panel + so;
            std::string x = Gui_Text(o); for (auto& ch : x) if (ch == '\n') ch = '|';
            Log("gui:   sub +%#x %s flags %08x '%s'", (int)so, ClassName(o), F<uint32_t>(o, 8), x.substr(0, 80).c_str());
        }
        return;
    }
    for (auto& c : it->second) {
        std::string t = Gui_Text(c.obj);
        if (t.size() > 60) t = t.substr(0, 60) + "...";
        for (auto& ch : t) if (ch == '\n') ch = '|';
        Log("gui:   %p %-28s %-26s flags %08x '%s'", c.obj, c.name.c_str(), ClassName(c.obj), F<uint32_t>(c.obj, 8), t.c_str());
        if (strcmp(ClassName(c.obj), "CGuiButtonListbox") == 0) {
            int n = F<int>(c.obj, 0xa0); void** rows = F<void**>(c.obj, 0x98);
            int* ids = F<int*>(c.obj, 0x420); int ni = F<int>(c.obj, 0x428);
            Log("gui:     %d rows, %d ids, selected %d", n, ni, F<int>(c.obj, 0x90));
            for (int i = 0; rows && i < n && i < 6; i++) {
                std::string all;
                for (auto& x : Gui_Texts(rows[i])) all += "[" + x + "]";
                std::string subs;
                for (auto& so : Gui_SubObjects(rows[i])) { char b[64]; snprintf(b, sizeof b, " %s@%#x", ClassName((char*)rows[i] + so), (int)so); subs += b; }
                Log("gui:     row %d %s id %d flags %08x %s |%s", i, ClassName(rows[i]), ids && i < ni ? ids[i] : -1, F<uint32_t>(rows[i], 8), all.c_str(), subs.c_str());
            }
        }
    }
}

const std::vector<void*>& Gui_LivePanels() { return g_live; }

std::vector<std::string> Gui_Texts(void* obj, size_t maxOff) {
    std::vector<std::string> v;
    if (!obj) return v;
    for (size_t off = 8; off < maxOff; off += 8) {
        if (!Readable((char*)obj + off, 8)) break;
        void* p = F<void*>(obj, off);
        if (!p || ((uintptr_t)p & 7)) continue;
        if (strcmp(ClassName(p), "StringGob") == 0) v.push_back(Gui_GobText(p));
    }
    return v;
}

// GUI objects embedded in an object (sub-buttons of list rows): offsets of their vtables
std::vector<size_t> Gui_SubObjects(void* obj, size_t maxOff) {
    std::vector<size_t> v;
    if (!obj) return v;
    for (size_t off = 0x40; off < maxOff; off += 8) {
        if (!Readable((char*)obj + off, 8)) break;
        const char* n = ClassName((char*)obj + off);
        if (n[0] == 'C' && n[1] == 'G' && n[2] == 'u' && n[3] == 'i') v.push_back(off);
    }
    return v;
}
