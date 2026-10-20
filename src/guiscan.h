// NWNPad - generic access to the game's classic GUI panels (control names, texts, clicks)
#pragma once
#include <string>
#include <vector>
struct GuiControl { void* obj; std::string name; };
void Gui_InstallHooks();
const std::vector<GuiControl>* Gui_Controls(void* panel);   // controls the panel imported from its layout
void* Gui_Find(void* panel, const char* name);                // control by layout name (case-insensitive)
std::string Gui_Name(void* obj);
std::string Gui_GobText(void* stringGob);
void* Gui_FindGob(void* obj, size_t maxOff = 0x300);
std::string Gui_Text(void* obj);                              // text of a label / text button / list row
bool Gui_Click(void* control);                                // run the control's click callback
bool Gui_Enabled(void* obj);
void Gui_Forget(void* panel);
void Gui_Dump(void* panel);
const std::vector<void*>& Gui_LivePanels();                  // activated (not yet deactivated) panels, newest last
std::vector<std::string> Gui_Texts(void* obj, size_t maxOff = 0x500);   // all texts referenced by the object
std::vector<size_t> Gui_SubObjects(void* obj, size_t maxOff = 0x500);  // offsets of embedded GUI objects
