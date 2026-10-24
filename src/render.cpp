// NWNPad overlay renderer
#include "render.h"
#include "log.h"
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdio>
static char g_renderer[160] = "";

// ---------------------------------------------------------------------------------------------
// GL entry points
#define GLFN(t, n) static t p##n = nullptr;
GLFN(PFNGLCREATESHADERPROC, glCreateShader) GLFN(PFNGLSHADERSOURCEPROC, glShaderSource)
GLFN(PFNGLCOMPILESHADERPROC, glCompileShader) GLFN(PFNGLGETSHADERIVPROC, glGetShaderiv)
GLFN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) GLFN(PFNGLCREATEPROGRAMPROC, glCreateProgram)
GLFN(PFNGLATTACHSHADERPROC, glAttachShader) GLFN(PFNGLLINKPROGRAMPROC, glLinkProgram)
GLFN(PFNGLGETPROGRAMIVPROC, glGetProgramiv) GLFN(PFNGLUSEPROGRAMPROC, glUseProgram)
GLFN(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) GLFN(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)
GLFN(PFNGLUNIFORM2FPROC, glUniform2f) GLFN(PFNGLUNIFORM1IPROC, glUniform1i)
GLFN(PFNGLGENBUFFERSPROC, glGenBuffers) GLFN(PFNGLBINDBUFFERPROC, glBindBuffer) GLFN(PFNGLBUFFERDATAPROC, glBufferData)
GLFN(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) GLFN(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)
GLFN(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) GLFN(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)
GLFN(PFNGLACTIVETEXTUREPROC, glActiveTexture) GLFN(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)
GLFN(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) GLFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)
GLFN(PFNGLBINDSAMPLERPROC, glBindSampler) GLFN(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D)
GLFN(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)
#undef GLFN

static bool g_glReady = false, g_glFailed = false;
static GLuint g_prog = 0, g_vbo = 0, g_vao = 0, g_white = 0;
static GLint g_uScreen = -1, g_uTex = -1;
static int g_w = 1, g_h = 1;
static float g_scale = 1.0f;

struct V {
    float x, y, u, v;
    uint32_t col;
    float lx, ly, hw, hh, rad, p1;   // local pos (relative to shape center), half size, radius, param
    float mode;                      // 0 texture, 1 sdf fill, 2 sdf border(p1=thickness), 3 sdf shadow(p1=blur), 4 arc
    float a0, a1;                    // arc range
};
static std::vector<V> g_verts;
struct Batch { GLuint tex; size_t first, count; int clip; };
static std::vector<Batch> g_batches;
struct Clip { float x, y, w, h; };
static std::vector<Clip> g_clipStack, g_clips;
static int g_curClip = -1;

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos; layout(location=1) in vec2 aUV; layout(location=2) in vec4 aCol;\n"
    "layout(location=3) in vec4 aLoc; layout(location=4) in vec2 aRad; layout(location=5) in vec3 aMode;\n"
    "uniform vec2 uScreen; out vec2 vUV; out vec4 vCol; out vec4 vLoc; out vec2 vRad; out vec3 vMode;\n"
    "void main(){ vUV=aUV; vCol=aCol; vLoc=aLoc; vRad=aRad; vMode=aMode;\n"
    " gl_Position=vec4(aPos.x/uScreen.x*2.0-1.0, 1.0-aPos.y/uScreen.y*2.0, 0.0, 1.0); }\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; in vec4 vCol; in vec4 vLoc; in vec2 vRad; in vec3 vMode; uniform sampler2D uTex; out vec4 o;\n"
    "float sdRound(vec2 p, vec2 b, float r){ vec2 q=abs(p)-b+vec2(r); return length(max(q,0.0))+min(max(q.x,q.y),0.0)-r; }\n"
    "void main(){ float m=vMode.x;\n"
    " if(m>6.5){ float s=texture(uTex,vUV).a; float w=max(fwidth(s),0.0001)*0.7; float a=smoothstep(0.5-w,0.5+w,s); o=vec4(vCol.rgb,vCol.a*a); return; }\n"
    " if(m<0.5){ o=vCol*texture(uTex,vUV); return; }\n"
    " float d=sdRound(vLoc.xy, vLoc.zw, vRad.x); float aa=max(fwidth(d),0.0001);\n"
    " if(m<1.5){ float a=clamp(0.5-d/aa,0.0,1.0); o=vec4(vCol.rgb, vCol.a*a); return; }\n"
    " if(m<2.5){ float t=vRad.y; float dd=abs(d+t*0.5)-t*0.5; float a=clamp(0.5-dd/aa,0.0,1.0); o=vec4(vCol.rgb,vCol.a*a); return; }\n"
    " if(m<3.5){ float b=max(vRad.y,1.0); float a=1.0-smoothstep(-b,b,d); o=vec4(vCol.rgb,vCol.a*a*a); return; }\n"
    " if(m<4.5){ float r=length(vLoc.xy); float t=vRad.y; float dd=abs(r-(vRad.x-t*0.5))-t*0.5; float aa2=max(fwidth(dd),0.0001);\n"
    "   float a=clamp(0.5-dd/aa2,0.0,1.0); float ang=atan(vLoc.x,-vLoc.y); if(ang<0.0) ang+=6.2831853;\n"
    "   float a0=vMode.y, a1=vMode.z; float inside = (ang>=a0 && ang<=a1) ? 1.0 : 0.0; o=vec4(vCol.rgb,vCol.a*a*inside); return; }\n"
    " if(m<5.5){ vec4 t=texture(uTex,vUV); float a=clamp(0.5-d/aa,0.0,1.0); o=vCol*t*vec4(1,1,1,a); return; }\n"
    " o=vCol; }\n";

static void* GLProc(const char* n) {
    void* p = (void*)wglGetProcAddress(n);
    if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) {
        static HMODULE gl = LoadLibraryA("opengl32.dll");
        p = (void*)GetProcAddress(gl, n);
    }
    return p;
}

static GLuint Compile(GLenum type, const char* src) {
    GLuint s = pglCreateShader(type);
    pglShaderSource(s, 1, &src, nullptr);
    pglCompileShader(s);
    GLint ok = 0; pglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char buf[2048]; pglGetShaderInfoLog(s, sizeof buf, nullptr, buf); Log("shader error: %s", buf); }
    return s;
}

static bool InitGL() {
#define L(n) p##n = (decltype(p##n))GLProc(#n); if (!p##n) { Log("GL: missing %s", #n); return false; }
    L(glCreateShader) L(glShaderSource) L(glCompileShader) L(glGetShaderiv) L(glGetShaderInfoLog)
    L(glCreateProgram) L(glAttachShader) L(glLinkProgram) L(glGetProgramiv) L(glUseProgram)
    L(glBindAttribLocation) L(glGetUniformLocation) L(glUniform2f) L(glUniform1i) L(glGenBuffers)
    L(glBindBuffer) L(glBufferData) L(glGenVertexArrays) L(glBindVertexArray) L(glEnableVertexAttribArray)
    L(glVertexAttribPointer) L(glActiveTexture) L(glBlendFuncSeparate) L(glBlendEquationSeparate)
    L(glBindFramebuffer) L(glCompressedTexImage2D) L(glGenerateMipmap)
#undef L
    pglBindSampler = (PFNGLBINDSAMPLERPROC)GLProc("glBindSampler");
    GLuint vs = Compile(GL_VERTEX_SHADER, VS), fs = Compile(GL_FRAGMENT_SHADER, FS);
    g_prog = pglCreateProgram();
    pglAttachShader(g_prog, vs); pglAttachShader(g_prog, fs);
    pglLinkProgram(g_prog);
    GLint ok = 0; pglGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { Log("GL: program link failed"); return false; }
    g_uScreen = pglGetUniformLocation(g_prog, "uScreen");
    g_uTex = pglGetUniformLocation(g_prog, "uTex");
    // the game caches its GL bindings: leave the vertex array and array buffer as they were
    GLint prevVao = 0, prevBuf = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    pglGenBuffers(1, &g_vbo);
    pglGenVertexArrays(1, &g_vao);
    pglBindVertexArray(g_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    pglEnableVertexAttribArray(0); pglVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, x));
    pglEnableVertexAttribArray(1); pglVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, u));
    pglEnableVertexAttribArray(2); pglVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(V), (void*)offsetof(V, col));
    pglEnableVertexAttribArray(3); pglVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, lx));
    pglEnableVertexAttribArray(4); pglVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, rad));
    pglEnableVertexAttribArray(5); pglVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, mode));
    pglBindVertexArray(prevVao);
    pglBindBuffer(GL_ARRAY_BUFFER, prevBuf);
    uint32_t white = 0xffffffff;
    g_white = R::CreateTexture(1, 1, &white);
    Log("GL overlay ready: %s / %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    if (const char* rn = (const char*)glGetString(GL_RENDERER)) snprintf(g_renderer, sizeof g_renderer, "%s", rn);
    return true;
}

extern bool Font_Init();     // font.cpp
extern float UI_UserScale();
extern GLuint Font_Texture();

// ---------------------------------------------------------------------------------------------
static void PushQuad(GLuint tex, const V& tl, const V& tr, const V& br, const V& bl) {
    if (g_batches.empty() || g_batches.back().tex != tex || g_batches.back().clip != g_curClip)
        g_batches.push_back({tex, g_verts.size(), 0, g_curClip});
    g_verts.push_back(tl); g_verts.push_back(tr); g_verts.push_back(br);
    g_verts.push_back(tl); g_verts.push_back(br); g_verts.push_back(bl);
    g_batches.back().count += 6;
}

static float g_uv[4] = {0, 0, 1, 1};
static void ShapeQuad(float x, float y, float w, float h, float rad, float p1, float mode, Col c, Col c2, float pad, float a0 = 0, float a1 = 0, GLuint tex = 0) {
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    float X0 = x - pad, Y0 = y - pad, X1 = x + w + pad, Y1 = y + h + pad;
    V v{};
    v.hw = hw; v.hh = hh; v.rad = rad; v.p1 = p1; v.mode = mode; v.a0 = a0; v.a1 = a1;
    V tl = v, tr = v, br = v, bl = v;
    tl.x = X0; tl.y = Y0; tr.x = X1; tr.y = Y0; br.x = X1; br.y = Y1; bl.x = X0; bl.y = Y1;
    auto loc = [&](V& q) {
        q.lx = q.x - cx; q.ly = q.y - cy;
        float fu = (q.x - x) / (w > 0 ? w : 1), fv = (q.y - y) / (h > 0 ? h : 1);
        q.u = g_uv[0] + (g_uv[2] - g_uv[0]) * fu; q.v = g_uv[1] + (g_uv[3] - g_uv[1]) * fv;
    };
    loc(tl); loc(tr); loc(br); loc(bl);
    tl.col = tr.col = c.u32(); br.col = bl.col = c2.u32();
    PushQuad(tex ? tex : g_white, tl, tr, br, bl);
}

namespace R {
const char* Renderer() { return g_renderer; }
int W() { return g_w; }
int H() { return g_h; }
float S() { return g_scale; }
float px(float v) { return v * g_scale; }

static GLint s_rfbo, s_prog, s_vao, s_abuf, s_tex, s_atex, s_fbo, s_vp[4], s_sc[4], s_bsrc, s_bdst, s_bsrca, s_bdsta, s_beq, s_beqa, s_sampler;
static GLboolean s_blend, s_depth, s_cull, s_scissor, s_stencil, s_cmask[4], s_dmask;

bool Begin(int sw, int sh) {
    if (g_glFailed) return false;
    if (!g_glReady) {
        if (!InitGL() || !Font_Init()) { g_glFailed = true; return false; }
        g_glReady = true;
    }
    g_w = sw > 0 ? sw : 1; g_h = sh > 0 ? sh : 1;
    g_scale = (g_h / 1080.0f);
    if (g_scale < 0.8f) g_scale = 0.8f;
    g_scale *= UI_UserScale();
    g_verts.clear(); g_batches.clear(); g_clips.clear(); g_clipStack.clear(); g_curClip = -1;
    return true;
}

void End() {
    if (!g_glReady) return;
    if (g_verts.empty()) return;
    // save state
    glGetIntegerv(GL_CURRENT_PROGRAM, &s_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s_abuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s_atex);
    pglActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s_tex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s_rfbo);
    glGetIntegerv(GL_VIEWPORT, s_vp);
    glGetIntegerv(GL_SCISSOR_BOX, s_sc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s_bsrc); glGetIntegerv(GL_BLEND_DST_RGB, &s_bdst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s_bsrca); glGetIntegerv(GL_BLEND_DST_ALPHA, &s_bdsta);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s_beq); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s_beqa);
    if (pglBindSampler) glGetIntegerv(GL_SAMPLER_BINDING, &s_sampler);
    s_blend = glIsEnabled(GL_BLEND); s_depth = glIsEnabled(GL_DEPTH_TEST); s_cull = glIsEnabled(GL_CULL_FACE);
    s_scissor = glIsEnabled(GL_SCISSOR_TEST); s_stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, s_cmask); glGetBooleanv(GL_DEPTH_WRITEMASK, &s_dmask);

    pglBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glEnable(GL_BLEND);
    pglBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    pglBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glColorMask(1, 1, 1, 1); glDepthMask(0);
    if (pglBindSampler) pglBindSampler(0, 0);
    pglUseProgram(g_prog);
    pglUniform2f(g_uScreen, (float)g_w, (float)g_h);
    pglUniform1i(g_uTex, 0);
    pglBindVertexArray(g_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    pglBufferData(GL_ARRAY_BUFFER, g_verts.size() * sizeof(V), g_verts.data(), GL_STREAM_DRAW);
    for (auto& b : g_batches) {
        if (b.clip >= 0) {
            const Clip& c = g_clips[b.clip];
            glEnable(GL_SCISSOR_TEST);
            glScissor((int)c.x, (int)(g_h - c.y - c.h), (int)(c.w > 0 ? c.w : 0), (int)(c.h > 0 ? c.h : 0));
        } else glDisable(GL_SCISSOR_TEST);
        glBindTexture(GL_TEXTURE_2D, b.tex);
        glDrawArrays(GL_TRIANGLES, (GLint)b.first, (GLsizei)b.count);
    }
    // restore
    pglBindVertexArray(s_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, s_abuf);
    pglUseProgram(s_prog);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    pglActiveTexture(s_atex);
    if (pglBindSampler) pglBindSampler(0, s_sampler);
    pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    pglBindFramebuffer(GL_READ_FRAMEBUFFER, s_rfbo);
    glViewport(s_vp[0], s_vp[1], s_vp[2], s_vp[3]);
    glScissor(s_sc[0], s_sc[1], s_sc[2], s_sc[3]);
    pglBlendFuncSeparate(s_bsrc, s_bdst, s_bsrca, s_bdsta);
    pglBlendEquationSeparate(s_beq, s_beqa);
    if (s_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (s_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s_stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glColorMask(s_cmask[0], s_cmask[1], s_cmask[2], s_cmask[3]);
    glDepthMask(s_dmask);
}

void RoundRect(float x, float y, float w, float h, float r, Col c) { ShapeQuad(x, y, w, h, r, 0, 1, c, c, 1); }
void RoundRectGrad(float x, float y, float w, float h, float r, Col t, Col b) { ShapeQuad(x, y, w, h, r, 0, 1, t, b, 0); }
void RoundRectBorder(float x, float y, float w, float h, float r, float t, Col c) { ShapeQuad(x, y, w, h, r, t, 2, c, c, 1); }
void Shadow(float x, float y, float w, float h, float r, float blur, Col c) { ShapeQuad(x, y, w, h, r, blur, 3, c, c, blur * 2); }
void Circle(float cx, float cy, float r, Col c) { ShapeQuad(cx - r, cy - r, 2 * r, 2 * r, r, 0, 1, c, c, 1); }
void Ring(float cx, float cy, float r, float t, Col c) { ShapeQuad(cx - r, cy - r, 2 * r, 2 * r, r, t, 2, c, c, 1); }
void Rect(float x, float y, float w, float h, Col c) { ShapeQuad(x, y, w, h, 0, 0, 1, c, c, 0); }
void Arc(float cx, float cy, float r, float t, float a0, float a1, Col c) {
    ShapeQuad(cx - r, cy - r, 2 * r, 2 * r, r, t, 4, c, c, 1, a0, a1);
}
void Line(float x0, float y0, float x1, float y1, float t, Col c) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.001f) return;
    float nx = -dy / l * t * 0.5f, ny = dx / l * t * 0.5f;
    V v{}; v.mode = 6; v.col = c.u32();
    V a = v, b = v, cc = v, d = v;
    a.x = x0 + nx; a.y = y0 + ny; b.x = x1 + nx; b.y = y1 + ny; cc.x = x1 - nx; cc.y = y1 - ny; d.x = x0 - nx; d.y = y0 - ny;
    PushQuad(g_white, a, b, cc, d);
}
void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, Col c) {
    V v{}; v.mode = 6; v.col = c.u32();
    V a = v, b = v, d = v;
    a.x = x0; a.y = y0; b.x = x1; b.y = y1; d.x = x2; d.y = y2;
    PushQuad(g_white, a, b, d, d);
}

uint32_t CreateTexture(int w, int h, const void* rgba) {
    GLuint t; glGenTextures(1, &t);
    GLint prev; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, t);
    GLint unpack = 4; glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
    glBindTexture(GL_TEXTURE_2D, prev);
    return t;
}
uint32_t CreateTextureCompressed(int w, int h, int fmt, const void* data, int size, int mips, const int* mipSizes) {
    if (!pglCompressedTexImage2D) return 0;
    GLuint t; glGenTextures(1, &t);
    GLint prev; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, t);
    const uint8_t* p = (const uint8_t*)data;
    int mw = w, mh = h, used = 0;
    int levels = 0;
    for (int i = 0; i < (mips > 0 ? mips : 1); i++) {
        int sz = mipSizes ? mipSizes[i] : size;
        if (used + sz > size) break;
        pglCompressedTexImage2D(GL_TEXTURE_2D, i, fmt, mw, mh, 0, sz, p + used);
        used += sz; levels++;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels > 0 ? levels - 1 : 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, prev);
    return t;
}
void DeleteTexture(uint32_t t) { GLuint x = t; if (x) glDeleteTextures(1, &x); }

void Image(uint32_t tex, float x, float y, float w, float h, Col tint, float u0, float v0, float u1, float v1) {
    V v{}; v.mode = 0; v.col = tint.u32();
    V a = v, b = v, c = v, d = v;
    a.x = x; a.y = y; a.u = u0; a.v = v0;
    b.x = x + w; b.y = y; b.u = u1; b.v = v0;
    c.x = x + w; c.y = y + h; c.u = u1; c.v = v1;
    d.x = x; d.y = y + h; d.u = u0; d.v = v1;
    PushQuad(tex, a, b, c, d);
}
void ImageRot(uint32_t tex, float x, float y, float w, float h, int quarter, Col tint) {
    V v{}; v.mode = 0; v.col = tint.u32();
    V q[4] = {v, v, v, v};
    const float P[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    const float UV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    quarter = ((quarter % 4) + 4) % 4;
    for (int i = 0; i < 4; i++) {
        q[i].x = P[i][0]; q[i].y = P[i][1];
        q[i].u = UV[(i + quarter) % 4][0]; q[i].v = UV[(i + quarter) % 4][1];
    }
    PushQuad(tex, q[0], q[1], q[2], q[3]);
}
void ImageQuad(uint32_t tex, const float* x, const float* y, int quarter, Col tint) {
    V v{}; v.mode = 0; v.col = tint.u32();
    V q[4] = {v, v, v, v};
    const float UV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    quarter = ((quarter % 4) + 4) % 4;
    for (int i = 0; i < 4; i++) {
        q[i].x = x[i]; q[i].y = y[i];
        q[i].u = UV[(i + quarter) % 4][0]; q[i].v = UV[(i + quarter) % 4][1];
    }
    PushQuad(tex ? tex : g_white, q[0], q[1], q[2], q[3]);
}
void ImageRounded(uint32_t tex, float x, float y, float w, float h, float r, Col tint, float u0, float v0, float u1, float v1) {
    g_uv[0] = u0; g_uv[1] = v0; g_uv[2] = u1; g_uv[3] = v1;
    ShapeQuad(x, y, w, h, r, 0, 5, tint, tint, 0, 0, 0, tex);
    g_uv[0] = 0; g_uv[1] = 0; g_uv[2] = 1; g_uv[3] = 1;
}

void PushClip(float x, float y, float w, float h) {
    if (!g_clipStack.empty()) {   // intersect with parent
        const Clip& p = g_clipStack.back();
        float x1 = fmaxf(x, p.x), y1 = fmaxf(y, p.y);
        float x2 = fminf(x + w, p.x + p.w), y2 = fminf(y + h, p.y + p.h);
        x = x1; y = y1; w = x2 - x1; h = y2 - y1;
    }
    Clip c{x, y, w, h};
    g_clipStack.push_back(c);
    g_clips.push_back(c);
    g_curClip = (int)g_clips.size() - 1;
}
void PopClip() {
    if (!g_clipStack.empty()) g_clipStack.pop_back();
    if (g_clipStack.empty()) g_curClip = -1;
    else { g_clips.push_back(g_clipStack.back()); g_curClip = (int)g_clips.size() - 1; }
}
}  // namespace R

// used by font.cpp
void R_PushGlyph(GLuint tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Col c) {
    V v{}; v.mode = 7; v.col = c.u32();
    V a = v, b = v, cc = v, d = v;
    a.x = x0; a.y = y0; a.u = u0; a.v = v0;
    b.x = x1; b.y = y0; b.u = u1; b.v = v0;
    cc.x = x1; cc.y = y1; cc.u = u1; cc.v = v1;
    d.x = x0; d.y = y1; d.u = u0; d.v = v1;
    PushQuad(tex, a, b, cc, d);
}
