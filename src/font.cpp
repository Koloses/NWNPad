// NWNPad text rendering: signed-distance-field glyph atlas built with stb_truetype
#include "render.h"
#include "log.h"
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstring>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

extern "C" const unsigned char font_cinzel[], font_lora[], font_lora_bold[];
extern "C" const unsigned int font_cinzel_len, font_lora_len, font_lora_bold_len;

void R_PushGlyph(GLuint tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Col c);

namespace {
const int ATLAS = 2048;
const float BASE = 42.0f;     // glyph raster size in atlas
const int PAD = 6;
struct Glyph { float u0, v0, u1, v1; float xoff, yoff, w, h; float adv; bool ok; };
struct FontData {
    stbtt_fontinfo info;
    float scale;              // for BASE
    float ascent, descent, lineGap;
    std::unordered_map<uint32_t, Glyph> glyphs;
    std::unordered_map<uint64_t, float> kern;   // (prev << 32 | cp) -> kerning at BASE size (GPOS lookups are slow)
};
FontData g_fonts[FONT_COUNT];
GLuint g_tex = 0;
std::vector<uint8_t> g_alpha;
int g_penX = 1, g_penY = 1, g_rowH = 0;
bool g_dirty = false;
int g_dx0 = ATLAS, g_dy0 = ATLAS, g_dx1 = 0, g_dy1 = 0;   // atlas rectangle changed since the last upload
bool g_uploaded = false;                                  // the whole atlas went up once

bool AddGlyph(FontData& f, uint32_t cp) {
    Glyph g{};
    int gi = stbtt_FindGlyphIndex(&f.info, (int)cp);
    int adv, lsb; stbtt_GetGlyphHMetrics(&f.info, gi, &adv, &lsb);
    g.adv = adv * f.scale;
    if (gi == 0 && cp != ' ') { f.glyphs[cp] = g; return false; }
    int w = 0, h = 0, xo = 0, yo = 0;
    unsigned char* sdf = stbtt_GetGlyphSDF(&f.info, f.scale, gi, PAD, 128, 128.0f / PAD, &w, &h, &xo, &yo);
    if (sdf && w > 0 && h > 0) {
        if (g_penX + w + 1 >= ATLAS) { g_penX = 1; g_penY += g_rowH + 1; g_rowH = 0; }
        if (g_penY + h + 1 >= ATLAS) { stbtt_FreeSDF(sdf, nullptr); f.glyphs[cp] = g; return false; }
        for (int y = 0; y < h; y++) memcpy(&g_alpha[(g_penY + y) * ATLAS + g_penX], sdf + y * w, w);
        g.u0 = (float)g_penX / ATLAS; g.v0 = (float)g_penY / ATLAS;
        g.u1 = (float)(g_penX + w) / ATLAS; g.v1 = (float)(g_penY + h) / ATLAS;
        g.xoff = (float)xo; g.yoff = (float)yo; g.w = (float)w; g.h = (float)h;
        if (g_penX < g_dx0) g_dx0 = g_penX;
        if (g_penY < g_dy0) g_dy0 = g_penY;
        if (g_penX + w > g_dx1) g_dx1 = g_penX + w;
        if (g_penY + h > g_dy1) g_dy1 = g_penY + h;
        g_penX += w + 1; if (h > g_rowH) g_rowH = h;
        g_dirty = true;
    }
    if (sdf) stbtt_FreeSDF(sdf, nullptr);
    g.ok = true;
    f.glyphs[cp] = g;
    return true;
}

const Glyph* GetGlyph(int fid, uint32_t cp) {
    FontData& f = g_fonts[fid];
    auto it = f.glyphs.find(cp);
    if (it == f.glyphs.end()) { AddGlyph(f, cp); it = f.glyphs.find(cp); }
    if (!it->second.ok && fid != FONT_BODY) return GetGlyph(FONT_BODY, cp);   // fallback
    return &it->second;
}

// the atlas goes up whole once; after that only the rectangle new glyphs were added to
void Upload() {
    if (!g_dirty) return;
    GLint prev; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    if (!g_uploaded) {
        std::vector<uint32_t> rgba(ATLAS * ATLAS);
        for (int i = 0; i < ATLAS * ATLAS; i++) rgba[i] = 0x00ffffffu | ((uint32_t)g_alpha[i] << 24);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS, ATLAS, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        g_uploaded = true;
    } else if (g_dx1 > g_dx0 && g_dy1 > g_dy0) {
        int w = g_dx1 - g_dx0, h = g_dy1 - g_dy0;
        std::vector<uint32_t> rgba((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) rgba[(size_t)y * w + x] = 0x00ffffffu | ((uint32_t)g_alpha[(g_dy0 + y) * ATLAS + g_dx0 + x] << 24);
        GLint align; glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        GLint rowLen; glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLen);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, g_dx0, g_dy0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLen);
    }
    glBindTexture(GL_TEXTURE_2D, prev);
    g_dx0 = g_dy0 = ATLAS; g_dx1 = g_dy1 = 0;
    g_dirty = false;
}

float Kern(FontData& f, uint32_t a, uint32_t b) {
    uint64_t key = ((uint64_t)a << 32) | b;
    auto it = f.kern.find(key);
    if (it != f.kern.end()) return it->second;
    float k = stbtt_GetCodepointKernAdvance(&f.info, (int)a, (int)b) * f.scale;
    if (f.kern.size() > 200000) f.kern.clear();
    f.kern.emplace(key, k);
    return k;
}

uint32_t NextCP(const char*& s) {
    const unsigned char* p = (const unsigned char*)s;
    uint32_t c = *p++;
    if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
    else if (c >= 0xe0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if (c >= 0xc0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
    s = (const char*)p;
    return c;
}
}  // namespace

GLuint Font_Texture() { return g_tex; }

bool Font_Init() {
    const unsigned char* data[FONT_COUNT] = {font_lora, font_lora_bold, font_cinzel};
    for (int i = 0; i < FONT_COUNT; i++) {
        FontData& f = g_fonts[i];
        if (!stbtt_InitFont(&f.info, data[i], stbtt_GetFontOffsetForIndex(data[i], 0))) { Log("font %d init failed", i); return false; }
        f.scale = stbtt_ScaleForPixelHeight(&f.info, BASE);
        int a, d, l; stbtt_GetFontVMetrics(&f.info, &a, &d, &l);
        f.ascent = a * f.scale; f.descent = d * f.scale; f.lineGap = l * f.scale;
    }
    g_alpha.assign(ATLAS * ATLAS, 0);
    glGenTextures(1, &g_tex);
    GLint prev; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, prev);
    // pre-build common glyphs
    for (int fid = 0; fid < FONT_COUNT; fid++) {
        for (uint32_t c = 32; c < 127; c++) GetGlyph(fid, c);
        for (uint32_t c = 0xa0; c < 0x180; c++) GetGlyph(fid, c);
        for (uint32_t c = 0x2010; c < 0x203b; c++) GetGlyph(fid, c);      // dashes, quotes, ellipsis ... common in module text
    }
    g_dirty = true;
    Upload();
    Log("font atlas built (rows used up to %d)", g_penY + g_rowH);
    return true;
}

namespace R {
float LineHeight(FontId fid, float size) {
    FontData& f = g_fonts[fid];
    return (f.ascent - f.descent + f.lineGap) * size / BASE;
}

static float Draw(FontId fid, float size, float x, float y, const char* s, Col c, int maxChars, bool draw) {
    if (!s) return 0;
    FontData& f = g_fonts[fid];
    float k = size / BASE;
    float pen = x;
    float base = y + f.ascent * k;
    uint32_t prev = 0;
    int n = 0;
    while (*s && (maxChars < 0 || n < maxChars)) {
        uint32_t cp = NextCP(s); n++;
        if (cp == '\n') continue;
        const Glyph* g = GetGlyph(fid, cp);
        if (prev) pen += Kern(f, prev, cp) * k;
        if (draw && g->w > 0) {
            float x0 = pen + g->xoff * k, y0 = base + g->yoff * k;
            R_PushGlyph(g_tex, x0, y0, x0 + g->w * k, y0 + g->h * k, g->u0, g->v0, g->u1, g->v1, c);
        }
        pen += g->adv * k;
        prev = cp;
    }
    if (draw) Upload();
    return pen - x;
}

float Text(FontId f, float size, float x, float y, const char* s, Col c, int maxChars) { return Draw(f, size, x, y, s, c, maxChars, true); }
float TextWidth(FontId f, float size, const char* s, int maxChars) { return Draw(f, size, 0, 0, s, Col(), maxChars, false); }
void TextCentered(FontId f, float size, float cx, float y, const char* s, Col c) { Text(f, size, cx - TextWidth(f, size, s) * 0.5f, y, s, c); }
void TextRight(FontId f, float size, float rx, float y, const char* s, Col c) { Text(f, size, rx - TextWidth(f, size, s), y, s, c); }

void TextFit(FontId f, float size, float x, float y, float w, const char* s, Col c) {
    if (TextWidth(f, size, s) <= w) { Text(f, size, x, y, s, c); return; }
    float ell = TextWidth(f, size, "...");
    // count characters that fit (widths summed as we go)
    FontData& fd = g_fonts[f];
    float k = size / BASE, run = 0;
    const char* p = s; int n = 0; int fit = 0; uint32_t prev = 0;
    while (*p) {
        uint32_t cp = NextCP(p); n++;
        if (cp != '\n') {
            if (prev) run += Kern(fd, prev, cp) * k;
            run += GetGlyph(f, cp)->adv * k;
            prev = cp;
        }
        if (run + ell <= w) fit = n; else break;
    }
    float used = Text(f, size, x, y, s, c, fit);
    Text(f, size, x + used, y, "...", c);
}

float TextWrap(FontId f, float size, float x, float y, float w, const char* s, Col c, int align, float spacing, bool measureOnly) {
    if (!s) return 0;
    float lh = LineHeight(f, size) * spacing;
    float cy = y;
    const char* line = s;
    while (*line) {
        // find the longest run that fits
        const char* p = line; const char* lastBreak = nullptr; int chars = 0, breakChars = 0;
        bool hardBreak = false;
        FontData& fd = g_fonts[f];
        float k = size / BASE, run = 0;                       // width of the line so far
        uint32_t prev = 0;
        while (*p) {
            const char* q = p;
            uint32_t cp = NextCP(q);
            if (cp == '\n') { hardBreak = true; break; }
            float next = run + (prev ? Kern(fd, prev, cp) * k : 0) + GetGlyph(f, cp)->adv * k;
            if (next > w && chars > 0) break;
            run = next; prev = cp;
            chars++;
            p = q;
            if (cp == ' ') { lastBreak = p; breakChars = chars; }
        }
        int drawChars = chars;
        const char* next = p;
        if (*p && !hardBreak && lastBreak) { drawChars = breakChars; next = lastBreak; }
        if (hardBreak) { next = p; NextCP(next); }
        if (!measureOnly) {
            float lw = TextWidth(f, size, line, drawChars);
            float lx = align == 1 ? x + (w - lw) * 0.5f : align == 2 ? x + w - lw : x;
            Text(f, size, lx, cy, line, c, drawChars);
        }
        cy += lh;
        if (next == line) break;
        line = next;
        while (*line == ' ') line++;
    }
    return cy - y;
}
}  // namespace R
