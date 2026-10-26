// NWNPad - gamepad version of the game's death screen (respawn / load / wait for help / exit)
#pragma once
bool Death_Active();                 // the death screen is up (our version is shown)
bool Death_Input();                  // true: input used
void Death_Draw();
bool Death_KeepHidden(void* panel);  // the game's own death panel stays hidden while ours is shown
