// NWNPad - quest markers on the minimap and the map (single player)
#pragma once
#include <string>
#include <vector>
#include "game.h"
void Quest_Init();
void Quest_Track(const std::string& tag);            // follow this quest; "" = the quest updated last
void Quest_Untrack();                                // no marker until a quest gets a new journal entry
std::string Quest_Tracked();                         // tag (lower case) of the followed quest
bool Quest_FollowChosen();                           // a quest was chosen (not just the latest)
std::string Quest_Name();                            // title of the followed quest
std::string Quest_Title(const std::string& tag);
// where the followed quest's marker points: the target in this area, or the exit towards it (here = false)
bool Quest_Marker(Vec3& point, bool& here);
struct QuestMark {
    std::string tag, title;
    bool found = false, here = false, followed = false;
    Vec3 point;
};
std::vector<QuestMark> Quest_All();                  // every active quest
