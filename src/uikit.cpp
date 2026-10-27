#include "uikit.h"
#include "pad.h"
#include <cmath>
#include <cstring>

using namespace R;

namespace UIK {
double Now() { return g_pad.now; }

void Panel(float x, float y, float w, float h, float radius, float a) {
    float r = radius < 0 ? px(14) : radius;
    Shadow(x, y + px(4), w, h, r, px(22), Theme::Shadow.alpha(a));
    RoundRectGrad(x, y, w, h, r, Theme::Bg0.alpha(a), Theme::Bg1.alpha(a));
    RoundRectBorder(x, y, w, h, r, px(1.5f), Theme::Line.alpha(a));
    // subtle inner top highlight
    RoundRectBorder(x + px(3), y + px(3), w - px(6), h - px(6), r - px(3), px(1), Col(255, 230, 180, (int)(18 * a)));
}

void PanelTitle(float x, float y, float w, const char* title) {
    float sz = px(30);
    Text(FONT_TITLE, sz, x, y, title, Theme::Gold);
    float lw = TextWidth(FONT_TITLE, sz, title);
    float ly = y + LineHeight(FONT_TITLE, sz) + px(2);
    RoundRect(x, ly, lw + px(40), px(2), px(1), Theme::LineStrong);
    RoundRect(x + lw + px(40), ly, w - lw - px(40), px(1), px(0.5f), Theme::Line.alpha(0.5f));
}

static void Letter(float cx, float cy, float size, const char* s, Col c) {
    float fs = size * 0.62f;
    float tw = TextWidth(FONT_BODY_BOLD, fs, s);
    float lh = LineHeight(FONT_BODY_BOLD, fs);
    Text(FONT_BODY_BOLD, fs, cx - tw * 0.5f, cy - lh * 0.52f, s, c);
}

float Glyph(PadGlyph g, float x, float y, float size, float a) {
    float r = size * 0.5f, cx = x + r, cy = y + r;
    Col dark(18, 16, 14, (int)(235 * a));
    switch (g) {
    case GL_A: case GL_B: case GL_X: case GL_Y: {
        Col c = g == GL_A ? Theme::BtnA : g == GL_B ? Theme::BtnB : g == GL_X ? Theme::BtnX : Theme::BtnY;
        Circle(cx, cy, r, dark);
        Ring(cx, cy, r, size * 0.09f, c.alpha(a));
        Letter(cx, cy, size, g == GL_A ? "A" : g == GL_B ? "B" : g == GL_X ? "X" : "Y", c.alpha(a));
        return size;
    }
    case GL_LB: case GL_RB: case GL_LT: case GL_RT: {
        float w = size * 1.45f;
        bool trig = g == GL_LT || g == GL_RT;
        float rad = trig ? size * 0.42f : size * 0.22f;
        RoundRect(x, y, w, size, rad, dark);
        RoundRectBorder(x, y, w, size, rad, size * 0.08f, Col(220, 214, 200, (int)(220 * a)));
        Letter(x + w * 0.5f, cy, size * 0.82f, g == GL_LB ? "LB" : g == GL_RB ? "RB" : g == GL_LT ? "LT" : "RT", Col(236, 230, 220, (int)(255 * a)));
        return w;
    }
    case GL_LS: case GL_RS: case GL_L3: case GL_R3: {
        Circle(cx, cy, r, dark);
        Ring(cx, cy, r, size * 0.08f, Col(220, 214, 200, (int)(220 * a)));
        Ring(cx, cy, r * 0.55f, size * 0.06f, Col(220, 214, 200, (int)(150 * a)));
        Letter(cx, cy, size * 0.7f, (g == GL_LS || g == GL_L3) ? "L" : "R", Col(236, 230, 220, (int)(255 * a)));
        return size;
    }
    case GL_DPAD: case GL_DUP: case GL_DDOWN: case GL_DLEFT: case GL_DRIGHT: {
        float t = size * 0.34f;
        Col base(200, 194, 182, (int)(200 * a)), hi(255, 214, 140, (int)(255 * a));
        RoundRect(cx - t / 2, y, t, size, t * 0.25f, dark);
        RoundRect(x, cy - t / 2, size, t, t * 0.25f, dark);
        RoundRect(cx - t / 2 + 1, y + 1, t - 2, size - 2, t * 0.2f, base.alpha(0.35f));
        RoundRect(x + 1, cy - t / 2 + 1, size - 2, t - 2, t * 0.2f, base.alpha(0.35f));
        if (g == GL_DUP) RoundRect(cx - t / 2, y, t, size * 0.45f, t * 0.25f, hi);
        if (g == GL_DDOWN) RoundRect(cx - t / 2, y + size * 0.55f, t, size * 0.45f, t * 0.25f, hi);
        if (g == GL_DLEFT) RoundRect(x, cy - t / 2, size * 0.45f, t, t * 0.25f, hi);
        if (g == GL_DRIGHT) RoundRect(x + size * 0.55f, cy - t / 2, size * 0.45f, t, t * 0.25f, hi);
        return size;
    }
    case GL_START: case GL_BACK: {
        float w = size * 1.2f;
        RoundRect(x, y + size * 0.15f, w, size * 0.7f, size * 0.35f, dark);
        RoundRectBorder(x, y + size * 0.15f, w, size * 0.7f, size * 0.35f, size * 0.07f, Col(220, 214, 200, (int)(220 * a)));
        Col l(236, 230, 220, (int)(255 * a));
        if (g == GL_START) {   // three lines (menu)
            for (int i = -1; i <= 1; i++) RoundRect(x + w * 0.3f, cy + i * size * 0.13f - size * 0.03f, w * 0.4f, size * 0.06f, size * 0.03f, l);
        } else {               // two overlapping squares (view)
            RoundRectBorder(x + w * 0.28f, cy - size * 0.17f, size * 0.28f, size * 0.24f, size * 0.04f, size * 0.05f, l);
            RoundRectBorder(x + w * 0.42f, cy - size * 0.07f, size * 0.28f, size * 0.24f, size * 0.04f, size * 0.05f, l);
        }
        return w;
    }
    }
    return size;
}

float HintWidth(PadGlyph g, const char* label, float size) {
    float gw = (g == GL_LB || g == GL_RB || g == GL_LT || g == GL_RT) ? size * 1.45f : (g == GL_START || g == GL_BACK) ? size * 1.2f : size;
    return gw + size * 0.35f + TextWidth(FONT_BODY, size * 0.72f, label);
}

float Hint(PadGlyph g, const char* label, float x, float y, float size, float a) {
    float w = Glyph(g, x, y, size, a);
    float fs = size * 0.72f;
    float lh = LineHeight(FONT_BODY, fs);
    Text(FONT_BODY, fs, x + w + size * 0.35f, y + (size - lh) * 0.5f, label, Theme::Text.alpha(a));
    return w + size * 0.35f + TextWidth(FONT_BODY, fs, label);
}

void Bar(float x, float y, float w, float h, float f, Col fill, Col back, float f2, Col fill2) {
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    float r = h * 0.5f;
    RoundRect(x, y, w, h, r, back);
    if (f2 > 0) RoundRect(x, y, fmaxf(h, w * fminf(f2, 1.0f)), h, r, fill2);
    if (f > 0) {
        RoundRectGrad(x, y, fmaxf(h, w * f), h, r, Lerp(fill, Col(255, 255, 255), 0.18f), fill);
    }
    RoundRectBorder(x, y, w, h, r, px(1), Col(0, 0, 0, 120));
}

void RowSel(float x, float y, float w, float h, bool sel, float pulse) {
    if (!sel) return;
    float r = px(8);
    RoundRectGrad(x, y, w, h, r, Theme::Sel0, Theme::Sel1);
    float a = 0.65f + 0.35f * sinf((float)Now() * 5.0f);
    RoundRectBorder(x, y, w, h, r, px(1.5f), Theme::LineStrong.alpha(a));
    (void)pulse;
}

void Scrollbar(float x, float y, float h, int first, int vis, int total) {
    if (total <= vis || total <= 0) return;
    float w = px(4);
    RoundRect(x, y, w, h, w / 2, Col(255, 255, 255, 25));
    float th = fmaxf(px(24), h * vis / total);
    float ty = y + (h - th) * first / (float)(total - vis);
    RoundRect(x, ty, w, th, w / 2, Theme::GoldDim);
}

PadGlyph GlyphFor(uint32_t b) {
    switch (b) {
    case PB_A: return GL_A; case PB_B: return GL_B; case PB_X: return GL_X; case PB_Y: return GL_Y;
    case PB_LB: return GL_LB; case PB_RB: return GL_RB; case PB_LT: return GL_LT; case PB_RT: return GL_RT;
    case PB_DUP: return GL_DUP; case PB_DDOWN: return GL_DDOWN; case PB_DLEFT: return GL_DLEFT; case PB_DRIGHT: return GL_DRIGHT;
    case PB_START: return GL_START; case PB_BACK: return GL_BACK;
    }
    return GL_A;
}
}  // namespace UIK

void UI_DrawHints(const HintItem* items, int n, float a) {
    float size = px(30);
    float gap = px(28);
    float total = 0;
    for (int i = 0; i < n; i++) total += UIK::HintWidth(items[i].g, items[i].label, size) + (i ? gap : 0);
    float h = size + px(20);
    float x = (W() - total) * 0.5f, y = H() - h - px(18);
    RoundRect(x - px(24), y, total + px(48), h, h * 0.5f, Col(12, 10, 9, (int)(200 * a)));
    RoundRectBorder(x - px(24), y, total + px(48), h, h * 0.5f, px(1), Theme::Line.alpha(0.6f * a));
    for (int i = 0; i < n; i++) {
        x += UIK::Hint(items[i].g, items[i].label, x, y + px(10), size, a) + gap;
    }
}

void UIK::QuestBang(float x, float y, float h, bool dim, float alpha) {
    using namespace R;
    Col c = dim ? Col(170, 160, 140) : Col(255, 196, 40);
    float w = h * 0.34f;
    // dark outline, then the mark: a tapering bar and a dot
    float o = fmaxf(1.0f, h * 0.09f);
    RoundRect(x - w * 0.5f - o, y - h * 0.5f - o, w + o * 2, h * 0.66f + o * 2, w * 0.5f, Col(20, 14, 6, (int)(230 * alpha)));
    Circle(x, y + h * 0.36f, w * 0.5f + o, Col(20, 14, 6, (int)(230 * alpha)));
    RoundRect(x - w * 0.5f, y - h * 0.5f, w, h * 0.66f, w * 0.5f, c.alpha((dim ? 0.75f : 1.0f) * alpha));
    Circle(x, y + h * 0.36f, w * 0.5f, c.alpha((dim ? 0.75f : 1.0f) * alpha));
}
