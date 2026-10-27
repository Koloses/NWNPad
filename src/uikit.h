// NWNPad UI kit: theme colours and common widgets
#pragma once
#include "render.h"
#include <string>
#include <cstdint>

namespace Theme {
const Col Bg0(20, 17, 14, 236), Bg1(11, 10, 9, 242);       // panel gradient
const Col BgSoft(16, 14, 12, 200);
const Col Line(196, 160, 96, 150), LineStrong(222, 184, 112, 230);
const Col Gold(232, 196, 120), GoldDim(170, 140, 92);
const Col Text(238, 230, 214), TextDim(170, 160, 142), TextFaint(120, 112, 100);
const Col Sel0(120, 92, 46, 210), Sel1(82, 62, 30, 210);    // selected row gradient
const Col HP(196, 54, 44), HPBack(54, 18, 16, 220), HPTemp(210, 170, 70);
const Col Mana(80, 140, 220);
const Col Friendly(110, 190, 110), Neutral(214, 196, 120), Hostile(222, 76, 60);
const Col BtnA(92, 172, 64), BtnB(206, 58, 48), BtnX(56, 122, 214), BtnY(232, 176, 36);
const Col Shadow(0, 0, 0, 150);
}

enum PadGlyph { GL_A, GL_B, GL_X, GL_Y, GL_LB, GL_RB, GL_LT, GL_RT, GL_LS, GL_RS, GL_DPAD, GL_DUP, GL_DDOWN, GL_DLEFT, GL_DRIGHT, GL_START, GL_BACK, GL_L3, GL_R3 };

namespace UIK {
// framed panel with shadow
void Panel(float x, float y, float w, float h, float radius = -1, float alpha = 1.0f);
void PanelTitle(float x, float y, float w, const char* title);
// controller button glyph (size = height)
float Glyph(PadGlyph g, float x, float y, float size, float alpha = 1.0f);
// glyph + label, returns width
float Hint(PadGlyph g, const char* label, float x, float y, float size, float alpha = 1.0f);
float HintWidth(PadGlyph g, const char* label, float size);
// bar with background
void Bar(float x, float y, float w, float h, float frac, Col fill, Col back, float frac2 = 0, Col fill2 = Col(0, 0, 0, 0));
// selectable list row background
void RowSel(float x, float y, float w, float h, bool selected, float pulse = 0);
// scrollbar
void Scrollbar(float x, float y, float h, int first, int visible, int total);
// map PadGlyph from a pad button
PadGlyph GlyphFor(uint32_t padBtn);
// "!" of someone with a quest for you, centred on (x, y); dim: may not be offered yet
void QuestBang(float x, float y, float h, bool dim, float alpha = 1.0f);
double Now();
}

// hint bar at the bottom of the screen: pairs of (glyph,label)
struct HintItem { PadGlyph g; const char* label; };
void UI_DrawHints(const HintItem* items, int n, float alpha = 1.0f);
