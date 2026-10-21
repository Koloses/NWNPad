// NWNPad - OpenGL diagnostics ([Developer] GLDiag=1). Independent of the gamepad features: logs the
// context, the driver's debug messages and every shader compile / link to NWNPad-gl.log, and on F11 (or
// both stick clicks held for a second) captures one frame: screenshot, every draw call with its program,
// textures and changed uniform values, the programs' shader sources and small textures as TGA files.
// Written to find rendering bugs on the player's GPU driver.
#include "gldiag.h"
#include "config.h"
#include "pad.h"
#include "log.h"
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>

#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#endif

namespace {
char g_dir[MAX_PATH];
FILE* g_log = nullptr;
void GLog(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}
void WriteFile_(const std::string& path, const std::string& s) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
}
void WriteTGA(const std::string& path, int w, int h, const std::vector<uint8_t>& bgra) {   // bottom-up rows
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint8_t hd[18] = {};
    hd[2] = 2; hd[12] = w & 255; hd[13] = w >> 8; hd[14] = h & 255; hd[15] = h >> 8; hd[16] = 32; hd[17] = 8;
    fwrite(hd, 1, 18, f); fwrite(bgra.data(), 1, bgra.size(), f); fclose(f);
}

// ---- originals ----
typedef PROC(WINAPI* wglGPA_t)(LPCSTR);
wglGPA_t o_wglGPA = nullptr;
template <class T> T GLP(const char* n) { return o_wglGPA ? (T)o_wglGPA(n) : nullptr; }

void(APIENTRY* o_DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
void(APIENTRY* o_DrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
void(APIENTRY* o_BindTexture)(GLenum, GLuint) = nullptr;

PFNGLCREATESHADERPROC o_CreateShader = nullptr;
PFNGLSHADERSOURCEPROC o_ShaderSource = nullptr;
PFNGLCOMPILESHADERPROC o_CompileShader = nullptr;
PFNGLATTACHSHADERPROC o_AttachShader = nullptr;
PFNGLLINKPROGRAMPROC o_LinkProgram = nullptr;
PFNGLUSEPROGRAMPROC o_UseProgram = nullptr;
PFNGLACTIVETEXTUREPROC o_ActiveTexture = nullptr;
PFNGLDRAWRANGEELEMENTSPROC o_DrawRangeElements = nullptr;
PFNGLDRAWELEMENTSBASEVERTEXPROC o_DrawElementsBaseVertex = nullptr;
PFNGLDRAWRANGEELEMENTSBASEVERTEXPROC o_DrawRangeElementsBaseVertex = nullptr;
PFNGLDRAWELEMENTSINSTANCEDPROC o_DrawElementsInstanced = nullptr;
PFNGLDRAWARRAYSINSTANCEDPROC o_DrawArraysInstanced = nullptr;
PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXPROC o_DrawElementsInstancedBaseVertex = nullptr;

// ---- tracked state ----
struct ShaderInfo { GLenum type = 0; std::string src; bool ok = true; std::string log; };
std::map<GLuint, ShaderInfo> g_shaders;
std::map<GLuint, std::vector<GLuint>> g_progShaders;
GLuint g_prog = 0;
int g_unit = 0;
struct Bind { GLenum target = 0; GLuint tex = 0; };
Bind g_bind[32];

// ---- capture ----
enum { CAP_IDLE, CAP_ARMED, CAP_REC };
int g_cap = CAP_IDLE;
std::string g_capDir;
std::string g_draws;
int g_drawCount = 0;
std::set<GLuint> g_capProgs, g_capTex;
std::map<std::pair<GLuint, std::string>, std::string> g_lastUni;

const char* TypeName(GLenum t) {
    switch (t) {
    case GL_VERTEX_SHADER: return "vertex"; case GL_FRAGMENT_SHADER: return "fragment";
    case GL_GEOMETRY_SHADER: return "geometry"; default: return "other";
    }
}

std::string UniformValue(GLuint prog, GLint loc, GLenum type, GLint size) {
    static auto getf = GLP<PFNGLGETUNIFORMFVPROC>("glGetUniformfv");
    static auto geti = GLP<PFNGLGETUNIFORMIVPROC>("glGetUniformiv");
    if (!getf || !geti) return "?";
    int n = 1; bool isInt = false;
    switch (type) {
    case GL_FLOAT: n = 1; break; case GL_FLOAT_VEC2: n = 2; break; case GL_FLOAT_VEC3: n = 3; break; case GL_FLOAT_VEC4: n = 4; break;
    case GL_FLOAT_MAT3: n = 9; break; case GL_FLOAT_MAT4: n = 16; break; case GL_FLOAT_MAT2: n = 4; break;
    case GL_INT: case GL_BOOL: case GL_SAMPLER_2D: case GL_SAMPLER_CUBE: case GL_SAMPLER_3D: case GL_SAMPLER_2D_SHADOW:
    case GL_SAMPLER_1D: case GL_UNSIGNED_INT: n = 1; isInt = true; break;
    case GL_INT_VEC2: case GL_BOOL_VEC2: n = 2; isInt = true; break; case GL_INT_VEC3: case GL_BOOL_VEC3: n = 3; isInt = true; break;
    case GL_INT_VEC4: case GL_BOOL_VEC4: n = 4; isInt = true; break;
    default: return "(type " + std::to_string(type) + ")";
    }
    std::string r;
    char b[48];
    if (isInt) { GLint v[16] = {}; geti(prog, loc, v); for (int i = 0; i < n; i++) { snprintf(b, sizeof b, "%s%d", i ? " " : "", v[i]); r += b; } }
    else { GLfloat v[16] = {}; getf(prog, loc, v); for (int i = 0; i < n; i++) { snprintf(b, sizeof b, "%s%.4g", i ? " " : "", v[i]); r += b; } }
    if (size > 1) r += " [+" + std::to_string(size - 1) + " more]";
    return r;
}

void RecordDraw(const char* fn, GLenum mode, GLsizei count, GLsizei inst) {
    if (g_cap != CAP_REC || g_drawCount >= 4000) return;
    g_drawCount++;
    char b[256];
    snprintf(b, sizeof b, "#%d %s mode %u count %d%s prog %u tex", g_drawCount, fn, mode, count,
             inst > 1 ? (" inst " + std::to_string(inst)).c_str() : "", g_prog);
    std::string line = b;
    for (int u = 0; u < 16; u++) if (g_bind[u].tex) {
        snprintf(b, sizeof b, " u%d:%u%s", u, g_bind[u].tex, g_bind[u].target == GL_TEXTURE_2D ? "" : g_bind[u].target == GL_TEXTURE_CUBE_MAP ? "(cube)" : "(other)");
        line += b;
        if (g_bind[u].target == GL_TEXTURE_2D) g_capTex.insert(g_bind[u].tex);
    }
    g_draws += line + "\n";
    g_capProgs.insert(g_prog);
    // uniforms whose value changed since the last draw with this program
    static auto getpi = GLP<PFNGLGETPROGRAMIVPROC>("glGetProgramiv");
    static auto getau = GLP<PFNGLGETACTIVEUNIFORMPROC>("glGetActiveUniform");
    static auto getloc = GLP<PFNGLGETUNIFORMLOCATIONPROC>("glGetUniformLocation");
    if (!g_prog || !getpi || !getau || !getloc || g_draws.size() > (24u << 20)) return;
    GLint nu = 0; getpi(g_prog, GL_ACTIVE_UNIFORMS, &nu);
    for (GLint i = 0; i < nu && i < 128; i++) {
        char name[128]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        getau(g_prog, i, sizeof name, &len, &size, &type, name);
        GLint loc = getloc(g_prog, name);
        if (loc < 0) continue;
        std::string v = UniformValue(g_prog, loc, type, size);
        auto key = std::make_pair(g_prog, std::string(name));
        auto it = g_lastUni.find(key);
        if (it != g_lastUni.end() && it->second == v) continue;
        g_lastUni[key] = v;
        g_draws += "    " + std::string(name) + " = " + v + "\n";
    }
}

// ---- wrappers ----
void APIENTRY w_DrawArrays(GLenum m, GLint f, GLsizei c) { RecordDraw("DrawArrays", m, c, 1); o_DrawArrays(m, f, c); }
void APIENTRY w_DrawElements(GLenum m, GLsizei c, GLenum t, const void* i) { RecordDraw("DrawElements", m, c, 1); o_DrawElements(m, c, t, i); }
void APIENTRY w_BindTexture(GLenum t, GLuint tex) {
    if (g_unit >= 0 && g_unit < 32) { g_bind[g_unit].target = t; g_bind[g_unit].tex = tex; }
    o_BindTexture(t, tex);
}
GLuint APIENTRY w_CreateShader(GLenum type) {
    GLuint s = o_CreateShader(type);
    g_shaders[s] = ShaderInfo(); g_shaders[s].type = type;
    return s;
}
void APIENTRY w_ShaderSource(GLuint s, GLsizei n, const GLchar* const* str, const GLint* len) {
    std::string src;
    for (GLsizei i = 0; i < n; i++) {
        if (!str[i]) continue;
        if (len && len[i] >= 0) src.append(str[i], len[i]); else src += str[i];
    }
    g_shaders[s].src = src;
    o_ShaderSource(s, n, str, len);
}
void APIENTRY w_CompileShader(GLuint s) {
    o_CompileShader(s);
    static auto getiv = GLP<PFNGLGETSHADERIVPROC>("glGetShaderiv");
    static auto getlog = GLP<PFNGLGETSHADERINFOLOGPROC>("glGetShaderInfoLog");
    ShaderInfo& si = g_shaders[s];
    GLint ok = 1, ll = 0;
    if (getiv) { getiv(s, GL_COMPILE_STATUS, &ok); getiv(s, GL_INFO_LOG_LENGTH, &ll); }
    si.ok = ok != 0;
    si.log.clear();
    if (ll > 1 && getlog) { si.log.resize(ll); getlog(s, ll, nullptr, &si.log[0]); si.log.resize(strlen(si.log.c_str())); }
    GLog("compile %s shader %u: %s (%u chars)%s%s", TypeName(si.type), s, ok ? "ok" : "FAILED", (unsigned)si.src.size(),
         si.log.empty() ? "" : " log: ", si.log.substr(0, 600).c_str());
    if (!ok || !si.log.empty()) {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\shader_%u_%s%s.glsl", g_dir, s, TypeName(si.type), ok ? "_warn" : "_FAILED");
        WriteFile_(p, si.src + "\n\n/* compile log:\n" + si.log + "\n*/\n");
    }
}
void APIENTRY w_AttachShader(GLuint p, GLuint s) { g_progShaders[p].push_back(s); o_AttachShader(p, s); }
void APIENTRY w_LinkProgram(GLuint p) {
    o_LinkProgram(p);
    static auto getpi = GLP<PFNGLGETPROGRAMIVPROC>("glGetProgramiv");
    static auto getlog = GLP<PFNGLGETPROGRAMINFOLOGPROC>("glGetProgramInfoLog");
    GLint ok = 1, ll = 0;
    if (getpi) { getpi(p, GL_LINK_STATUS, &ok); getpi(p, GL_INFO_LOG_LENGTH, &ll); }
    std::string log;
    if (ll > 1 && getlog) { log.resize(ll); getlog(p, ll, nullptr, &log[0]); log.resize(strlen(log.c_str())); }
    std::string sh;
    for (GLuint s : g_progShaders[p]) sh += " " + std::to_string(s);
    GLog("link program %u (shaders%s): %s%s%s", p, sh.c_str(), ok ? "ok" : "FAILED", log.empty() ? "" : " log: ", log.substr(0, 600).c_str());
}
void APIENTRY w_UseProgram(GLuint p) { g_prog = p; o_UseProgram(p); }
void APIENTRY w_ActiveTexture(GLenum u) { g_unit = (int)u - GL_TEXTURE0; o_ActiveTexture(u); }
void APIENTRY w_DrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei c, GLenum t, const void* i) { RecordDraw("DrawRangeElements", m, c, 1); o_DrawRangeElements(m, a, b, c, t, i); }
void APIENTRY w_DrawElementsBaseVertex(GLenum m, GLsizei c, GLenum t, const void* i, GLint bv) { RecordDraw("DrawElementsBaseVertex", m, c, 1); o_DrawElementsBaseVertex(m, c, t, i, bv); }
void APIENTRY w_DrawRangeElementsBaseVertex(GLenum m, GLuint a, GLuint b, GLsizei c, GLenum t, const void* i, GLint bv) { RecordDraw("DrawRangeElementsBaseVertex", m, c, 1); o_DrawRangeElementsBaseVertex(m, a, b, c, t, i, bv); }
void APIENTRY w_DrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n) { RecordDraw("DrawElementsInstanced", m, c, n); o_DrawElementsInstanced(m, c, t, i, n); }
void APIENTRY w_DrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) { RecordDraw("DrawArraysInstanced", m, c, n); o_DrawArraysInstanced(m, f, c, n); }
void APIENTRY w_DrawElementsInstancedBaseVertex(GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n, GLint bv) { RecordDraw("DrawElementsInstancedBaseVertex", m, c, n); o_DrawElementsInstancedBaseVertex(m, c, t, i, n, bv); }

struct Wrap { const char* name; void* wrapper; void** orig; };
Wrap WRAPS[] = {
    {"glCreateShader", (void*)w_CreateShader, (void**)&o_CreateShader},
    {"glShaderSource", (void*)w_ShaderSource, (void**)&o_ShaderSource},
    {"glCompileShader", (void*)w_CompileShader, (void**)&o_CompileShader},
    {"glAttachShader", (void*)w_AttachShader, (void**)&o_AttachShader},
    {"glLinkProgram", (void*)w_LinkProgram, (void**)&o_LinkProgram},
    {"glUseProgram", (void*)w_UseProgram, (void**)&o_UseProgram},
    {"glActiveTexture", (void*)w_ActiveTexture, (void**)&o_ActiveTexture},
    {"glDrawRangeElements", (void*)w_DrawRangeElements, (void**)&o_DrawRangeElements},
    {"glDrawElementsBaseVertex", (void*)w_DrawElementsBaseVertex, (void**)&o_DrawElementsBaseVertex},
    {"glDrawRangeElementsBaseVertex", (void*)w_DrawRangeElementsBaseVertex, (void**)&o_DrawRangeElementsBaseVertex},
    {"glDrawElementsInstanced", (void*)w_DrawElementsInstanced, (void**)&o_DrawElementsInstanced},
    {"glDrawArraysInstanced", (void*)w_DrawArraysInstanced, (void**)&o_DrawArraysInstanced},
    {"glDrawElementsInstancedBaseVertex", (void*)w_DrawElementsInstancedBaseVertex, (void**)&o_DrawElementsInstancedBaseVertex},
};

PROC WINAPI hk_wglGPA(LPCSTR name) {
    PROC p = o_wglGPA(name);
    if (!p || !name || ((uintptr_t)name >> 16) == 0) return p;
    for (auto& w : WRAPS) {
        if (strcmp(name, w.name) == 0) { *w.orig = (void*)p; return (PROC)w.wrapper; }
    }
    // the ARB / EXT spellings go to the same functions
    static const char* alias[][2] = {{"glActiveTextureARB", "glActiveTexture"}, {"glUseProgramObjectARB", "glUseProgram"},
                                     {"glShaderSourceARB", "glShaderSource"}, {"glCompileShaderARB", "glCompileShader"},
                                     {"glLinkProgramARB", "glLinkProgram"}};
    for (auto& a : alias) if (strcmp(name, a[0]) == 0) for (auto& w : WRAPS) if (strcmp(a[1], w.name) == 0) { *w.orig = (void*)p; return (PROC)w.wrapper; }
    return p;
}

void APIENTRY DebugCb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    static std::map<GLuint, int> seen;
    if (++seen[id] > 3) return;
    static int total = 0;
    if (++total > 500) return;
    GLog("driver: src %#x type %#x id %u severity %#x: %s", source, type, id, severity, msg ? msg : "");
    if (type == GL_DEBUG_TYPE_ERROR) {
        // where in the game the failing call came from (synchronous output: this is the caller's stack)
        void* fr[24]; USHORT n = RtlCaptureStackBackTrace(1, 24, fr, nullptr);
        uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
        std::string st;
        for (USHORT i = 0; i < n; i++) {
            uintptr_t a = (uintptr_t)fr[i];
            char b[40];
            if (a >= base && a < base + 0x1711000) snprintf(b, sizeof b, " %llx", (unsigned long long)(a - base + 0x140000000ull));
            else snprintf(b, sizeof b, " ?%llx", (unsigned long long)a);
            st += b;
        }
        GLint prog = 0, fb = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        GLog("   at program %d framebuffer %d, stack:%s", prog, fb, st.c_str());
    }
}

void OnFirstFrame() {
    GLog("vendor: %s", (const char*)glGetString(GL_VENDOR));
    GLog("renderer: %s", (const char*)glGetString(GL_RENDERER));
    GLog("version: %s", (const char*)glGetString(GL_VERSION));
    GLog("glsl: %s", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint prof = 0, flags = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &prof); glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    while (glGetError() != GL_NO_ERROR) {}
    GLog("profile mask %#x (1 core, 2 compatibility), flags %#x", prof, flags);
    auto cb = GLP<PFNGLDEBUGMESSAGECALLBACKPROC>("glDebugMessageCallback");
    if (!cb) cb = GLP<PFNGLDEBUGMESSAGECALLBACKPROC>("glDebugMessageCallbackARB");
    if (cb) { glEnable(GL_DEBUG_OUTPUT); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS); cb(DebugCb, nullptr); GLog("driver debug messages enabled"); }
    else GLog("no debug message callback");
    GLog("shaders compiled before the first frame: %u", (unsigned)g_shaders.size());
}

bool Trigger() {
    static bool f11 = false;
    bool k = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    bool fire = k && !f11;
    f11 = k;
    static double held = 0; static bool fired = false;
    if (g_pad.connected && g_pad.Down(PB_LS) && g_pad.Down(PB_RS)) {
        if (held == 0) held = g_pad.now;
        if (!fired && g_pad.now - held > 1.0) { fired = true; fire = true; }
    } else { held = 0; fired = false; }
    return fire;
}

void FinishCapture() {
    // screenshot of the game's frame (before the overlay)
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    auto bindfb = GLP<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer");
    auto bindbuf = GLP<PFNGLBINDBUFFERPROC>("glBindBuffer");
    GLint rfb = 0, pbo = 0, pack = 4, rb = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb); glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack); glGetIntegerv(GL_READ_BUFFER, &rb);
    if (bindfb) bindfb(GL_READ_FRAMEBUFFER, 0);
    if (bindbuf && pbo) bindbuf(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    RECT rc; GetClientRect(GetActiveWindow() ? GetActiveWindow() : GetForegroundWindow(), &rc);
    int w = vp[2] > 0 ? vp[2] : rc.right, h = vp[3] > 0 ? vp[3] : rc.bottom;
    GLint dims[4] = {0, 0, w, h};
    if (w > 0 && h > 0 && w < 16384 && h < 16384) {
        std::vector<uint8_t> px((size_t)w * h * 4);
        glReadPixels(dims[0], dims[1], w, h, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        WriteTGA(g_capDir + "\\frame.tga", w, h, px);
    }
    // textures used by the draws: size, format, small ones as images
    std::string ti;
    GLint atex = 0, t0 = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &atex);
    if (o_ActiveTexture) o_ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &t0);
    int saved = 0;
    for (GLuint t : g_capTex) {
        o_BindTexture(GL_TEXTURE_2D, t);
        GLint tw = 0, th = 0, fmt = 0, comp = 0, minf = 0, magf = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &comp);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
        char b[200]; snprintf(b, sizeof b, "tex %u: %dx%d internal %#x%s min %#x mag %#x\n", t, tw, th, fmt, comp ? " compressed" : "", minf, magf);
        ti += b;
        bool depth = fmt == 0x88f0 || fmt == 0x81a5 || fmt == 0x81a6 || fmt == 0x81a7 || fmt == 0x8cac || fmt == 0x8cad || fmt == GL_DEPTH_COMPONENT;
        if (!depth && tw > 0 && th > 0 && tw <= 512 && th <= 512 && saved < 150) {
            std::vector<uint8_t> px((size_t)tw * th * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
            WriteTGA(g_capDir + "\\tex_" + std::to_string(t) + ".tga", tw, th, px);
            saved++;
        }
    }
    o_BindTexture(GL_TEXTURE_2D, t0);
    if (o_ActiveTexture) o_ActiveTexture(atex);
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    if (bindbuf && pbo) bindbuf(GL_PIXEL_PACK_BUFFER, pbo);
    if (bindfb) bindfb(GL_READ_FRAMEBUFFER, rfb);
    glReadBuffer(rb);
    while (glGetError() != GL_NO_ERROR) {}
    WriteFile_(g_capDir + "\\textures.txt", ti);
    WriteFile_(g_capDir + "\\draws.txt", g_draws);
    // the programs' shader sources
    for (GLuint p : g_capProgs) {
        if (!p) continue;
        std::string s;
        for (GLuint sh : g_progShaders[p]) {
            auto it = g_shaders.find(sh);
            if (it == g_shaders.end()) continue;
            s += "// ===== " + std::string(TypeName(it->second.type)) + " shader " + std::to_string(sh) + (it->second.ok ? "" : " (FAILED)") + "\n";
            s += it->second.src + "\n";
            if (!it->second.log.empty()) s += "/* compile log:\n" + it->second.log + "\n*/\n";
        }
        WriteFile_(g_capDir + "\\program_" + std::to_string(p) + ".glsl", s);
    }
    GLog("capture: %d draws, %u programs, %u textures -> %s", g_drawCount, (unsigned)g_capProgs.size(), (unsigned)g_capTex.size(), g_capDir.c_str());
    Log("gldiag: frame captured to %s", g_capDir.c_str());
    MessageBeep(MB_OK);
}
}  // namespace

void GLDiag_Init(const char* binDir) {
    if (!g_cfg.glDiag) return;
    snprintf(g_dir, sizeof g_dir, "%s\\NWNPad-diag", binDir);
    CreateDirectoryA(g_dir, nullptr);
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\NWNPad-gl.log", g_dir);
    g_log = fopen(p, "w");
    GLog("NWNPad GL diagnostics (F11 or both stick clicks held 1 s: capture a frame)");
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { GLog("MinHook init failed %d", st); return; }
    HMODULE gl = LoadLibraryA("opengl32.dll");
    if (!gl) { GLog("opengl32.dll not loaded"); return; }
    struct H { const char* n; void* d; void** o; } hs[] = {
        {"wglGetProcAddress", (void*)hk_wglGPA, (void**)&o_wglGPA},
        {"glDrawArrays", (void*)w_DrawArrays, (void**)&o_DrawArrays},
        {"glDrawElements", (void*)w_DrawElements, (void**)&o_DrawElements},
        {"glBindTexture", (void*)w_BindTexture, (void**)&o_BindTexture},
    };
    for (auto& h : hs) {
        void* t = (void*)GetProcAddress(gl, h.n);
        if (!t || MH_CreateHook(t, h.d, h.o) != MH_OK || MH_EnableHook(t) != MH_OK) GLog("hook %s FAILED", h.n);
    }
    Log("gldiag: on, writing to %s", g_dir);
}

// before the overlay is drawn: the recorded frame is complete
// black-frame detector: brightness of a few back-buffer pixels before and after the overlay; a frame far
// darker than the frames before it is logged with what the pad UI was doing
static float g_avgPre = -1, g_lastPre = 0;
static float SampleBack() {
    static auto bindfb = GLP<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer");
    if (!bindfb) return -1;
    GLint rfb = 0, vp[4]; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb); glGetIntegerv(GL_VIEWPORT, vp);
    GLint rb; glGetIntegerv(GL_READ_BUFFER, &rb);
    bindfb(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    RECT rc; HWND w = GetActiveWindow(); int W = 1920, H = 1080;
    if (w && GetClientRect(w, &rc)) { W = rc.right; H = rc.bottom; }
    float sum = 0; int n = 0;
    for (int j = 1; j <= 5; j++) for (int i = 1; i <= 5; i++) {
        unsigned char p[4] = {};
        glReadPixels(W * i / 6, H * j / 6, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        sum += 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]; n++;
    }
    bindfb(GL_READ_FRAMEBUFFER, rfb);
    glReadBuffer(rb);
    return sum / n;
}
std::string UI_DebugState();
static void CheckBlack(bool post) {
    float v = SampleBack();
    if (v < 0) return;
    if (!post) {
        g_lastPre = v;
        if (g_avgPre > 12 && v < g_avgPre * 0.25f)
            GLog("BLACK frame before overlay: %.1f (usual %.1f) ui: %s", v, g_avgPre, UI_DebugState().c_str());
        g_avgPre = g_avgPre < 0 ? v : g_avgPre * 0.9f + v * 0.1f;
    } else {
        static float avgPost = -1;
        if (avgPost > 12 && v < avgPost * 0.25f)
            GLog("BLACK frame after overlay: %.1f (usual %.1f, before overlay %.1f) ui: %s", v, avgPost, g_lastPre, UI_DebugState().c_str());
        avgPost = avgPost < 0 ? v : avgPost * 0.9f + v * 0.1f;
    }
}

void GLDiag_PreOverlay() {
    if (!g_cfg.glDiag || !o_wglGPA) return;
    static bool first = true;
    if (first) { first = false; OnFirstFrame(); }
    if (g_cap == CAP_REC) { g_cap = CAP_IDLE; FinishCapture(); }
    CheckBlack(false);
}

// after the overlay, right before the swap: arm the recording of the next frame
void GLDiag_PostOverlay() {
    if (!g_cfg.glDiag || !o_wglGPA) return;
    CheckBlack(true);
    if (g_cap == CAP_IDLE && Trigger()) {
        SYSTEMTIME t; GetLocalTime(&t);
        char d[MAX_PATH]; snprintf(d, sizeof d, "%s\\capture-%02d%02d%02d", g_dir, t.wHour, t.wMinute, t.wSecond);
        CreateDirectoryA(d, nullptr);
        g_capDir = d;
        g_draws.clear(); g_drawCount = 0; g_capProgs.clear(); g_capTex.clear(); g_lastUni.clear();
        g_cap = CAP_REC;          // records the next frame
        GLog("capture armed");
    }
}

bool GLDiag_Capturing() { return g_cap == CAP_REC; }

// with the gamepad features off nothing else hooks SwapBuffers
static BOOL(WINAPI* o_Swap)(HDC) = nullptr;
static BOOL WINAPI hk_Swap(HDC dc) { GLDiag_PreOverlay(); GLDiag_PostOverlay(); return o_Swap(dc); }
void GLDiag_HookSwap() {
    if (!g_cfg.glDiag) return;
    void* sb = (void*)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SwapBuffers");
    if (sb && MH_CreateHook(sb, (void*)hk_Swap, (void**)&o_Swap) == MH_OK && MH_EnableHook(sb) == MH_OK) GLog("own SwapBuffers hook");
}

