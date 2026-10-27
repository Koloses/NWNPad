// NWNPad - conversation log.
//
// Every line of a conversation (the NPC's text and the reply picked) is recorded as it is shown. When the
// journal reports a quest update (CNWCJournal::JournalUpdated, the "Your journal has been updated" notice)
// while a conversation is open or a few seconds after it closed, the conversation is linked to that quest.
// Single player: the log is written into the save's folder when the game saves (SaveGame) and read back
// when that save is loaded, so an older save brings back the matching log. Online it is kept per character
// in Documents\Neverwinter Nights\NWNPad\.
#include "talklog.h"
#include "game.h"
#include "hooks.h"
#include "text.h"
#include "log.h"
#include "gamedata.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <algorithm>

using namespace G;

namespace {
const uintptr_t VA_JournalUpdated = 0x1407d4c50;      // CNWCJournal::JournalUpdated(int deferrable, int completed, CExoString& name)
const uintptr_t VA_LoadBarState = 0x1406a9490;        // CClientExoApp::GetLoadBarState
const size_t MaxTalks = 400, MaxLines = 400;
const double LinkAfter = 6.0;                         // seconds after a conversation closed that a journal update still belongs to it
const char* FileName = "nwnpad_talk.txt";

std::recursive_mutex g_mx;
std::vector<Talk> g_talks;                            // oldest first
uint32_t g_seq = 0;
bool g_open = false;                                  // the last talk is still running
double g_closedAt = -100;
std::vector<std::string> g_replies;
std::string g_owner;                                  // the hero this log belongs to
std::string g_onlineFile;                             // online: the file of the current character
bool g_dirty = false;
uint32_t g_ver = 1;                                   // bumped on every change (readers copy only then)

double Now() { return GetTickCount64() / 1000.0; }
std::string Norm(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++;
    s = s.substr(i);
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
bool Online() {
    static void** am = (void**)GetProcAddress(GetModuleHandleA(nullptr), "?g_pAppManager@@3PEAVCAppManager@@EA");
    return am && *am && F<int>(*am, 0x28) != 0;
}
std::string Docs() {
    static std::string docs;
    if (docs.empty()) { char d[MAX_PATH]; if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, d))) docs = std::string(d) + "\\Neverwinter Nights\\"; }
    return docs;
}
std::string AreaName() {
    void* pl = Player(); void* ar = pl ? F<void*>(pl, OFS::Obj_Area) : nullptr;
    if (!ar || !F<char*>(ar, 0x238)) return std::string();
    return G2UClean(F<char*>(ar, 0x238), (int)F<uint32_t>(ar, 0x240));
}

// ---- file: one record per line, fields separated by tabs, \t \n \\ escaped ----
std::string Esc(const std::string& s) {
    std::string r;
    for (char c : s) { if (c == '\\') r += "\\\\"; else if (c == '\n') r += "\\n"; else if (c == '\t') r += "\\t"; else if (c != '\r') r += c; }
    return r;
}
std::string Unesc(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { char n = s[++i]; r += n == 'n' ? '\n' : n == 't' ? '\t' : n; }
        else r += s[i];
    }
    return r;
}
std::vector<std::string> Split(const std::string& l) {
    std::vector<std::string> v; size_t p = 0;
    for (;;) { size_t q = l.find('\t', p); v.push_back(Unesc(l.substr(p, q == std::string::npos ? std::string::npos : q - p))); if (q == std::string::npos) break; p = q + 1; }
    return v;
}
bool Write(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { Log("talk: can't write %s", path.c_str()); return false; }
    fprintf(f, "NWNPadTalk\t1\n");
    fprintf(f, "O\t%s\n", Esc(g_owner).c_str());
    for (auto& t : g_talks) {
        fprintf(f, "T\t%s\t%s\t%08x\n", Esc(t.npc).c_str(), Esc(t.area).c_str(), t.npcId);
        for (auto& q : t.quests) fprintf(f, "Q\t%s\n", Esc(q).c_str());
        for (auto& l : t.lines) fprintf(f, "%s\t%s\t%s\n", l.player ? "P" : "N", Esc(l.who).c_str(), Esc(l.text).c_str());
    }
    fclose(f);
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    Log("talk: %d conversations -> %s", (int)g_talks.size(), path.c_str());
    return true;
}
void Read(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    g_talks.clear(); g_open = false; g_seq = 0; g_owner.clear(); g_ver++;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { Log("talk: no log in %s", path.c_str()); return; }
    std::string line; int c;
    auto one = [&]() {
        if (line.empty()) return;
        auto v = Split(line);
        if (v[0] == "O" && v.size() >= 2) g_owner = v[1];
        else if (v[0] == "T" && v.size() >= 3) { Talk t; t.seq = ++g_seq; t.npc = v[1]; t.area = v[2]; if (v.size() >= 4) t.npcId = (uint32_t)strtoul(v[3].c_str(), nullptr, 16) & 0x7fffffffu; g_talks.push_back(t); }
        else if (!g_talks.empty() && v[0] == "Q" && v.size() >= 2) g_talks.back().quests.push_back(v[1]);
        else if (!g_talks.empty() && (v[0] == "N" || v[0] == "P") && v.size() >= 3) { TalkLine l; l.player = v[0] == "P"; l.who = v[1]; l.text = v[2]; g_talks.back().lines.push_back(l); }
    };
    while ((c = fgetc(f)) != EOF) { if (c == '\n') { one(); line.clear(); } else line += (char)c; }
    one();
    fclose(f);
    Log("talk: %d conversations from %s", (int)g_talks.size(), path.c_str());
}

void Close() {
    if (!g_open) return;
    g_open = false; g_closedAt = Now();
    if (!g_talks.empty() && g_talks.back().lines.empty()) g_talks.pop_back();
    g_ver++;
    g_dirty = true;
}

// ---- hooks ----
void (*o_JournalUpdated)(void* j, int deferrable, int completed, void* name) = nullptr;
void hk_JournalUpdated(void* j, int deferrable, int completed, void* name) {
    void* app = App();
    bool shown = !deferrable || !app || ((int (*)(void*))A(VA_LoadBarState))(app) == 0;   // otherwise queued for after the loading screen
    if (shown && name) {
        std::string title = ExoStr(name);
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (!g_talks.empty() && (g_open || Now() - g_closedAt < LinkAfter) && !title.empty()) {
            auto& q = g_talks.back().quests;
            bool have = false;
            for (auto& x : q) if (Norm(x) == Norm(title)) have = true;
            if (!have) { g_ver++; q.push_back(title); g_dirty = true; Log("talk: '%s' linked to the conversation with %s", title.c_str(), g_talks.back().npc.c_str()); }
        }
    }
    o_JournalUpdated(j, deferrable, completed, name);
}

typedef int (*SaveFn_t)(void*, void*, void*, void*, void*, void*, void*);
SaveFn_t o_SaveGame = nullptr;
int hk_SaveGame(void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    int r = o_SaveGame(a, b, c, d, e, f, g);
    uint32_t number = (uint32_t)(uintptr_t)b;
    std::string name = c ? std::string(F<char*>(c, 0) ? F<char*>(c, 0) : "", F<uint32_t>(c, 8)) : std::string();
    char dir[600];
    snprintf(dir, sizeof dir, "%ssaves\\%06u - %s", Docs().c_str(), number, name.c_str());
    if (r && GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES) {
        Write(std::string(dir) + "\\" + FileName);
    } else Log("talk: save %u '%s' -> %d, no folder %s", number, name.c_str(), r, dir);
    return r;
}
}  // namespace

void TalkLog_InstallHooks() {
    o_JournalUpdated = (decltype(o_JournalUpdated))Hook_Create(VA_JournalUpdated, (void*)hk_JournalUpdated, "CNWCJournal::JournalUpdated");
    HMODULE exe = GetModuleHandleA(nullptr);
    void* sg = (void*)GetProcAddress(exe, "?SaveGame@CServerExoAppInternal@@QEAAHIAEAVCExoString@@0PEAVCNWSPlayer@@H0@Z");
    if (sg) o_SaveGame = (SaveFn_t)Hook_Create((uintptr_t)sg - (uintptr_t)exe + 0x140000000ull, (void*)hk_SaveGame, "CServerExoAppInternal::SaveGame");
}

void TalkLog_GameLoaded(uint32_t number, const std::string& name) {
    char dir[600];
    if (number == 0xffffffff) snprintf(dir, sizeof dir, "%ssaves\\%s", Docs().c_str(), name.c_str());
    else snprintf(dir, sizeof dir, "%ssaves\\%06u - %s", Docs().c_str(), number, name.c_str());
    Read(std::string(dir) + "\\" + FileName);
    g_onlineFile.clear();
}

void TalkLog_Npc(uint32_t id, const std::string& who, const std::string& text) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (text.empty()) return;
    if (!g_open) {
        Talk t; t.seq = ++g_seq; t.npc = who; t.npcId = id & 0x7fffffffu; t.area = AreaName();
        g_talks.push_back(t);
        if (g_talks.size() > MaxTalks) g_talks.erase(g_talks.begin());
        g_open = true;
    }
    auto& ls = g_talks.back().lines;
    if (!ls.empty() && !ls.back().player && ls.back().text == text) return;     // the same line shown again
    if (ls.size() < MaxLines) { TalkLine l; l.who = who; l.text = text; ls.push_back(l); } g_ver++;
    g_replies.clear();
}
void TalkLog_Replies(const std::vector<std::string>& r) { std::lock_guard<std::recursive_mutex> lk(g_mx); g_replies = r; }
void TalkLog_Choice(int i) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_open || g_talks.empty() || i < 0 || i >= (int)g_replies.size() || g_replies[i].empty()) return;
    std::string k = Norm(g_replies[i]);
    if (k.size() > 2 && k.front() == '[' && k.back() == ']') k = k.substr(1, k.size() - 2);
    if (k == "continue" || k == "end dialogue" || k == "end conversation" || k == "leave") return;   // nothing said
    auto& ls = g_talks.back().lines;
    if (ls.size() < MaxLines) { TalkLine l; l.player = true; l.text = g_replies[i]; ls.push_back(l); } g_ver++;
    g_replies.clear();
}

void TalkLog_Frame(bool conversationOpen) {
    static double goneSince = -1;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (conversationOpen) goneSince = -1;
        else if (g_open) {
            if (goneSince < 0) goneSince = Now();
            if (Now() - goneSince > 0.3) { Close(); goneSince = -1; }
        }
    }
    static double lastCheck = -10;
    if (Now() - lastCheck < 1.0) return;
    lastCheck = Now();
    void* pl = Player();
    std::string me = pl ? GD::ObjName(Id(pl)) : std::string();
    if (me.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!Online()) {
        // a different hero (a new game): a new log
        if (me != g_owner) {
            if (!g_owner.empty()) { g_ver++; g_talks.clear(); g_open = false; g_seq = 0; Log("talk: new hero '%s', empty log", me.c_str()); }
            g_owner = me;
        }
        return;
    }
    // online: one file per character, written after each conversation
    std::string nm;
    for (char ch : me) if (!strchr("\\/:*?\"<>|", ch) && (unsigned char)ch >= 32) nm += ch;
    std::string dir = Docs() + "NWNPad";
    std::string path = dir + "\\talk - " + nm + ".txt";
    if (path != g_onlineFile) {
        if (g_dirty && !g_onlineFile.empty()) Write(g_onlineFile);
        CreateDirectoryA(dir.c_str(), nullptr);
        g_onlineFile = path;
        Read(path);
        g_owner = me; g_dirty = false;
    }
    if (g_dirty && !g_open && Now() - g_closedAt > LinkAfter) { g_dirty = false; Write(g_onlineFile); }
}

std::vector<Talk> TalkLog_All() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    return std::vector<Talk>(g_talks.rbegin(), g_talks.rend());
}
std::vector<Talk> TalkLog_ForQuest(const std::string& title) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::vector<Talk> r;
    std::string n = Norm(title);
    for (auto it = g_talks.rbegin(); it != g_talks.rend(); ++it)
        for (auto& q : it->quests) if (Norm(q) == n) { r.push_back(*it); break; }
    return r;
}

uint32_t TalkLog_Version() { return g_ver; }

std::set<uint32_t> TalkLog_MovedBy(const std::string& title) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::set<uint32_t> r;
    std::string n = Norm(title);
    for (auto& t : g_talks)
        if (t.npcId) for (auto& q : t.quests) if (Norm(q) == n) { r.insert(t.npcId); break; }
    return r;
}

std::set<std::string> TalkLog_MovedByName(const std::string& title) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::set<std::string> r;
    std::string n = Norm(title);
    for (auto& t : g_talks)
        if (!t.npcId) for (auto& q : t.quests) if (Norm(q) == n) { r.insert(Norm(t.npc)); break; }
    return r;
}
