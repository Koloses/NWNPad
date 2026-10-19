// NWNPad - gamepad HUD: hero frame, party, target frame, action cluster, message feed
#include "hud.h"
#include "hub_pages.h"
#include "uikit.h"
#include "game.h"
#include "gamedata.h"
#include "control.h"
#include "hooks.h"
#include "pad.h"
#include "config.h"
#include "text.h"
#include "res.h"
#include "log.h"
#include <windows.h>
#include <deque>
#include <string>
#include <cmath>
#include <cstring>

using namespace G;
using namespace R;

namespace {
const uintptr_t VA_AppendToMsgBuffer = 0x1406f3f40;
void (*o_AppendToMsgBuffer)(void* gig, void* msg, uint32_t col, uint32_t p4, int p5, void* resref) = nullptr;

struct FeedLine { std::string text; Col col; double t; };
std::deque<FeedLine> g_feed;

void hk_AppendToMsgBuffer(void* gig, void* msg, uint32_t col, uint32_t p4, int p5, void* rr) {
    std::string s = ExoStr(msg);
    if (!s.empty()) {
        Col c = Theme::Text;
        // colour = first inline <cRGB> token of the raw message
        const char* raw = msg ? *(const char**)msg : nullptr;
        uint32_t rl = msg ? *(const uint32_t*)((const char*)msg + 8) : 0;
        if (raw) {
            for (uint32_t i = 0; i + 5 < rl; i++) {
                if (raw[i] == '<' && raw[i + 1] == 'c' && raw[i + 5] == '>') {
                    c = Col((uint8_t)raw[i + 2], (uint8_t)raw[i + 3], (uint8_t)raw[i + 4]);
                    int mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
                    if (mx < 200 && mx > 0) { float k = 200.0f / mx; c = Col((int)fminf(255, c.r * k), (int)fminf(255, c.g * k), (int)fminf(255, c.b * k)); }
                    if (mx == 0) c = Theme::Text;
                    c = Lerp(c, Col(255, 255, 255), 0.25f);
                    break;
                }
            }
        }
        // keep the feed readable: skip very long multi-line messages
        if (s.size() > 220) s = s.substr(0, 217) + "...";
        for (auto& ch : s) if (ch == '\n') ch = ' ';
        g_feed.push_back({s, c, g_pad.now});
        while (g_feed.size() > 40) g_feed.pop_front();
    }
    o_AppendToMsgBuffer(gig, msg, col, p4, p5, rr);
}

// game HUD panels that the gamepad HUD replaces
struct HidePanel { const char* cls; };
const char* kHideClasses[] = {"CPanelQuickBar", "CPanelPartyBar", "CPanelActionQueue", "CPanelPlayerStateBar",
                              "CGuiInGameChatWindow", "CGuiInGameChatPrompt", "CGuiCompassPanel"};
typedef void (*PanelFn)(void*);
PanelFn fToggleOff = nullptr, fUnToggleOff = nullptr;
bool g_panelsHidden = false;

void ForEachGameHudPanel(bool hide) {
    void* in = Internal();
    void* gig = in ? F<void*>(in, OFS::In_GuiInGame) : nullptr;
    if (!gig || !fToggleOff) return;
    for (int off = 8; off <= 0xd0; off += 8) {
        void* p = F<void*>(gig, off);
        if (!p) continue;
        const char* cn = ClassName(p);
        if (!cn[0]) continue;
        for (const char* h : kHideClasses) {
            if (strcmp(cn, h) == 0) {
                uint32_t fl = F<uint32_t>(p, 0xb0);
                if (hide && !(fl & 4)) fToggleOff(p);
                if (!hide && (fl & 4) && fUnToggleOff) fUnToggleOff(p);
            }
        }
    }
}

void LogGameHud() {
    static bool done = false;
    if (done) return;
    void* in = Internal();
    void* gig = in ? F<void*>(in, OFS::In_GuiInGame) : nullptr;
    if (!gig || !Player()) return;
    done = true;
    for (int off = 8; off <= 0x3c0; off += 8) {
        void* p = F<void*>(gig, off);
        const char* cn = p ? ClassName(p) : "";
        if (cn[0] && g_cfg.devDumpArea) Log("GIG+0x%03x %s flags=%x", off, cn, F<uint32_t>(p, 0xb0));
    }
}

const char* ActionVerb(void* t) {
    switch (Type(t)) {
    case OT_CREATURE:
        if (GD::IsDead(t)) return "Search";
        if (GD::Reaction(t) == 2) return "Attack";
        return "Talk";
    case OT_ITEM: return "Pick up";
    case OT_PLACEABLE: return "Use";
    case OT_DOOR: return "Open";
    }
    return "Interact";
}

Col ReactionCol(void* t) {
    if (Type(t) != OT_CREATURE) return Theme::Gold;
    int r = GD::Reaction(t);
    return r == 2 ? Theme::Hostile : r == 1 ? Theme::Friendly : Theme::Neutral;
}

void PortraitCircle(void* cre, float cx, float cy, float r, float hpRing, bool big) {
    GD::HP hp = GD::CreatureHP(cre);
    Shadow(cx - r, cy - r, 2 * r, 2 * r, r, px(14), Col(0, 0, 0, 170));
    Circle(cx, cy, r, Col(16, 14, 12, 235));
    uint32_t tex = GD::PortraitTex(cre, big ? 'm' : 's');
    float ir = r - hpRing - px(3);
    if (tex) {
        // NWN portraits are 1:2 textures with the picture in the upper ~80%: show the face square
        ImageRounded(tex, cx - ir, cy - ir, 2 * ir, 2 * ir, ir, Col(), 0.0f, 0.04f, 1.0f, 0.54f);
    }
    // hp ring
    float f = hp.known && hp.max > 0 ? fmaxf(0.0f, fminf(1.0f, (float)hp.cur / hp.max)) : 1.0f;
    Ring(cx, cy, r - hpRing * 0.5f, hpRing, Theme::HPBack);
    Col hc = f > 0.5f ? Lerp(Col(214, 170, 60), Theme::Friendly, (f - 0.5f) * 2) : Lerp(Theme::HP, Col(214, 170, 60), f * 2);
    if (f > 0.001f) Arc(cx, cy, r, hpRing, 0, 6.2831853f * f, hc);
    Ring(cx, cy, r, px(1.5f), Theme::Line);
}
}  // namespace

void HUD_InstallHooks() {
    o_AppendToMsgBuffer = (decltype(o_AppendToMsgBuffer))Hook_Create(VA_AppendToMsgBuffer, (void*)hk_AppendToMsgBuffer, "AppendToMsgBuffer");
    HMODULE exe = GetModuleHandleA(nullptr);
    fToggleOff = (PanelFn)GetProcAddress(exe, "?ToggleOff@CGuiPanel@@QEAAXXZ");
    fUnToggleOff = (PanelFn)GetProcAddress(exe, "?UnToggleOff@CGuiPanel@@QEAAXXZ");
}

void HUD_UpdatePanels(bool hudVisible) {
    LogGameHud();
    bool want = g_cfg.newHud && hudVisible;
    if (want) { ForEachGameHudPanel(true); g_panelsHidden = true; }
    else if (g_panelsHidden) { ForEachGameHudPanel(false); g_panelsHidden = false; }
}

static void DrawHero(void* pl) {
    float r = px(70);
    float cx = px(40) + r, cy = H() - px(40) - r;
    PortraitCircle(pl, cx, cy, r, px(7), true);
    // name plate
    std::string name = GD::ObjName(Id(pl));
    GD::HP hp = GD::CreatureHP(pl);
    float nx = cx + r + px(18);
    float ny = cy - px(40);
    Text(FONT_TITLE, px(30), nx, ny, name.c_str(), Theme::Gold);
    char buf[64];
    snprintf(buf, sizeof buf, "Level %d", GD::Level(pl));
    Text(FONT_BODY, px(20), nx, ny + px(36), buf, Theme::TextDim);
    // hp bar
    float bw = px(300), bh = px(14);
    float by = ny + px(66);
    float f = hp.max > 0 ? (float)hp.cur / hp.max : 0;
    float ft = hp.max > 0 ? (float)(hp.cur + hp.temp) / hp.max : 0;
    UIK::Bar(nx, by, bw, bh, f, Theme::HP, Theme::HPBack, ft, Theme::HPTemp);
    snprintf(buf, sizeof buf, "%d / %d", hp.cur + hp.temp, hp.max);
    Text(FONT_BODY_BOLD, px(18), nx + bw + px(10), by - px(4), buf, Theme::Text);
    // party members
    auto party = GD::PartyMembers(pl);
    float px0 = nx, py0 = by + px(30);
    float pr = px(26);
    int i = 0;
    for (void* m : party) {
        if (i >= 5) break;
        float mx = px0 + pr + i * (pr * 2 + px(12));
        PortraitCircle(m, mx, py0 + pr, pr, px(4), false);
        i++;
    }
}

static void DrawTarget(void* pl) {
    void* t = g_ctl.softTarget ? GetObj(g_ctl.softTarget) : nullptr;
    if (!t) return;
    // world marker above the target
    Vec3 p = Pos(t);
    p.z += (Type(t) == OT_CREATURE) ? 2.2f : 1.4f;
    float sx, sy;
    Col rc = ReactionCol(t);
    if (!g_ctl.radialOpen && WorldToScreen(p, sx, sy) && sx > 0 && sx < 1 && sy > 0 && sy < 1) {
        float x = sx * W(), y = sy * H();
        float bob = sinf((float)g_pad.now * 4.0f) * px(4);
        float s = px(13);
        y += bob;
        Triangle(x - s, y - s * 1.4f, x + s, y - s * 1.4f, x, y, Col(0, 0, 0, 140));
        Triangle(x - s * 0.8f, y - s * 1.3f, x + s * 0.8f, y - s * 1.3f, x, y - s * 0.15f, rc);
        if (g_ctl.locked) Ring(x, y - s * 0.75f, s * 1.6f, px(2), rc.alpha(0.8f));
    }
    // top frame
    std::string name = GD::ObjName(Id(t));
    float w = px(460), h = px(96);
    float x = (W() - w) * 0.5f, y = px(26);
    UIK::Panel(x, y, w, h, px(14), 0.92f);
    float tw = TextWidth(FONT_TITLE, px(26), name.c_str());
    if (tw > w - px(40)) TextFit(FONT_TITLE, px(26), x + px(20), y + px(10), w - px(40), name.c_str(), rc);
    else Text(FONT_TITLE, px(26), x + (w - tw) * 0.5f, y + px(10), name.c_str(), rc);
    float by = y + px(48);
    if (Type(t) == OT_CREATURE) {
        GD::HP hp = GD::CreatureHP(t);
        if (hp.known) UIK::Bar(x + px(24), by, w - px(48), px(10), (float)hp.cur / hp.max, rc.r > 200 && rc.g < 120 ? Theme::HP : Theme::Friendly, Theme::HPBack);
    }
    // action prompt
    const char* verb = g_ctl.inActionTargetMode ? "Use on target" : ActionVerb(t);
    float hw = UIK::HintWidth(GL_A, verb, px(26)) + px(30) + UIK::HintWidth(GL_A, "Hold: Actions", px(22));
    float hx = x + (w - hw) * 0.5f;
    if (g_ctl.inActionTargetMode) hx += (UIK::HintWidth(GL_A, "Hold: Actions", px(22)) + px(30)) * 0.5f;
    hx += UIK::Hint(GL_A, verb, hx, by + px(14), px(26)) + px(30);
    if (!g_ctl.inActionTargetMode) Text(FONT_BODY, px(17), hx, by + px(19), "hold for actions", Theme::TextDim);
    (void)pl;
}

static void DrawSlot(int bank, int slot, PadGlyph g, float cx, float cy, float r, bool active) {
    void* b = Control_QuickButton(bank, slot);
    uint8_t type = b ? F<uint8_t>(b, 0x140) : 0;
    char ref[17] = {};
    if (b && type) memcpy(ref, (char*)b + 0x11c, 16);
    bool disabled = b ? (F<uint32_t>(b, 8) & 1) != 0 : true;
    Circle(cx, cy, r, Col(14, 12, 10, 225));
    uint32_t tex = ref[0] ? Res_Texture(ref) : 0;
    if (tex) ImageRounded(tex, cx - r * 0.78f, cy - r * 0.78f, r * 1.56f, r * 1.56f, r * 0.78f, disabled ? Col(110, 110, 110, 200) : Col(255, 255, 255, 255));
    else if (!type) TextCentered(FONT_BODY, r * 0.6f, cx, cy - r * 0.42f, "-", Theme::TextFaint);
    Ring(cx, cy, r, px(2), active ? Theme::LineStrong : Theme::Line);
    UIK::Glyph(g, cx + r * 0.42f, cy + r * 0.42f, r * 0.62f);
}

static void DrawActions() {
    float r = px(36);
    float gap = px(78);
    float cx = W() - px(70) - gap - r, cy = H() - px(70) - gap - r;
    bool lt = g_pad.Down(PB_LT), rt = g_pad.Down(PB_RT);
    int bank = g_ctl.quickBank;
    // layer: base = X/Y/B slots 1-3 (A = interact), LT = 4-7, RT = 8-11
    int sX, sY, sB, sA;
    if (lt && !rt) { sX = 3; sY = 4; sB = 5; sA = 6; }
    else if (rt && !lt) { sX = 7; sY = 8; sB = 9; sA = 10; }
    else { sX = 0; sY = 1; sB = 2; sA = -1; }
    Shadow(cx - gap - r * 1.5f, cy - gap - r * 1.5f, (gap + r * 1.5f) * 2, (gap + r * 1.5f) * 2, gap + r, px(30), Col(0, 0, 0, 90));
    DrawSlot(bank, sY, GL_Y, cx, cy - gap, r, true);
    DrawSlot(bank, sX, GL_X, cx - gap, cy, r, true);
    DrawSlot(bank, sB, GL_B, cx + gap, cy, r, true);
    if (sA >= 0) DrawSlot(bank, sA, GL_A, cx, cy + gap, r, true);
    else {
        Circle(cx, cy + gap, r, Col(14, 12, 10, 225));
        Ring(cx, cy + gap, r, px(2), Theme::Line);
        TextCentered(FONT_BODY_BOLD, px(16), cx, cy + gap - px(10), "Interact", Theme::TextDim);
        UIK::Glyph(GL_A, cx + r * 0.42f, cy + gap + r * 0.42f, r * 0.62f);
    }
    // layer indicators
    float ly = cy - gap - r - px(52);
    float lx = cx - gap - r;
    UIK::Glyph(GL_LT, lx, ly, px(26), lt ? 1.0f : 0.55f);
    UIK::Glyph(GL_RT, cx + gap + r - px(38), ly, px(26), rt ? 1.0f : 0.55f);
    static const char* banks[] = {"I", "II", "III"};
    char buf[32]; snprintf(buf, sizeof buf, "Set %s", banks[bank % 3]);
    TextCentered(FONT_TITLE, px(20), cx, ly + px(2), buf, Theme::Gold);
    UIK::Glyph(GL_DUP, cx - px(13), ly + px(28), px(26), 0.7f);
}

static void DrawFeed() {
    float x = px(40), y = H() * 0.42f;
    float w = px(620);
    int shown = 0;
    float lh = LineHeight(FONT_BODY, px(19)) * 1.05f;
    // newest at the bottom
    std::vector<const FeedLine*> lines;
    for (auto it = g_feed.rbegin(); it != g_feed.rend() && lines.size() < 7; ++it) {
        if (g_pad.now - it->t > 10.0) break;
        lines.push_back(&*it);
    }
    float cy = y;
    for (int i = (int)lines.size() - 1; i >= 0; i--) {
        const FeedLine* l = lines[i];
        float age = (float)(g_pad.now - l->t);
        float a = age > 8.0f ? 1.0f - (age - 8.0f) / 2.0f : 1.0f;
        float h = TextWrap(FONT_BODY, px(19), 0, 0, w, l->text.c_str(), l->col, 0, 1.05f, true);
        RoundRect(x - px(10), cy - px(2), w + px(20), h + px(4), px(6), Col(0, 0, 0, (int)(110 * a)));
        TextWrap(FONT_BODY, px(19), x, cy, w, l->text.c_str(), l->col.alpha(a), 0, 1.05f);
        cy += h + px(6);
        shown++;
    }
    (void)lh;
}

void HUD_Draw() {
    if (!g_cfg.newHud) return;
    void* pl = Player();
    if (!pl) return;
    DrawHero(pl);
    DrawTarget(pl);
    DrawActions();
    DrawFeed();
    if (g_cfg.minimap) { float ms = px(230); Map_DrawMinimap(W() - px(40) - ms, px(40), ms, 0.95f); }
    if (GD::IsPaused()) {
        float w = px(260), h = px(56);
        UIK::Panel((W() - w) * 0.5f, H() * 0.28f, w, h, px(28));
        TextCentered(FONT_TITLE, px(30), W() * 0.5f, H() * 0.28f + px(10), "PAUSED", Theme::Gold);
    }
}
