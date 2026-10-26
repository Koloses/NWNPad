// NWNPad - conversation log: what NPCs said and what you answered, kept with the save and linked to the
// quests whose journal entries changed during (or right after) the conversation
#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct TalkLine { bool player = false; std::string who, text; };
struct Talk {
    uint32_t seq = 0;
    std::string npc, area;
    std::vector<TalkLine> lines;
    std::vector<std::string> quests;     // quest titles updated during the conversation
};
void TalkLog_InstallHooks();
void TalkLog_Npc(const std::string& who, const std::string& text);   // a line on the conversation window
void TalkLog_Choice(int index);                                      // the reply picked (index into the shown replies)
void TalkLog_Replies(const std::vector<std::string>& replies);
void TalkLog_Frame(bool conversationOpen);
void TalkLog_GameLoaded(uint32_t number, const std::string& name);   // server LoadGame (single player)
std::vector<Talk> TalkLog_All();                                     // newest first
std::vector<Talk> TalkLog_ForQuest(const std::string& title);        // newest first
