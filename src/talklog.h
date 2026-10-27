// NWNPad - conversation log: what NPCs said and what you answered, kept with the save and linked to the
// quests whose journal entries changed during (or right after) the conversation
#pragma once
#include <string>
#include <vector>
#include <set>
#include <cstdint>

struct TalkLine { bool player = false; std::string who, text; };
struct Talk {
    uint32_t seq = 0;
    std::string npc, area;
    uint32_t npcId = 0;                  // the object (ids are kept in saves); 0 when unknown
    std::vector<TalkLine> lines;
    std::vector<std::string> quests;     // quest titles updated during the conversation
};
void TalkLog_InstallHooks();
void TalkLog_Npc(uint32_t id, const std::string& who, const std::string& text);   // a line on the conversation window
void TalkLog_Choice(int index);                                      // the reply picked (index into the shown replies)
void TalkLog_Replies(const std::vector<std::string>& replies);
void TalkLog_Frame(bool conversationOpen);
void TalkLog_GameLoaded(uint32_t number, const std::string& name);   // server LoadGame (single player)
uint32_t TalkLog_Version();                                          // changes whenever the log does
std::vector<Talk> TalkLog_All();                                     // newest first
std::vector<Talk> TalkLog_ForQuest(const std::string& title);        // newest first
std::set<uint32_t> TalkLog_MovedBy(const std::string& title);       // who you talked to when this quest moved on (ids, high bit off)
std::set<std::string> TalkLog_MovedByName(const std::string& title); // the same by name (lower case) for talks logged without an id
