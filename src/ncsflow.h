// NWNPad - what a compiled script (NCS) does for the quest marker, worked out by running it on values that
// only say where they came from: the journal entries it writes and the tests guarding them, what it creates,
// where it sends people, the conversations and scripts it starts. No game state is needed.
#pragma once
#include <cstdint>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace NcsFlow {
// in strings: '\x01' = the tag of the object running the script, '\x02' = some number
// a test: journal state of quest <op> n (op one of = ! < > l g; quest "\x04name": a local int), or about a tag:
// h has the item, x has it not, o the object exists, O it does not ("\x05master": the runner is an associate)
struct Guard { std::string quest; char op; int n; };
struct Call {
    std::string quest;
    int entry = -1;                                          // -1: not a constant
    bool override_ = false;                                  // bAllowOverrideHigher (a status, not a step)
    std::set<std::string> tags;                              // what it needs: items had, objects gone, local ints ("\x04name")
    std::vector<Guard> guards;                               // the tests on the way to it
};
struct Create { uint16_t routine; std::string tmpl, newTag; std::set<std::string> at; std::vector<Guard> guards; };   // guards: the tests on the way to it
struct Info {
    bool ok = false;
    std::vector<Call> calls;
    std::vector<Create> creates;
    std::set<std::string> jumps;     // tags of the waypoints / objects it teleports someone to
    std::set<std::string> convs;     // conversations it starts
    std::set<std::string> execs;     // scripts it runs
    std::set<std::string> locals;    // strings it stores in local variables
    std::set<std::string> setsInt;   // local ints it sets (not on the runner itself)
    std::set<std::string> cond;      // everything it tests: tags, "nw_journal_entry<quest>" (a condition script)
    // a condition script (int StartingConditional): the journal tests of each way it can return TRUE
    // (one empty list: it can without any); none recorded = not known
    std::vector<std::vector<Guard>> pass;
    std::set<std::string> req;       // what every way of returning TRUE needs (items, objects gone, "\x04" local ints)
};
Info Analyze(const uint8_t* d, size_t n);
bool Pass(const Guard& g, int state);
}
