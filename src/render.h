// NWNPad overlay renderer (OpenGL 3.3, drawn on top of the game right before SwapBuffers)
#pragma once
#include <cstdint>

struct Col {
    uint8_t r, g, b, a;
    Col() : r(255), g(255), b(255), a(255) {}
    Col(int R, int G, int B, int A = 255) : r((uint8_t)R), g((uint8_t)G), b((uint8_t)B), a((uint8_t)A) {}
    Col alpha(float f) const { Col c = *this; c.a = (uint8_t)(a * (f < 0 ? 0 : f > 1 ? 1 : f)); return c; }
    uint32_t u32() const { return r | (g << 8) | (b << 16) | ((uint32_t)a << 24); }
};
inline Col Lerp(Col a, Col b, float t) {
    return Col((int)(a.r + (b.r - a.r) * t), (int)(a.g + (b.g - a.g) * t), (int)(a.b + (b.b - a.b) * t), (int)(a.a + (b.a - a.a) * t));
}

struct Rect { float x, y, w, h; };

enum FontId { FONT_BODY = 0, FONT_BODY_BOLD = 1, FONT_TITLE = 2, FONT_COUNT };

namespace R {
bool Begin(int screenW, int screenH);    // returns false if GL not ready
void End();
int W(); int H();
float S();                               // ui scale (1.0 at 1080p)
float px(float v);                       // scaled pixels
// shapes (anti-aliased, SDF)
void RoundRect(float x, float y, float w, float h, float radius, Col c);
void RoundRectGrad(float x, float y, float w, float h, float radius, Col top, Col bottom);
void RoundRectBorder(float x, float y, float w, float h, float radius, float thick, Col c);
void Shadow(float x, float y, float w, float h, float radius, float blur, Col c);
void Circle(float cx, float cy, float r, Col c);
void Ring(float cx, float cy, float r, float thick, Col c);
void Rect(float x, float y, float w, float h, Col c);
void Line(float x0, float y0, float x1, float y1, float thick, Col c);
void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, Col c);
void Arc(float cx, float cy, float r, float thick, float a0, float a1, Col c);   // radians, 0 = up, clockwise
// textures
uint32_t CreateTexture(int w, int h, const void* rgba);     // returns GL name
uint32_t CreateTextureCompressed(int w, int h, int fmt, const void* data, int size, int mips, const int* mipSizes);
void DeleteTexture(uint32_t t);
void Image(uint32_t tex, float x, float y, float w, float h, Col tint = Col(), float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
// image rotated by quarter turns (texture corners shifted clockwise)
void ImageRot(uint32_t tex, float x, float y, float w, float h, int quarter, Col tint = Col());
void ImageRounded(uint32_t tex, float x, float y, float w, float h, float radius, Col tint = Col(), float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
// clipping
void PushClip(float x, float y, float w, float h);
void PopClip();
// text (strings are UTF-8)
float Text(FontId f, float size, float x, float y, const char* s, Col c, int maxChars = -1);
float TextWidth(FontId f, float size, const char* s, int maxChars = -1);
float LineHeight(FontId f, float size);
// wrapped text, returns used height; align 0 left, 1 center, 2 right
float TextWrap(FontId f, float size, float x, float y, float w, const char* s, Col c, int align = 0, float lineSpacing = 1.15f, bool measureOnly = false);
void TextCentered(FontId f, float size, float cx, float y, const char* s, Col c);
void TextRight(FontId f, float size, float rx, float y, const char* s, Col c);
// truncated with ellipsis to fit width
void TextFit(FontId f, float size, float x, float y, float w, const char* s, Col c);
}
