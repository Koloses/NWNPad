// NWNPad - quest marker on the minimap (single player)
#pragma once
#include <string>
#include "game.h"
void Quest_Init();
void Quest_Track(const std::string& tag);            // "" = the quest updated last
std::string Quest_Tracked();
std::string Quest_Name();                            // title of the quest shown                         // tag (lower case) of the quest shown
// where the marker points: the target in this area, or the exit towards it (here = false)
bool Quest_Marker(Vec3& point, bool& here);
