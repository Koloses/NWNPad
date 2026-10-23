// NWNPad - gamepad version of the game's "timed event" window (resting progress).
//
// The server starts a timed event (resting) with CNWCMessage::HandleServerToPlayerGuiTimingEvent_Info,
// which calls CGuiInGame::StartPlayerTimedEventGui(duration ms, text strref): a CGuiPanelPlayerTimedEvent
// at CGuiInGame +0xf0 with a status bar and an X button. Its Update counts the elapsed seconds (+0x2e4)
// up to the duration (+0x2e0). We show our own window with the same progress and send the X button's
// action (HandleCancelButton: CNWCMessage::SendPlayerToServerInput_CancelGuiTimingEvent) for B.
#include "rest.h"
#include "ui.h"
#include "uikit.h"
#include "render.h"
#include "game.h"
#include "hooks.h"
#include "text.h"
#include "res.h"
#include "pad.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <string>

using namespace G;
using namespace R;

namespace {
const uintptr_t VA_StartTimedEvent = 0x1406f9910;      // CGuiInGame::StartPlayerTimedEventGui(this, ms, strref)
const size_t GIG_TimedEvent = 0xf0;
const size_t TE_Duration = 0x2e0, TE_Elapsed = 0x2e4;  // seconds (float)
const uintptr_t VA_TimedEvent_Cancel = 0x1406f5450;    // CGuiPanelPlayerTimedEvent::HandleCancelButton(this)

typedef void (*Start_t)(void*, unsigned, unsigned);
Start_t o_Start = nullptr;
std::string g_text;
double g_shown = -10;

void hk_Start(void* gig, unsigned ms, unsigned strref) {
    g_text = Tlk(strref);
    g_shown = g_pad.now;
    Log("timed event: %u ms '%s'", ms, g_text.c_str());
    o_Start(gig, ms, strref);
}

void* Panel() {
    void* in = Internal();
    void* gig = in ? F<void*>(in, OFS::In_GuiInGame) : nullptr;
    void* p = gig ? F<void*>(gig, GIG_TimedEvent) : nullptr;
    return p && Readable(p, 0x2e8) ? p : nullptr;
}
}  // namespace

void Rest_InstallHooks() {
    o_Start = (Start_t)Hook_Create(VA_StartTimedEvent, (void*)hk_Start, "CGuiInGame::StartPlayerTimedEventGui");
}

bool Rest_Active() { return UI_PadPresent() && Panel() != nullptr; }

bool Rest_KeepHidden(void* panel) { return panel && panel == Panel() && UI_PadPresent(); }

bool Rest_Input() {
    void* p = Panel();
    if (!p || !UI_PadPresent()) return false;
    if (g_pad.Pressed(PB_B)) {
        Log("timed event: cancel");
        ((void (*)(void*))A(VA_TimedEvent_Cancel))(p);
        return true;     // B is "stop" here, not the quick slot
    }
    return false;        // moving / acting still works (the game cancels resting itself)
}

void Rest_Draw() {
    void* p = Panel();
    if (!p) return;
    float dur = F<float>(p, TE_Duration), el = F<float>(p, TE_Elapsed);
    float f = dur > 0 ? fminf(1.0f, fmaxf(0.0f, el / dur)) : 0.0f;
    float a = fminf(1.0f, (float)(g_pad.now - g_shown) * 5.0f);
    float w = px(520), h = px(176);
    float x = (W() - w) * 0.5f, y = H() * 0.62f;
    UIK::Panel(x, y, w, h, px(16), a);
    std::string t = g_text.empty() ? std::string("Resting") : g_text;
    TextCentered(FONT_TITLE, px(28), x + w * 0.5f, y + px(20), t.c_str(), Theme::Gold.alpha(a));
    float bx = x + px(36), bw = w - px(72), by = y + px(70);
    UIK::Bar(bx, by, bw, px(16), f, Theme::Gold, Theme::HPBack, 0, Col());
    char b[64];
    float left = fmaxf(0.0f, dur - el);
    snprintf(b, sizeof b, "%d%%   %d s left", (int)(f * 100 + 0.5f), (int)ceilf(left));
    TextCentered(FONT_BODY, px(18), x + w * 0.5f, by + px(26), b, Theme::TextDim.alpha(a));
    float hw = UIK::HintWidth(GL_B, "Stop", px(26));
    UIK::Hint(GL_B, "Stop", x + (w - hw) * 0.5f, y + h - px(44), px(26), a);
}
