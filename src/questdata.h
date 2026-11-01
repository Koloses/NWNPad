// NWNPad - what a module's resources say about its quests: compiled scripts, conversations, blueprints,
// the journal. Game independent (resources come through QD_Load).
#pragma once
#include "ncsflow.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// supplied by the user of this file
extern bool (*QD_Load)(const char* resref, uint16_t type, std::vector<uint8_t>& out);
extern std::string (*QD_Tlk)(uint32_t strref);
extern std::string (*QD_Text)(const char* s, int len);   // module text to UTF-8
extern bool (*QD_CanLoad)();                              // may another resource be read now (time slicing)
// run fn(arg) on another thread (scripts are worked out there, the caller sees them pending until done);
// not set: at once
extern void (*QD_RunAsync)(void (*fn)(void*), void* arg);

std::string QD_Lower(std::string s);

// ---- minimal GFF reader ----
struct Gff {
    std::vector<uint8_t> d;
    uint32_t so = 0, sc = 0, fo = 0, fc = 0, lo = 0, lc = 0, fdo = 0, fio = 0, lio = 0;
    bool ok = false;
    bool Load(std::vector<uint8_t>&& v);
    uint32_t U(size_t off) const;
    struct Field { std::string label; uint32_t type, data; };
    std::vector<Field> Fields(uint32_t s) const;                // fields of a struct
    std::string Str(const Field& f) const;                      // CExoString
    std::string Ref(const Field& f) const;                      // CResRef
    std::string Loc(const Field& f) const;                      // CExoLocString (UTF-8)
    std::vector<uint32_t> List(const Field& f) const;           // struct indices of a list
    const Field* Find(const std::vector<Field>& fs, const char* label) const;
};

// ---- scripts ----
struct NcsInfo : NcsFlow::Info {
    bool exists = false;
    bool Relevant() const { return !calls.empty() || !creates.empty() || !jumps.empty() || !execs.empty() || !locals.empty() || !convs.empty() || !setsInt.empty(); }
};
// counters for the slicing: resources read since the last reset; a read was refused (data incomplete)
extern int QD_loads;
extern bool QD_pending;
const NcsInfo& QD_Script(const std::string& resref);

// ---- conversations ----
struct DlgLink { int to; int cond; };                           // node (entries, then replies), condition (-1 none)
struct DlgWrite {
    int node;
    std::string quest;
    uint32_t entry;                                             // 0x7fffffff: not a constant
    std::vector<std::string> tags;                              // what the conditions on the way there test
    std::vector<NcsFlow::Guard> guards;                         // journal tests of the action script around the write
    bool status = false;                                        // a status label (written over whatever it was)
};
struct DlgInfo {
    bool ok = false;
    int entries = 0;                                            // nodes [0, entries) are the NPC's lines
    std::vector<std::vector<DlgLink>> out;
    std::vector<DlgLink> start;
    std::vector<std::string> conds;                             // condition scripts
    std::vector<DlgWrite> writes;
    std::set<std::string> quests;                               // quests it writes
    struct NodeCreate { int node; NcsFlow::Create c; };
    struct NodeJump { int node; std::string tag; };
    std::vector<NodeCreate> creates;                            // what its action scripts create (on which line)
    std::vector<NodeJump> jumps;                                // where they teleport to
    std::set<std::string> convs;                                // conversations they start
    std::set<std::string> setsInt;                              // local ints they set
};
const DlgInfo& QD_Dialog(const std::string& resref);

// ---- blueprints ----
struct Blueprint {
    bool ok = false;
    std::string tag;
    const DlgInfo* dlg = nullptr;
    std::vector<std::string> scripts;                           // event scripts with something for the quests
    std::vector<std::string> items;                             // inventory (item blueprints)
};
const Blueprint& QD_Blueprint(uint16_t routine, const std::string& resref);   // CreateObject (utc/utp) / CreateItemOnObject (uti)

// ---- the journal ----
struct QuestDef { std::string title; std::set<uint32_t> ends; uint32_t first = 0xffffffffu; };
extern std::map<std::string, QuestDef> QD_quests;
void QD_LoadJournal();

void QD_Reset();                                                // another module: forget everything
