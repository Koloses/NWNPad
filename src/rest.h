// NWNPad - gamepad version of the game's "timed event" window (resting progress)
#pragma once
void Rest_InstallHooks();
bool Rest_Active();                 // the game shows a timed event (resting ...)
bool Rest_Input();                  // B cancels; returns true while it owns the input
void Rest_Draw();
bool Rest_KeepHidden(void* panel);  // the game's own panel stays hidden while ours is shown
