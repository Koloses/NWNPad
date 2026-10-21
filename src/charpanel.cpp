// NWNPad - level-up and character creation screens on the pad UI. The game's own panels
// (CCharacter*Panel) stay alive but hidden; their lists, labels and buttons are read through the
// layout names recorded by guiscan and driven through the controls' own callbacks.
#include "charpanel.h"
#include "guiscan.h"
#include "ui.h"
#include "uikit.h"
#include "render.h"
#include "game.h"
#include "twoda.h"
#include "res.h"
#include "text.h"
#include "gamedata.h"
#include "inv.h"
#include "pad.h"
#include "log.h"
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <cmath>
#include <algorithm>

using namespace G;
using namespace R;

namespace {
typedef void (*PanelFn)(void*);
PanelFn fHide = nullptr, fUnHide = nullptr;

// panels shown with the pad UI (others, like appearance or portrait, keep the gamepad cursor)
const char* HANDLED[] = {"CCharacterClassPanel", "CCharacterLevelUpAbilitiesPanel", "CCharacterAbilitiesPanel",
                         "CCharacterSkillsPanel", "CCharacterFeatsPanel", "CCharacterSpellsPanel", "CCharacterCantripsPanel",
                         "CCharacterSchoolsPanel", "CCharacterDomainPanel", "CCharacterFamiliarPanel",
                         "CCharacterPackagesPanel", "CCharacterLevelUpSummaryPanel", "CCharacterRacePanel",
                         "CCharacterSexPanel", "CCharacterAlignmentPanel", "CCharacterGenerationPanel",
                         "CCharacterPortraitPanel", "CCharacterInfoPanel"};

enum Kind { K_INFO, K_OTHER, K_CLASS, K_SKILL, K_FEAT, K_SPELL, K_DOMAIN, K_RACE, K_SCHOOL, K_FAMILIAR, K_PACKAGE };

struct Row {
    void* obj = nullptr;
    std::string text, value;
    int id = -1;
    bool enabled = true;
    void* up = nullptr; void* down = nullptr;     // skill / ability +/- buttons
    void* action = nullptr;                      // add / remove arrow of feat and spell rows
};
struct List { void* box = nullptr; bool radio = false; std::string name, caption; std::vector<Row> rows; int sel = -1; };
struct Model {
    void* panel = nullptr;
    std::string cls, title, help, points, pointsCaption;
    Kind kind = K_OTHER;
    std::vector<List> lists;
    std::vector<std::pair<void*, std::string>> buttons;   // named buttons (layout name -> object)
    std::vector<void*> levels;                            // spell level tabs
    int level = -1;
    bool menu = false;                                    // only buttons: shown as a menu
    bool portraits = false;                               // portrait picker
    std::vector<std::string> pics; int picSel = -1;
};

Model g_m;

std::string Strip(std::string s);
// character generation hub (CCharacterGenerationPanel): its step buttons in the game's order
const size_t GEN_Creature = 0xbe8;     // creature id being built (Linux +0xd58)
const size_t GEN_Encouraged = 0xc08;   // button the game suggests next
const size_t RADIO_On = 0x90;          // CGuiCharGenRadioButton: checked
const size_t STEP_Done = 0xe8;         // CGuiCharGenCompleteableButton: step completed
const char* STEPS[] = {"GenderButton", "RaceButton", "PortraitButton", "ClassButton", "AlignButton",
                       "AbilitiesButton", "PackagesButton", "CustomizeButton", "PlayButton"};
struct Step { void* b = nullptr; std::string label; bool done = false, en = false, next = false; };
std::vector<Step> g_steps;
// portrait picker (CCharacterPortraitPanel): CGuiPortraitSelect at +0x378, resrefs (17 bytes) +0x408 / count +0x410,
// its tab set at +0xf0 (selected index at tab set +0x10)
const size_t PORT_Select = 0x378, PS_Refs = 0x408, PS_Count = 0x410, PS_Tabs = 0xf0;
int g_picCur = 0;
void BuildPortraits(Model& m) {
    char* ps = (char*)m.panel + PORT_Select;
    int n = F<int>(ps, PS_Count);
    char* refs = F<char*>(ps, PS_Refs);
    if (n < 0 || n > 5000 || !refs || !Readable(refs, (size_t)n * 17)) return;
    for (int i = 0; i < n; i++) { char r[17] = {}; memcpy(r, refs + i * 17, 16); m.pics.push_back(r); }
    m.picSel = F<int>(ps + PS_Tabs, 0x10);
    m.portraits = true;
}
void PickPortrait(int i) {
    typedef void (*tab_t)(void*, int);
    static tab_t fTab = (tab_t)GetProcAddress(GetModuleHandleA(nullptr), "?HandleTabClick@CGuiTabSet@@IEAAXH@Z");
    if (fTab) fTab((char*)g_m.panel + PORT_Select + PS_Tabs, i);
}
void BuildSteps(void* panel) {
    g_steps.clear();
    void* enc = Readable((char*)panel + GEN_Encouraged, 8) ? F<void*>(panel, GEN_Encouraged) : nullptr;
    for (auto n : STEPS) {
        void* b = Gui_Find(panel, n);
        if (!b) continue;
        Step st; st.b = b; st.label = Strip(Gui_Text(b));
        if (st.label.empty()) st.label = n;
        st.en = Gui_Enabled(b);
        st.done = strcmp(n, "PlayButton") != 0 && F<int>(b, STEP_Done) != 0;
        st.next = b == enc;
        g_steps.push_back(st);
    }
}
void* GenCreature(void* panel) {
    uint32_t id = F<uint32_t>(panel, GEN_Creature);
    void* o = id ? GetObj(id) : nullptr;
    return o && Type(o) == OT_CREATURE ? o : nullptr;
}
void* g_panel = nullptr;          // panel shown by the pad UI
void* g_hidden = nullptr;         // panel we hid
int g_focus = 0;                  // focused list
int g_cursor[4] = {0, 0, 0, 0};   // our cursor per list
int g_menuSel = 0;
float g_descScroll = 0; std::string g_descKey;
double g_openTime = 0;
double g_fadeStart = -10;         // fade-in only when the pad window appears, not between creation steps
double g_lastShown = -10;
bool g_waitRelease = false;      // buttons held when a panel opened belong to the previous one

bool Handled(const char* cn) {
    for (auto h : HANDLED) if (!strcmp(cn, h)) return true;
    return false;
}

void* g_classic = nullptr;        // panel the player chose to use with the classic interface (pad cursor)
void* CurrentPanel() {
    const auto& lp = Gui_LivePanels();
    bool classicAlive = false;
    for (void* q : lp) if (q == g_classic) classicAlive = true;
    if (!classicAlive) g_classic = nullptr;
    for (int i = (int)lp.size() - 1; i >= 0; i--) {
        void* p = lp[i];
        if (!Readable(p, 0xc0)) continue;
        const char* cn = ClassName(p);
        if (strncmp(cn, "CCharacter", 10) != 0) continue;
        if (!Handled(cn) || p == g_classic) return nullptr;       // newest character panel decides
        if ((F<uint32_t>(p, 0xb0) & 1) == 0) continue;            // not active
        return p;
    }
    return nullptr;
}

// the game's help boxes wrap their text with hard line breaks: join wrapped lines again
std::string Reflow(const std::string& s) {
    std::string r;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(i, e - i);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        r += line;
        if (e < s.size()) {
            bool nextBreak = e + 1 >= s.size() || s[e + 1] == '\n' || s[e + 1] == '-' || s[e + 1] == '*';
            bool longLine = line.size() >= 30;
            r += (longLine && !nextBreak) ? " " : "\n";
        }
        i = e + 1;
    }
    return r;
}

std::string Squeeze(const std::string& s) {      // at most one empty line in a row
    std::string r; int nl = 0;
    for (char c : s) { if (c == '\n') { if (++nl > 2) continue; } else nl = 0; r += c; }
    return r;
}

std::string Strip(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
    size_t a = s.find_first_not_of(" \n");
    return a == std::string::npos ? std::string() : s.substr(a);
}

// object size of a list row: the next row of the same class starts right after it
size_t RowSize(void* row) {
    const char* cn = ClassName(row);
    std::string own = cn;
    for (size_t off = 0x40; off < 0x800; off += 8) {
        if (!Readable((char*)row + off, 8)) return off;
        uintptr_t vt = F<uintptr_t>(row, off);
        if (!vt || (vt & 7)) continue;
        if (!Readable((void*)(vt - 8), 8)) continue;
        uintptr_t col = *(uintptr_t*)(vt - 8);
        if (!col || (col & 3) || !Readable((void*)col, 16)) continue;
        if (*(int32_t*)(col + 4) != 0) continue;                  // secondary vtable of this object
        if (own == ClassName((char*)row + off)) return off;
    }
    return 0x800;
}

Kind KindOf(const std::string& cls) {
    if (cls == "CCharacterClassPanel" || cls == "CCharacterPackagesPanel") return cls == "CCharacterClassPanel" ? K_CLASS : K_PACKAGE;
    if (cls == "CCharacterSkillsPanel") return K_SKILL;
    if (cls == "CCharacterFeatsPanel") return K_FEAT;
    if (cls == "CCharacterSpellsPanel" || cls == "CCharacterCantripsPanel") return K_SPELL;
    if (cls == "CCharacterDomainPanel") return K_DOMAIN;
    if (cls == "CCharacterRacePanel") return K_RACE;
    if (cls == "CCharacterSchoolsPanel") return K_SCHOOL;
    if (cls == "CCharacterFamiliarPanel") return K_FAMILIAR;
    if (cls == "CCharacterInfoPanel") return K_INFO;
    return K_OTHER;
}

bool EndsWith(const std::string& s, const char* e) { size_t n = strlen(e); return s.size() >= n && _stricmp(s.c_str() + s.size() - n, e) == 0; }

void Build(void* panel) {
    Model m;
    m.panel = panel;
    m.cls = ClassName(panel);
    m.kind = KindOf(m.cls);
    const auto* ctl = Gui_Controls(panel);
    if (!ctl) { g_m = m; return; }
    static const char* AB[6] = {"Str", "Dex", "Con", "Int", "Wis", "Cha"};
    List ab; ab.name = "Abilities";
    bool hasAbilities = false;
    for (int k = 0; k < 6; k++) {
        std::string b = AB[k];
        void* lab = Gui_Find(panel, (b + "Label").c_str());
        void* ed = Gui_Find(panel, (b + "Edit").c_str());
        void* up = Gui_Find(panel, (b + "UpButton").c_str());
        void* dn = Gui_Find(panel, (b + "DownButton").c_str());
        if (!lab || !up) continue;
        hasAbilities = true;
        Row r; r.obj = lab; r.text = Strip(Gui_Text(lab)); r.value = ed ? Strip(Gui_Text(ed)) : ""; r.up = up; r.down = dn; r.id = k;
        ab.rows.push_back(r);
    }
    List radios; radios.name = "Options"; radios.radio = true;
    for (auto& c : *ctl) {
        const char* cn = ClassName(c.obj);
        const std::string& nm = c.name;
        if (strstr(cn, "RadioButton") && strncmp(nm.c_str(), "SpellLevel", 10) != 0) {
            Row r; r.obj = c.obj; r.text = Strip(Gui_Text(c.obj)); r.id = (int)radios.rows.size();
            r.enabled = (F<uint32_t>(c.obj, 8) & 1) == 0;
            if (F<int>(c.obj, RADIO_On)) radios.sel = (int)radios.rows.size();
            radios.rows.push_back(r);
            continue;
        }
        if (!strcmp(cn, "CGuiButtonListbox")) {
            List l; l.box = c.obj; l.name = nm;
            int n = F<int>(c.obj, 0xa0); void** rows = F<void**>(c.obj, 0x98);
            int* ids = F<int*>(c.obj, 0x420); int ni = F<int>(c.obj, 0x428);
            l.sel = F<int>(c.obj, 0x90);
            for (int i = 0; rows && i < n && i < 2000; i++) {
                void* ro = rows[i];
                if (!ro || !Readable(ro, 0x100)) continue;
                Row r; r.obj = ro; r.id = ids && i < ni ? ids[i] : -1;
                size_t sz = RowSize(ro);
                std::vector<std::string> tx = Gui_Texts(ro, sz);
                if (!tx.empty()) r.text = Strip(tx[0]);
                if (tx.size() > 1) r.value = Strip(tx[1]);
                r.enabled = (F<uint32_t>(ro, 8) & 1) == 0;
                if (!strcmp(ClassName(ro), "CGuiClassButton") && Readable((char*)ro + 0xc8, 4)) r.enabled = r.enabled && F<int>(ro, 0xc8) != 0;
                std::vector<void*> timers, childs;
                for (size_t so : Gui_SubObjects(ro, sz)) {
                    const char* sc = ClassName((char*)ro + so);
                    if (strstr(sc, "TimerButton")) timers.push_back((char*)ro + so);
                    else if (strstr(sc, "MouseoverChildButton")) childs.push_back((char*)ro + so);
                }
                if (timers.size() >= 2) { r.up = timers[0]; r.down = timers[1]; }
                if (childs.size() >= 2) r.action = childs[0];          // add / remove arrow (the other one is "info")
                l.rows.push_back(r);
            }
            m.lists.push_back(l);
        } else if (!strcmp(cn, "CGuiCGHelpBox") || !strcmp(cn, "CGuiEditScroll")) {
            std::string t = Strip(Gui_Text(c.obj));
            if (!strcmp(cn, "CGuiEditScroll")) {     // summary: every line is followed by an empty one
                std::string h; int run = 0;
                for (char ch : t + "x") {
                    if (ch == '\n') { run++; continue; }
                    for (int k = 0; k < (run + 1) / 2; k++) h += '\n';
                    run = 0; h += ch;
                }
                h.pop_back();
                t = h;
            }
            if (!t.empty()) m.help = Squeeze(Reflow(t));
        } else if (strstr(cn, "Edit") && !strstr(cn, "Scroll") && strstr(nm.c_str(), "Name")) {
            m.points = Strip(Gui_Text(c.obj)); m.pointsCaption = "Name";
        } else if (nm == "Title") {
            m.title = Strip(Gui_Text(c.obj));
        } else if (!strncmp(nm.c_str(), "SpellLevel", 10)) {
            m.levels.push_back(c.obj);
            void* ts = F<void*>(c.obj, 0xa8);
            if (ts && Readable(ts, 0x18)) m.level = F<int>(ts, 0x10);
        } else if (strstr(cn, "Label") && (strstr(nm.c_str(), "Pts") || strstr(nm.c_str(), "Points") || strstr(nm.c_str(), "Remain"))) {
            if (!strstr(nm.c_str(), "Title")) m.points = Strip(Gui_Text(c.obj));
        } else if ((strstr(cn, "Button") || strstr(cn, "button")) && !EndsWith(nm, "UpButton") && !EndsWith(nm, "DownButton")) {
            m.buttons.push_back({c.obj, nm});
        }
    }
    if (void* fn = Gui_Find(panel, "FirstNameEdit")) {
        void* ln = Gui_Find(panel, "LastNameEdit");
        m.points = Strip(Gui_Text(fn)) + (ln ? " " + Strip(Gui_Text(ln)) : std::string());
        m.pointsCaption = "Name";
    }
    if (hasAbilities) m.lists.insert(m.lists.begin(), ab);
    if (!radios.rows.empty()) m.lists.insert(m.lists.begin(), radios);
    if (m.pointsCaption == "Name") {}
    else if (m.cls == "CCharacterSkillsPanel") m.pointsCaption = "Skill points left";
    else if (m.kind == K_SPELL) m.pointsCaption = "Spells left to choose";
    else if (m.kind == K_FEAT) m.pointsCaption = "Feats left to choose";
    else if (hasAbilities) m.pointsCaption = "Points left";
    else m.pointsCaption = "Remaining";
    m.menu = m.lists.empty() && m.cls == "CCharacterGenerationPanel";
    if (m.menu) BuildSteps(panel);
    if (m.cls == "CCharacterPortraitPanel") BuildPortraits(m);
    g_m = m;
}

void* Button(const char* name) {
    for (auto& b : g_m.buttons) if (_stricmp(b.second.c_str(), name) == 0) return b.first;
    return nullptr;
}
void* ButtonAny(std::initializer_list<const char*> names) {
    for (auto n : names) if (void* b = Button(n)) return b;
    return nullptr;
}
bool Press(void* b) {
    if (!b || !Gui_Enabled(b)) return false;
    return Gui_Click(b);
}

List* Focus() { return g_focus < (int)g_m.lists.size() ? &g_m.lists[g_focus] : nullptr; }

void SelectRow(List& l, int i) {
    if (i < 0 || i >= (int)l.rows.size()) return;
    g_cursor[std::min(g_focus, 3)] = i;
    if ((l.box || l.radio) && l.sel != i) { Gui_Click(l.rows[i].obj); l.sel = i; }
}

// description: the game's help box, or the 2DA text of the selected entry
std::string Describe() {
    List* l = Focus();
    int cur = g_cursor[std::min(g_focus, 3)];
    int id = l && cur < (int)l->rows.size() ? l->rows[cur].id : -1;
    std::string d;
    if (id >= 0) {
        const char* tbl = nullptr; const char* col = nullptr;
        switch (g_m.kind) {
        case K_CLASS: tbl = "classes"; col = "Description"; break;
        case K_SKILL: tbl = "skills"; col = "Description"; break;
        case K_FEAT: tbl = "feat"; col = "DESCRIPTION"; break;
        case K_SPELL: tbl = "spells"; col = "SpellDesc"; break;
        case K_DOMAIN: tbl = "domains"; col = "Description"; break;
        case K_RACE: tbl = "racialtypes"; col = "Description"; break;
        case K_SCHOOL: tbl = "spellschools"; col = "Description"; break;
        case K_FAMILIAR: tbl = "hen_familiar"; col = "DESCRIPTION"; break;
        case K_PACKAGE: tbl = "packages"; col = "Description"; break;
        default: break;
        }
        if (tbl && (l->box || (l->radio && g_m.kind == K_SCHOOL))) { int s = TDA(tbl).Int(id, col, -1); if (s >= 0) d = Tlk((uint32_t)s); }
    }
    if (d.empty()) d = g_m.help;
    return d;
}

uint32_t RowIcon(const Row& r) {
    if (r.id < 0) return 0;
    const char* tbl = nullptr; const char* col = nullptr;
    switch (g_m.kind) {
    case K_CLASS: tbl = "classes"; col = "Icon"; break;
    case K_SKILL: tbl = "skills"; col = "Icon"; break;
    case K_FEAT: tbl = "feat"; col = "ICON"; break;
    case K_SPELL: tbl = "spells"; col = "IconResRef"; break;
    case K_DOMAIN: tbl = "domains"; col = "Icon"; break;
    case K_RACE: tbl = "racialtypes"; col = "Icon"; break;
    default: return 0;
    }
    const std::string& ic = TDA(tbl).Get(r.id, col);
    return ic.empty() ? 0 : Res_Texture(ic.c_str());
}

bool SingleChoice() {        // one list of options and an OK button: A confirms
    if (g_m.lists.size() != 1) return false;
    const List& l = g_m.lists[0];
    if (!l.box && !l.radio) return false;
    for (auto& r : l.rows) if (r.up || r.action) return false;
    return Button("OkButton") != nullptr && !Button("AddButton") && g_m.kind != K_INFO;
}
}  // namespace

void CharPanel_Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    fHide = (PanelFn)GetProcAddress(exe, "?Hide@CGuiPanel@@QEAAXXZ");
    fUnHide = (PanelFn)GetProcAddress(exe, "?UnHide@CGuiPanel@@QEAAXXZ");
}

bool CharPanel_Handled(void* panel) { return panel && panel == g_panel; }
bool CharPanel_KeepHidden(void* p) {
    if (!p || !UI_PadPresent() || p == g_classic || !Readable(p, 0xc0)) return false;
    const char* cn = ClassName(p);
    return cn && Handled(cn);
}
bool CharPanel_Active() { return g_panel != nullptr; }

void CharPanel_Frame() {
    static bool wasConnected = false;
    if (wasConnected && !UI_PadPresent()) {       // pad gone: give every character panel back to the mouse
        for (void* q : Gui_LivePanels())
            if (Readable(q, 0xc0) && Handled(ClassName(q))) Gui_ShowPanel(q);
    }
    wasConnected = UI_PadPresent();
    void* p = UI_PadPresent() ? CurrentPanel() : nullptr;
    if (g_hidden && g_hidden != p) {
        // panel changed or pad gone: show the previous one again if it is still alive
        bool alive = false;
        for (void* q : Gui_LivePanels()) if (q == g_hidden) alive = true;
        if (alive && !CharPanel_KeepHidden(g_hidden)) Gui_ShowPanel(g_hidden);
        g_hidden = nullptr;
    }
    if (p != g_panel) {
        g_panel = p;
        g_focus = 0; g_cursor[0] = g_cursor[1] = g_cursor[2] = g_cursor[3] = 0; g_menuSel = 0;
        g_openTime = g_pad.now;
        if (p && g_pad.now - g_lastShown > 0.3) g_fadeStart = g_pad.now;
        g_waitRelease = true;
        if (p) {
            Log("charpanel: %s", ClassName(p)); Build(p);
            for (int i = 0; i < (int)g_m.lists.size() && i < 4; i++) if (g_m.lists[i].sel >= 0) g_cursor[i] = g_m.lists[i].sel;
            for (int i = 0; g_m.menu && i < (int)g_steps.size(); i++) if (g_steps[i].next) g_menuSel = i;
            g_picCur = g_m.picSel > 0 ? g_m.picSel : 0;
        }
    }
    if (!p) return;
    g_lastShown = g_pad.now;
    Build(p);
    Gui_HidePanel(p); g_hidden = p;
}

bool CharPanel_Input() {
    if (!g_panel) return false;
    if (g_waitRelease) { if (g_pad.held & ~(PB_LT | PB_RT)) return true; g_waitRelease = false; }
    if (g_pad.now - g_openTime < 0.2) return true;
    Model& m = g_m;
    if (m.menu) {
        int n = (int)g_steps.size();
        int dir = (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) ? -1 : (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) ? 1 : 0;
        if (n && dir) {          // enabled steps only
            int s2 = g_menuSel;
            for (int k = 0; k < n; k++) { s2 = (s2 + dir + n) % n; if (g_steps[s2].en) { g_menuSel = s2; break; } }
        }
        if (n && g_pad.Pressed(PB_A)) Press(g_steps[std::min(g_menuSel, n - 1)].b);
        if (g_pad.Pressed(PB_START)) Press(Button("PlayButton"));
        if (g_pad.Pressed(PB_BACK)) Press(Button("ResetButton"));
        if (g_pad.Pressed(PB_B)) Press(Button("CancelButton"));
        return true;
    }
    if (m.portraits) {
        const int COLS = 7;
        int n = (int)m.pics.size(), c = g_picCur;
        if (g_pad.Repeat(PB_DRIGHT) || g_pad.Repeat(PB_LRIGHT)) c++;
        if (g_pad.Repeat(PB_DLEFT) || g_pad.Repeat(PB_LLEFT)) c--;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) c += COLS;
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) c -= COLS;
        if (g_pad.Repeat(PB_RB)) c += COLS * 3;
        if (g_pad.Repeat(PB_LB)) c -= COLS * 3;
        if (n) { c = std::max(0, std::min(n - 1, c)); if (c != g_picCur) { g_picCur = c; PickPortrait(c); } }
        if (n && (g_pad.Pressed(PB_A) || g_pad.Pressed(PB_START))) { if (m.picSel != g_picCur) PickPortrait(g_picCur); Press(Button("OkButton")); }
        if (g_pad.Pressed(PB_B)) Press(Button("CancelButton"));
        return true;
    }
    // list navigation
    if (m.lists.size() > 1) {
        if (g_pad.Pressed(PB_RB)) g_focus = (g_focus + 1) % (int)m.lists.size();
        if (g_pad.Pressed(PB_LB)) g_focus = (g_focus + (int)m.lists.size() - 1) % (int)m.lists.size();
    }
    List* l = Focus();
    if (l && l->rows.empty())        // focused list emptied (last entry moved): go to one with entries
        for (int i = 0; i < (int)m.lists.size(); i++) if (!m.lists[i].rows.empty()) { g_focus = i; l = Focus(); break; }
    int& cur = g_cursor[std::min(g_focus, 3)];
    if (l && !l->rows.empty()) {
        int n = (int)l->rows.size();
        if (cur >= n) cur = n - 1;
        int nc = cur;
        if (g_pad.Repeat(PB_DUP) || g_pad.Repeat(PB_LUP)) nc = (cur + n - 1) % n;
        if (g_pad.Repeat(PB_DDOWN) || g_pad.Repeat(PB_LDOWN)) nc = (cur + 1) % n;
        if (nc != cur) SelectRow(*l, nc);
        Row& r = l->rows[cur];
        if (r.up) {
            if (g_pad.Repeat(PB_DRIGHT) || g_pad.Repeat(PB_LRIGHT) || g_pad.Repeat(PB_A, 0.4, 0.12)) { if (l->box && l->sel != cur) SelectRow(*l, cur); Press(r.up); }
            if (g_pad.Repeat(PB_DLEFT) || g_pad.Repeat(PB_LLEFT) || g_pad.Repeat(PB_X, 0.4, 0.12)) { if (l->box && l->sel != cur) SelectRow(*l, cur); Press(r.down); }
        } else if (g_pad.Pressed(PB_A)) {
            if (r.action) { SelectRow(*l, cur); Press(r.action); }
            else if (Button("AddButton")) { SelectRow(*l, cur); Press(Button("AddButton")); }
            else if (SingleChoice() && r.enabled) { SelectRow(*l, cur); Press(Button("OkButton")); }
            else SelectRow(*l, cur);
        }
        if (!r.up && g_pad.Pressed(PB_X) && Button("RemoveButton")) { SelectRow(*l, cur); Press(Button("RemoveButton")); }
    }
    // spell levels
    if (!m.levels.empty()) {
        int nl = (int)m.levels.size();
        int dir = g_pad.Repeat(PB_RT) ? 1 : g_pad.Repeat(PB_LT) ? -1 : 0;
        if (dir) {
            int lv = m.level;
            for (int k = 0; k < nl; k++) {
                lv = (lv + dir + nl) % nl;
                if (Gui_Enabled(m.levels[lv])) { Gui_Click(m.levels[lv]); g_cursor[std::min(g_focus, 3)] = 0; break; }
            }
        }
    }
    if (fabsf(g_pad.ry) > 0.2f) g_descScroll -= g_pad.ry * px(700.0f) * (float)g_pad.dt;
    if (g_pad.Pressed(PB_Y)) Press(ButtonAny({"RecommendButton", "RecommendedButton"}));
    if (g_pad.Pressed(PB_X) && Button("ConfigurePckg")) Press(Button("ConfigurePckg"));
    if (g_pad.Pressed(PB_X) && m.kind == K_INFO) {           // type a name / description: the game's own panel
        g_classic = g_panel;
        return true;
    }
    if (g_pad.Pressed(PB_BACK)) Press(ButtonAny({"ResetButton", "GenNameButton", "RandomButton"}));
    if (g_pad.Pressed(PB_START) || (m.lists.empty() && g_pad.Pressed(PB_A))) Press(Button("OkButton"));
    if (g_pad.Pressed(PB_B)) Press(Button("CancelButton"));
    return true;
}

namespace {
const char* Caption(const std::string& name) {
    static std::string s;
    s = name;
    for (const char* suf : {"ListBox", "Box", "List", "ButtonBox"}) if (EndsWith(s, suf)) { s.resize(s.size() - strlen(suf)); break; }
    if (s == "Avail") return "Available";
    if (s == "Known" || s == "Chosen" || s == "Selected") return "Chosen";
    if (s == "Skills" || s == "SkillsButton") return "Skills";
    if (s.empty()) return "Options";
    return s.c_str();
}

void DrawList(List& l, int li, float x, float y, float w, float h, float a) {
    bool focus = li == g_focus;
    RoundRect(x, y, w, h, px(12), Col(8, 7, 6, (int)(120 * a)));
    if (focus && g_m.lists.size() > 1) RoundRectBorder(x, y, w, h, px(12), px(1), Theme::Line.alpha(0.6f * a));
    float rowH = px(54);
    int n = (int)l.rows.size();
    int vis = std::max(1, (int)(h / rowH));
    int cur = std::min(g_cursor[std::min(li, 3)], std::max(0, n - 1));
    static int scroll[4];
    int& sc = scroll[std::min(li, 3)];
    if (cur < sc) sc = cur;
    if (cur >= sc + vis) sc = cur - vis + 1;
    if (sc > n - vis) sc = n - vis;
    if (sc < 0) sc = 0;
    PushClip(x, y, w, h);
    for (int i = sc; i < n && i < sc + vis + 1; i++) {
        const Row& r = l.rows[i];
        float ry = y + (i - sc) * rowH;
        bool sel = focus && i == cur;
        bool chosen = (l.box || l.radio) && i == l.sel;
        UIK::RowSel(x + px(4), ry + px(2), w - px(8), rowH - px(4), sel);
        if (chosen && !sel) RoundRectBorder(x + px(4), ry + px(2), w - px(8), rowH - px(4), px(8), px(1), Theme::GoldDim.alpha(0.7f * a));
        float tx = x + px(16);
        uint32_t ic = RowIcon(r);
        if (ic) { ImageRounded(ic, tx, ry + px(7), rowH - px(14), rowH - px(14), px(6), (r.enabled ? Col(255, 255, 255) : Col(100, 100, 100)).alpha(a)); tx += rowH; }
        Col tc = !r.enabled ? Theme::TextFaint : sel ? Col(255, 244, 222) : Theme::Text;
        float vw = r.up ? px(120) : px(20);
        TextFit(FONT_BODY, px(21), tx, ry + (rowH - LineHeight(FONT_BODY, px(21))) * 0.5f, x + w - vw - tx, r.text.c_str(), tc.alpha(a));
        if (r.up) {
            float cx = x + w - px(64);
            Col ac = sel ? Theme::Gold : Theme::GoldDim;
            Triangle(cx - px(40), ry + rowH * 0.5f, cx - px(30), ry + rowH * 0.5f - px(8), cx - px(30), ry + rowH * 0.5f + px(8), ac.alpha(sel ? a : 0.4f * a));
            Triangle(cx + px(40), ry + rowH * 0.5f, cx + px(30), ry + rowH * 0.5f - px(8), cx + px(30), ry + rowH * 0.5f + px(8), ac.alpha(sel ? a : 0.4f * a));
            TextCentered(FONT_BODY_BOLD, px(21), cx, ry + (rowH - LineHeight(FONT_BODY_BOLD, px(21))) * 0.5f, r.value.c_str(), Theme::Text.alpha(a));
        } else if (r.action && sel) {
            UIK::Glyph(GL_A, x + w - px(46), ry + (rowH - px(26)) * 0.5f, px(26), a);
        }
    }
    if (n == 0) TextCentered(FONT_BODY, px(19), x + w * 0.5f, y + px(24), "Nothing here", Theme::TextFaint.alpha(a));
    PopClip();
    UIK::Scrollbar(x + w - px(8), y + px(6), h - px(12), sc, vis, n);
}
}  // namespace

void CharPanel_Draw() {
    static int lastShade = 0;
    if (!g_panel) {
        // the game swaps panels between steps: keep the backdrop for a moment so nothing blinks through
        double gone = g_pad.now - g_lastShown;
        if (gone < 0.25 && lastShade) {
            float k = (float)fmax(0.0, 1.0 - gone / 0.25);
            R::Rect(0, 0, (float)W(), (float)H(), Col(0, 0, 0, (int)(lastShade * k)));
        }
        return;
    }
    Model& m = g_m;
    float t = (float)fmin(1.0, (g_pad.now - g_fadeStart) / 0.16);
    float a = 1 - (1 - t) * (1 - t);
    lastShade = (int)((m.menu || !Player() ? 235 : 140) * a);
    R::Rect(0, 0, (float)W(), (float)H(), Col(0, 0, 0, lastShade));
    float w = fminf(px(1600), W() - px(60));
    float h = fminf(px(880), H() - px(150));
    float x = (W() - w) * 0.5f, y = (H() - h) * 0.5f - px(30);
    UIK::Panel(x, y, w, h, px(20), a);
    std::string title = m.title;
    for (auto& c : title) if (c == '\n') c = ' ';
    // the level-up title packs "name   class (level)   step" into one line
    std::string step = title, who;
    size_t p = title.rfind("   ");
    if (p != std::string::npos) { step = Strip(title.substr(p)); who = Strip(title.substr(0, p)); }
    if (step.empty()) step = m.cls == "CCharacterLevelUpSummaryPanel" ? "Level up summary" : m.cls == "CCharacterGenerationPanel" ? "New character" : "Character";
    Text(FONT_TITLE, px(30), x + px(36), y + px(24), step.c_str(), Theme::Gold.alpha(a));
    if (!who.empty()) {
        std::string ww; for (size_t i = 0; i < who.size(); i++) { if (who[i] == ' ' && i + 1 < who.size() && who[i + 1] == ' ') { while (i + 1 < who.size() && who[i + 1] == ' ') i++; ww += "  ·  "; } else ww += who[i]; }
        TextRight(FONT_BODY, px(20), x + w - px(36), y + px(32), ww.c_str(), Theme::TextDim.alpha(a));
    }
    float top = y + px(80);
    RoundRect(x + px(30), top - px(8), w - px(60), px(1), 0, Theme::Line.alpha(0.5f * a));
    float bottom = y + h - px(24);
    float lw = m.lists.empty() ? 0 : (m.lists.size() > 1 ? (w - px(36) * 2 - px(30) * 2) * 0.62f : (w - px(36) * 2 - px(30)) * 0.5f);
    float lx = x + px(36);
    float ly = top + px(10);
    // points + spell levels
    if (!m.points.empty()) {
        std::string pt = m.pointsCaption + ": " + m.points;
        Text(FONT_BODY_BOLD, px(21), lx, ly, pt.c_str(), Theme::Friendly.alpha(a));
    }
    if (!m.levels.empty()) {
        float gx = lx + px(330);
        UIK::Glyph(GL_LT, gx, ly - px(2), px(26), a); gx += px(46);
        for (int i = 0; i < (int)m.levels.size(); i++) {
            bool on = i == m.level, en = Gui_Enabled(m.levels[i]);
            char b[8]; snprintf(b, sizeof b, "%d", i);
            RoundRect(gx, ly - px(4), px(38), px(32), px(8), on ? Theme::Sel1.alpha(a) : Col(8, 7, 6, (int)(110 * a)));
            TextCentered(FONT_TITLE, px(19), gx + px(19), ly, b, (on ? Theme::Gold : en ? Theme::Text : Theme::TextFaint).alpha(a));
            gx += px(44);
        }
        UIK::Glyph(GL_RT, gx + px(4), ly - px(2), px(26), a);
    }
    if (!m.points.empty() || !m.levels.empty()) ly += px(44);
    if (m.portraits) {
        const int COLS = 7;
        float pw = px(320), ph = px(500);
        float gx = lx + pw + px(40), gw = x + w - px(36) - gx;
        float cw = (gw - px(12) * (COLS - 1)) / COLS, chh = cw * 1.56f;
        float gy = ly, gh = bottom - px(56) - gy;
        int n = (int)m.pics.size();
        int visRows = std::max(1, (int)((gh + px(12)) / (chh + px(12))));
        static int top = 0;
        int row = g_picCur / COLS;
        if (row < top) top = row;
        if (row >= top + visRows) top = row - visRows + 1;
        std::string big = g_picCur < n ? m.pics[g_picCur] : std::string();
        uint32_t bt = big.empty() ? 0 : Res_Texture((big + "h").c_str());
        if (!bt && !big.empty()) bt = Res_Texture((big + "l").c_str());
        RoundRect(lx, ly, pw, ph, px(12), Col(20, 17, 14, (int)(200 * a)));
        if (bt) ImageRounded(bt, lx, ly, pw, ph, px(12), Col(255, 255, 255).alpha(a), 0, 0, 1, 0.78125f);
        RoundRectBorder(lx, ly, pw, ph, px(12), px(1.5f), Theme::Line.alpha(a));
        char cnt[32]; snprintf(cnt, sizeof cnt, "%d / %d", g_picCur + 1, n);
        TextCentered(FONT_BODY, px(18), lx + pw * 0.5f, ly + ph + px(14), cnt, Theme::TextDim.alpha(a));
        PushClip(gx - px(4), gy - px(4), gw + px(8), gh + px(8));
        int loads = 0;
        for (int i = top * COLS; i < n && i < (top + visRows + 1) * COLS; i++) {
            int r = i / COLS - top, c = i % COLS;
            float cx = gx + c * (cw + px(12)), cy = gy + r * (chh + px(12));
            std::string ref = m.pics[i] + "m";
            static std::map<std::string, uint32_t> got;
            uint32_t t = 0;
            auto gi = got.find(ref);
            if (gi != got.end()) t = gi->second;
            else if (loads < 4) { t = got[ref] = Res_Texture(ref.c_str()); loads++; }   // a few new textures per frame
            RoundRect(cx, cy, cw, chh, px(8), Col(20, 17, 14, (int)(200 * a)));
            if (t) ImageRounded(t, cx, cy, cw, chh, px(8), (i == g_picCur ? Col(255, 255, 255) : Col(200, 200, 200)).alpha(a), 0, 0, 1, 0.78125f);   // portrait art fills the top 100/128 of the texture
            if (i == g_picCur) { RoundRectBorder(cx - px(3), cy - px(3), cw + px(6), chh + px(6), px(10), px(3), Theme::Gold.alpha(a)); }
            else if (i == m.picSel) RoundRectBorder(cx, cy, cw, chh, px(8), px(1.5f), Theme::GoldDim.alpha(a));
        }
        PopClip();
        int rows = (n + COLS - 1) / COLS;
        UIK::Scrollbar(x + w - px(24), gy, gh, top, visRows, rows);
    } else if (m.menu) {
        // creation hub: the steps in order, the character so far on the right
        float rowH = px(64), cw = px(600);
        for (int i = 0; i < (int)g_steps.size(); i++) {
            const Step& st = g_steps[i];
            float ry = ly + i * rowH;
            bool sel = i == g_menuSel;
            UIK::RowSel(lx, ry, cw, rowH - px(8), sel);
            float cy = ry + (rowH - px(8)) * 0.5f, cx = lx + px(34);
            bool play = i == (int)g_steps.size() - 1;
            if (st.done) {
                Circle(cx, cy, px(15), Theme::Friendly.alpha(0.85f * a));
                Line(cx - px(7), cy, cx - px(2), cy + px(6), px(3), Col(20, 16, 12).alpha(a));
                Line(cx - px(2), cy + px(6), cx + px(8), cy - px(6), px(3), Col(20, 16, 12).alpha(a));
            } else if (!play) {
                char nb[8]; snprintf(nb, sizeof nb, "%d", i + 1);
                Ring(cx, cy, px(15), px(1.5f), (st.en ? Theme::Gold : Theme::Line).alpha(a));
                TextCentered(FONT_TITLE, px(17), cx, cy - px(11), nb, (st.en ? Theme::Gold : Theme::TextFaint).alpha(a));
            }
            Col tc = !st.en ? Theme::TextFaint : sel ? Col(255, 244, 222) : st.done ? Theme::TextDim : Theme::Text;
            Text(FONT_TITLE, px(24), lx + px(68), cy - px(15), st.label.c_str(), tc.alpha(a));
            if (st.next && st.en) {
                const char* tag = play ? "Ready" : "Next";
                float tw = TextWidth(FONT_BODY_BOLD, px(16), tag) + px(20);
                RoundRect(lx + cw - tw - px(16), cy - px(13), tw, px(26), px(13), Theme::Sel1.alpha(a));
                TextCentered(FONT_BODY_BOLD, px(16), lx + cw - tw * 0.5f - px(16), cy - px(10), tag, Theme::Gold.alpha(a));
            }
        }
        // the character so far
        float dx = lx + cw + px(40), dw = x + w - px(36) - dx, dy = ly, dh = bottom - px(56) - dy;
        RoundRect(dx, dy, dw, dh, px(12), Col(8, 7, 6, (int)(120 * a)));
        void* cre = GenCreature(m.panel);
        auto done = [&](const char* n) { for (size_t i = 0; i < g_steps.size(); i++) if (!strcmp(STEPS[i], n) || (g_steps[i].b == Button(n))) return g_steps[i].done; return false; };
        if (cre) {
            Inv::CharInfo c = Inv::Character(cre);
            void* st = F<void*>(cre, OFS::Cre_Stats);
            float px0 = dx + px(28), py = dy + px(26);
            float pw = px(180), ph = px(280);
            uint32_t pt = done("PortraitButton") ? GD::PortraitTex(cre, 'h') : 0;
            if (pt) ImageRounded(pt, px0, py, pw, ph, px(10), Col(255, 255, 255).alpha(a), 0, 0, 1, 0.78125f);
            else {
                RoundRect(px0, py, pw, ph, px(10), Col(20, 17, 14, (int)(200 * a)));
                TextCentered(FONT_BODY, px(17), px0 + pw * 0.5f, py + ph * 0.5f - px(10), "No portrait yet", Theme::TextFaint.alpha(a));
            }
            RoundRectBorder(px0, py, pw, ph, px(10), px(1), Theme::Line.alpha(a));
            float tx = px0 + pw + px(30), ty = py;
            std::string nm = done("CustomizeButton") ? c.name : std::string();
            Text(FONT_TITLE, px(30), tx, ty, nm.empty() ? "Unnamed hero" : nm.c_str(), (nm.empty() ? Theme::TextFaint : Theme::Gold).alpha(a));
            ty += px(54);
            auto field = [&](const char* k, const std::string& v) {
                Text(FONT_BODY, px(18), tx, ty, k, Theme::TextDim.alpha(a));
                Text(FONT_BODY_BOLD, px(21), tx + px(150), ty - px(2), v.empty() ? "-" : v.c_str(), (v.empty() ? Theme::TextFaint : Theme::Text).alpha(a));
                ty += px(38);
            };
            field("Gender", done("GenderButton") && st ? (F<uint8_t>(st, 0x68) == 1 ? "Female" : "Male") : "");
            field("Race", done("RaceButton") ? c.race : "");
            std::string cls = c.classes; size_t pp = cls.find(" ("); if (pp != std::string::npos) cls.resize(pp);
            field("Class", done("ClassButton") ? cls : "");
            std::string al;
            if (done("AlignButton") && st) {
                int lc = F<short>(st, 0xc6), ge = F<short>(st, 0xc8);
                std::string l1 = lc >= 70 ? "Lawful" : lc <= 30 ? "Chaotic" : "Neutral";
                std::string g1 = ge >= 70 ? "Good" : ge <= 30 ? "Evil" : "Neutral";
                al = (l1 == "Neutral" && g1 == "Neutral") ? "True Neutral" : l1 + " " + g1;
            }
            field("Alignment", al);
            // abilities
            float ay2 = py + ph + px(36);
            static const char* AN[6] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
            float bw = (dw - px(56) - px(10) * 5) / 6;
            bool abd = done("AbilitiesButton");
            for (int k = 0; k < 6; k++) {
                float bx = px0 + k * (bw + px(10));
                RoundRect(bx, ay2, bw, px(84), px(10), Col(20, 17, 14, (int)(200 * a)));
                TextCentered(FONT_BODY_BOLD, px(16), bx + bw * 0.5f, ay2 + px(10), AN[k], Theme::GoldDim.alpha(a));
                char v[8]; snprintf(v, sizeof v, "%d", c.ab[k]);
                TextCentered(FONT_TITLE, px(30), bx + bw * 0.5f, ay2 + px(36), abd ? v : "-", (abd ? Theme::Text : Theme::TextFaint).alpha(a));
            }
            // what the selected step does
            static const char* WHAT[] = {"Choose whether your character is male or female.",
                "Your race grants ability adjustments, favoured class and special abilities.",
                "Pick the portrait shown for your character.", "Your class decides your skills, feats and powers.",
                "Your moral and ethical outlook. Some classes require certain alignments.",
                "Distribute points among the six ability scores.", "Choose a package (recommended skills, feats and spells), or customize them yourself.",
                "Name, appearance, voice and description.", "Enter the module with this character."};
            if (g_menuSel >= 0 && g_menuSel < (int)g_steps.size()) {
                int wi = 0;
                for (int k = 0; k < 9; k++) if (Gui_Find(m.panel, STEPS[k]) == g_steps[g_menuSel].b) wi = k;
                TextWrap(FONT_BODY, px(19), px0, ay2 + px(116), dw - px(56), WHAT[wi], Theme::TextDim.alpha(a), 0, 1.3f);
            }
        }
    } else {
        float lh = bottom - px(56) - ly;
        int nl = (int)m.lists.size();
        float gap = px(20);
        float each = nl > 0 ? (lw - gap * (nl - 1)) / nl : 0;
        for (int i = 0; i < nl; i++) {
            float bx = lx + i * (each + gap);
            if (nl > 1) {
                Text(FONT_TITLE, px(19), bx, ly, Caption(m.lists[i].name), (i == g_focus ? Theme::Gold : Theme::GoldDim).alpha(a));
                DrawList(m.lists[i], i, bx, ly + px(32), each, lh - px(32), a);
            } else DrawList(m.lists[i], i, bx, ly, each, lh, a);
        }
        // description
        float dx = nl ? lx + lw + px(30) : lx, dw = x + w - px(36) - dx;
        if (!nl) { dw = fminf(dw, px(900)); dx = x + (w - dw) * 0.5f; }
        float dy = top + px(10);
        float dh = bottom - px(56) - dy;
        RoundRect(dx, dy, dw, dh, px(12), Col(8, 7, 6, (int)(120 * a)));
        std::string d = Describe();
        if (d != g_descKey) { g_descKey = d; g_descScroll = 0; }
        PushClip(dx + px(10), dy + px(10), dw - px(20), dh - px(20));
        float full = TextWrap(FONT_BODY, px(19), 0, 0, dw - px(44), d.c_str(), Theme::Text, 0, 1.25f, true);
        float maxS = fmaxf(0.0f, full - dh + px(30));
        g_descScroll = fminf(fmaxf(g_descScroll, 0.0f), maxS);
        TextWrap(FONT_BODY, px(19), dx + px(20), dy + px(16) - g_descScroll, dw - px(44), d.c_str(), Theme::Text.alpha(0.92f * a), 0, 1.25f);
        PopClip();
        if (maxS > 0) { float t2 = dh * dh / full; RoundRect(dx + dw - px(10), dy + (dh - t2) * (g_descScroll / maxS), px(4), t2, px(2), Theme::GoldDim.alpha(a)); }
    }
    // action row (buttons the pad maps)
    float ay = bottom - px(40), ax = x + px(36);
    auto hint = [&](PadGlyph g, void* b, const char* fallback) {
        if (!b) return;
        std::string lab = Strip(Gui_Text(b));
        if (lab.empty()) lab = fallback;
        float al = Gui_Enabled(b) ? a : 0.35f * a;
        ax += UIK::Hint(g, lab.c_str(), ax, ay, px(28), al) + px(30);
    };
    if (m.portraits) {
    } else if (m.menu) {
        hint(GL_START, Button("PlayButton"), "Play");
        hint(GL_BACK, Button("ResetButton"), "Reset");
    } else {
        hint(GL_START, Button("OkButton"), "OK");
        hint(GL_Y, ButtonAny({"RecommendButton", "RecommendedButton"}), "Recommended");
        hint(GL_X, Button("ConfigurePckg"), "Configure");
        if (m.kind == K_INFO) ax += UIK::Hint(GL_X, "Type name / description", ax, ay, px(28), a) + px(30);
        hint(GL_BACK, ButtonAny({"ResetButton", "GenNameButton", "RandomButton"}), "Reset");
    }
    hint(GL_B, Button("CancelButton"), "Cancel");
    // hint bar
    HintItem hb[6]; int n = 0;
    if (m.portraits) {
        HintItem ph[] = {{GL_DPAD, "Browse"}, {GL_LB, "Page"}, {GL_A, "Choose"}};
        UI_DrawHints(ph, 3, a);
        return;
    }
    if (!m.lists.empty() || m.menu) hb[n++] = {GL_DPAD, SingleChoice() ? "Browse" : "Choose"};
    List* l = Focus();
    bool plusMinus = l && !l->rows.empty() && l->rows[std::min(g_cursor[std::min(g_focus, 3)], (int)l->rows.size() - 1)].up;
    if (m.menu) hb[n++] = {GL_A, "Select"};
    else if (m.lists.empty()) hb[n++] = {GL_A, "Accept"};
    else if (plusMinus) { hb[n++] = {GL_A, "Raise"}; hb[n++] = {GL_X, "Lower"}; }
    else if (SingleChoice()) hb[n++] = {GL_A, "Choose"};
    else if (l && !l->rows.empty() && l->rows[std::min(g_cursor[std::min(g_focus, 3)], (int)l->rows.size() - 1)].action)
        hb[n++] = {GL_A, g_focus == 0 ? "Add" : "Remove"};
    else hb[n++] = {GL_A, Button("RemoveButton") ? "Add" : "Select"};
    if (Button("RemoveButton")) hb[n++] = {GL_X, "Remove"};
    if (m.lists.size() > 1) hb[n++] = {GL_LB, "Switch list"};
    if (!m.menu) hb[n++] = {GL_RS, "Scroll"};
    UI_DrawHints(hb, n, a);
}
