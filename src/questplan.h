// NWNPad - one round of working out where each active quest goes on: who and what moves it on, and which
// entry each of them leads to. Game independent: the game (questmark.cpp) and the offline audit tool feed it
// the objects of the module.
#pragma once
#include "questdata.h"

namespace QP {
struct P3 { float x = 0, y = 0, z = 0; };
struct Target {
    uint32_t id = 0, area = 0;
    P3 pos;
    bool isArea = false;
    uint32_t entry = 0x7fffffff;        // the lowest journal entry it leads to
    bool giver = false;                 // talks the quest on (an objective otherwise)
    bool party = false;                 // a giver in your party (a henchman's tale): nothing to point at
    bool loose = false;                 // a giver whose line local ints keep shut (only when there is nobody else)
    bool maybe = false;                 // created only under a condition not known here (after the sure ones)
    std::string via;                    // objective: the tag it was found by (or the script, blueprint)
    std::string needs;                  // " "-separated: what its line / script needs (it waits for whoever has it)
    const char* why = "";               // (for the logs)
};
// a local int of the module ("m:name") or the player ("p:name"); false: not known (the offline audit)
extern bool (*LocalInt)(const std::string& key, int& value);
// (audit) told where each tag a quest waits for comes from
extern void (*Trace)(const std::string& quest, const std::string& tag, uint32_t entry, const std::string& from);
struct Edge { uint32_t from, to; P3 at; uint32_t obj; };
struct Offer { uint32_t id, area; P3 pos; bool maybe; std::vector<std::string> quests; };
struct Obj {                            // an object of the module (5 creature, 7 trigger, 9 placeable, 10 door, 12 waypoint)
    uint32_t id = 0, area = 0;
    P3 pos;
    uint8_t type = 0;
    std::string tag;                    // lower case
    std::vector<std::string> scripts;   // its event scripts (any)
    std::string dialog;
    bool party = false;                 // one of yours (only his conversation counts)
};

// the event scripts that matter, with those they run (ExecuteScript, "<tag>_setup")
std::vector<std::string> Scripts(const std::vector<std::string>& names, const std::string& tag);
// a tag from a script with the runner's tag / a number in it: the possible tags
std::vector<std::string> Expand(const std::string& t, const std::string& owner);

struct Round {
    // input
    std::map<std::string, uint32_t> state;               // active quests: current entry
    std::set<std::string> known;                         // every quest in the journal, finished ones too
    // output
    std::map<std::string, std::map<std::string, uint32_t>> goal;   // quest -> tag it waits for -> lowest entry
    std::map<std::string, std::vector<Target>> objectives, givers, byTag;
    std::vector<Offer> offers;
    std::vector<Edge> edges;                             // teleports (the game adds the area transitions)
    std::set<std::string> held;                          // item tags you (or your party) carry

    // a step returns false when a resource could not be read in this slice: call it again later
    bool Module(const std::vector<std::string>& scripts);
    bool Area(uint32_t id, const std::vector<std::string>& scripts);
    bool Object(const Obj& o);
    bool Spawned(bool& done);                            // what scripts create, one at a time
    void Match();                                        // tags -> objects (after every object)
    void Item(const std::string& tag, const Target& where, bool mine = false);   // an item somewhere (after Match)
    // the candidates to point at among those given (reachable ones): the lowest entry, objectives first;
    // none when that is someone in your party
    std::vector<const Target*> Best(const std::string& quest, const std::vector<const Target*>& v) const;

    bool Later(const std::string& q, int64_t entry) const;
    bool GuardsOk(const std::vector<NcsFlow::Guard>& g, bool locals = true) const;

  private:
    struct Spawn { Target self; std::string selfTag; uint16_t routine; std::string resref, newTag; int depth; std::set<std::string> talks; int tries = 0; bool maybe = false; };
    std::vector<Spawn> created;
    size_t ci = 0;
    struct Spawner { Target self; std::string tag; std::set<std::string> talks; bool maybe = false; };
    std::vector<Spawner> spawners;
    std::vector<std::pair<Target, std::string>> jumps;
    std::vector<Spawner> setters;                        // who sets a local int ("\x04name")
    std::map<std::string, std::vector<std::pair<std::string, uint32_t>>> questsOf;
    struct DlgRound {
        bool ok = false;
        std::map<std::string, std::map<uint32_t, std::vector<std::string>>> moves;   // active quest -> later entry -> tags
        std::map<std::string, std::map<uint32_t, std::vector<std::string>>> loose;   // the same, lines only the local ints keep shut
        std::map<std::string, std::map<uint32_t, std::vector<std::string>>> needs;   // what every line writing that entry needs
        std::vector<std::string> offers;
        std::vector<char> reach;                         // the lines you can get to (local ints left open)
        bool maybe = true;
    };
    std::map<const DlgInfo*, DlgRound> dlg;
    const DlgRound& Eval(const DlgInfo& d);
    void Calls(const std::vector<std::string>& scripts, const Target* self, const std::string& tag, const std::set<std::string>& skip);
    void Goal(const std::string& q, const std::string& tag, const std::string& owner, uint32_t entry, const std::string& from);
};
}
