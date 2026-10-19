// NWNPad - hub pages implemented in their own files (system, journal, spells, map)
#pragma once
#include "uikit.h"
#include <string>
struct HubPage {
    void (*open)();                                            // tab became visible
    void (*input)();                                           // per frame while visible
    void (*draw)(float x, float y, float w, float h, float a);
    int (*hints)(HintItem* out);                               // hint bar entries after "Tabs", returns count
    void (*close)();                                           // tab hidden (hub closed or other tab), may be null
};
extern const HubPage PAGE_SYSTEM, PAGE_JOURNAL, PAGE_SPELLS, PAGE_MAP;
void Hub_Toast(const std::string& s);
void Hub_FireEvent(int ev);
void Hub_ShowTab(const char* name);
// called every frame from the UI layer (pages that wait for game panels)
void HubSystem_Update();
