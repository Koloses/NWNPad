#pragma once
#include <cstdint>
void Control_Init();
void Control_PreInput(void* internal);
void Control_PostInput(void* internal);
void Control_Drive(void* internal, void (*orig)(void*));
// state shared with the UI layer
struct ControlState {
    uint32_t softTarget = 0;     // object id of current soft target (0 = none)
    bool locked = false;
    bool inActionTargetMode = false;
    bool radialOpen = false;
    bool padActive = false;      // pad used recently (vs mouse)
    int quickBank = 0;           // 0..2
    double lastPadUse = 0;
};
extern ControlState g_ctl;
void Control_ClickQuickSlot(int bank, int slot);
void* Control_QuickButton(int bank, int slot);
bool Control_UIWantsInput();     // implemented by UI layer
void Control_SetMouse(int x, int y);
void Control_GuiSize(int& w, int& h);
int Control_GuiKey(int key, int down);
