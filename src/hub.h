// NWNPad - in-game hub menu (Start): inventory, character, and shortcuts to the game's panels
#pragma once
void Hub_Init();
bool Hub_Open();                 // hub visible (custom tab)
void Hub_Toggle();
void Hub_Close();
// input routing; returns true when the hub consumed the frame
bool Hub_Input();
// while one of the game's panels opened from the hub is up (cursor mode): LB/RB switch tabs
bool Hub_ExternalInput(bool cursorActive);
void Hub_Draw();
bool Hub_ExternalActive();
