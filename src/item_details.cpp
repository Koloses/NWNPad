// NWNPad - item details pane (icon, name, type, examine text) shared by the loot and store windows
#include "item_details.h"
#include "describe.h"
#include "uikit.h"
#include "render.h"
#include "pad.h"
#include <cmath>

using namespace R;

void UI_ItemDetails(const InvItem& it, float x, float y, float w, float h, float a, ItemDetailsState& st, const std::string& extra) {
    RoundRect(x, y, w, h, px(12), Col(8, 7, 6, (int)(120 * a)));
    float is = px(76);
    float ix = x + px(16), iy = y + px(16);
    RoundRect(ix, iy, is + px(12), is + px(12), px(10), Col(0, 0, 0, (int)(90 * a)));
    Inv::DrawIcon(it, ix + px(6), iy + px(6), is, a);
    float tx = ix + is + px(26), tw = x + w - px(16) - tx, ty = iy;
    ty += TextWrap(FONT_TITLE, px(22), tx, ty, tw, it.name.c_str(), (it.identified ? Theme::Gold : Col(150, 170, 220)).alpha(a)) + px(4);
    Text(FONT_BODY, px(17), tx, ty, it.typeName.c_str(), Theme::TextDim.alpha(a)); ty += px(24);
    std::string facts = extra;
    if (!it.identified) facts = facts.empty() ? "Unidentified" : facts + "  -  Unidentified";
    if (!facts.empty()) { TextFit(FONT_BODY_BOLD, px(17), tx, ty, tw, facts.c_str(), Theme::Friendly.alpha(a)); ty += px(24); }
    float cy = fmaxf(iy + is + px(24), ty + px(6));
    RoundRect(x + px(16), cy, w - px(32), px(1), 0, Theme::Line.alpha(0.4f * a));
    cy += px(10);
    float dh = y + h - px(12) - cy;
    if (st.id != it.id) { st.id = it.id; st.scroll = 0; }
    if (fabsf(g_pad.ry) > 0.2f) st.scroll -= g_pad.ry * px(700.0f) * (float)g_pad.dt;
    const std::string* d = Describe_Get(it.id);
    PushClip(x + px(8), cy, w - px(16), dh);
    if (d) {
        float full = TextWrap(FONT_BODY, px(17), 0, 0, w - px(40), d->c_str(), Theme::Text, 0, 1.2f, true);
        float maxS = fmaxf(0.0f, full - dh + px(8));
        st.scroll = fminf(fmaxf(st.scroll, 0.0f), maxS);
        TextWrap(FONT_BODY, px(17), x + px(18), cy - st.scroll, w - px(40), d->c_str(), Theme::Text.alpha(0.92f * a), 0, 1.2f);
        if (maxS > 0) {
            float t2 = dh * dh / full;
            RoundRect(x + w - px(12), cy + (dh - t2) * (st.scroll / maxS), px(4), t2, px(2), Theme::GoldDim.alpha(a));
        }
    } else {
        Text(FONT_BODY, px(17), x + px(18), cy, "...", Theme::TextFaint.alpha(a));
    }
    PopClip();
}
