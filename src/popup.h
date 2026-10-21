// NWNPad - the game's message / confirmation pop-ups shown as gamepad dialogs
#pragma once
void Popup_InstallHooks();
bool Popup_Active();
bool Popup_Handled(void* panel);
bool Popup_KeepHidden(void* panel);   // panel is shown by the gamepad dialog (no cursor needed)
bool Popup_Input();                // true while a pop-up has the input
void Popup_Draw();
void Popup_Frame();                // once per frame, before input / drawing
