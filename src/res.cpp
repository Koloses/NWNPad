// NWNPad - game resource access (CExoResMan) and texture cache for icons/portraits
#include "res.h"
#include "render.h"
#include "log.h"
#include "text.h"
#include <windows.h>
#include <unordered_map>
#include <string>
#include <cstring>
#include <cstdio>
#include <GL/gl.h>
#include <GL/glext.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "stb_image.h"

namespace {
void** p_resman = nullptr;
void** p_tlk = nullptr;
typedef void* (*GetResObject_t)(void*, const char* resref16, unsigned short type);
typedef void* (*CResDemand_t)(void*);
typedef int (*CResGetSize_t)(void*);
typedef int (*CResRelease_t)(void*);
typedef int (*ReleaseResObject_t)(void*, void*, bool);
typedef void* (*GetSimpleString_t)(void*, void* ret, unsigned);
typedef void (*ExoStrDtor_t)(void*);
GetResObject_t fGetResObject; CResDemand_t fDemand; CResGetSize_t fGetSize; CResRelease_t fRelease;
ReleaseResObject_t fReleaseResObject; GetSimpleString_t fGetSimpleString; ExoStrDtor_t fExoStrDtor;
typedef void (*Get_t)(void* resman, void* ret, const char* ref, unsigned short type);
Get_t fGet;

struct TexEntry { uint32_t tex; int w, h; };
std::unordered_map<std::string, TexEntry> g_texCache;
}

void Res_Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    p_resman = (void**)GetProcAddress(exe, "?g_pExoResMan@@3PEAVCExoResMan@@EA");
    p_tlk = (void**)GetProcAddress(exe, "?g_pTlkTable@@3PEAVCTlkTable@@EA");
    fGetResObject = (GetResObject_t)GetProcAddress(exe, "?GetResObject@CExoResMan@@QEAAPEAVCRes@@AEBVCResRef@@G@Z");
    fDemand = (CResDemand_t)GetProcAddress(exe, "?Demand@CRes@@QEAAPEAXXZ");
    fGetSize = (CResGetSize_t)GetProcAddress(exe, "?GetSize@CRes@@QEAAHXZ");
    fRelease = (CResRelease_t)GetProcAddress(exe, "?Release@CRes@@QEAAHXZ");
    fReleaseResObject = (ReleaseResObject_t)GetProcAddress(exe, "?ReleaseResObject@CExoResMan@@QEAAHPEAVCRes@@_N@Z");
    fGetSimpleString = (GetSimpleString_t)GetProcAddress(exe, "?GetSimpleString@CTlkTable@@QEAA?AVCExoString@@I@Z");
    fExoStrDtor = (ExoStrDtor_t)GetProcAddress(exe, "??1CExoString@@QEAA@XZ");
    fGet = (Get_t)GetProcAddress(exe, "?Get@CExoResMan@@QEAA?AV?$shared_ptr@VDataBlock@@@std@@AEBVCResRef@@G@Z");
    Log("res: resman %p tlk %p fns %p %p %p %p %p %p", p_resman, p_tlk, (void*)fGetResObject, (void*)fDemand,
        (void*)fGetSize, (void*)fRelease, (void*)fReleaseResObject, (void*)fGetSimpleString);
}

bool Res_Load(const char* resref, unsigned short type, std::vector<uint8_t>& out) {
    out.clear();
    if (!p_resman || !*p_resman || !fGet || !resref || !resref[0]) return false;
    char ref[17] = {};
    for (int i = 0; i < 16 && resref[i]; i++) ref[i] = (char)tolower((unsigned char)resref[i]);
    struct SP { void* ptr; void* ctrl; } sp = {nullptr, nullptr};
    fGet(*p_resman, &sp, ref, type);
    if (!sp.ptr) return false;
    // DataBlock -> shared buffer { uint8_t* data; size_t size; size_t cap; ... }
    void* shared = *(void**)sp.ptr;
    if (shared) {
        uint8_t* data = *(uint8_t**)shared;
        size_t size = *(size_t*)((char*)shared + 8);
        if (data && size > 0 && size < (256u << 20)) out.assign(data, data + size);
    }
    // release the shared_ptr (MSVC _Ref_count_base: vtbl, uses, weaks)
    if (sp.ctrl) {
        volatile long* uses = (volatile long*)((char*)sp.ctrl + 8);
        volatile long* weaks = (volatile long*)((char*)sp.ctrl + 12);
        void** vt = *(void***)sp.ctrl;
        if (InterlockedDecrement(uses) == 0) {
            ((void (*)(void*))vt[0])(sp.ctrl);                 // _Destroy
            if (InterlockedDecrement(weaks) == 0) ((void (*)(void*))vt[1])(sp.ctrl);   // _Delete_this
        }
    }
    return !out.empty();
}

std::string Tlk(uint32_t strref) {
    if (!p_tlk || !*p_tlk || !fGetSimpleString || strref == 0xffffffffu) return std::string();
    struct { char* p; uint32_t len, cap; } s = {nullptr, 0, 0};
    fGetSimpleString(*p_tlk, &s, strref);
    std::string r = s.p ? G2UClean(s.p, (int)s.len) : std::string();
    if (fExoStrDtor) fExoStrDtor(&s);
    return r;
}

// ---- texture decoding ------------------------------------------------------------------------
static uint32_t UploadDXT(const uint8_t* d, size_t n, int w, int h, bool dxt5, int& outW, int& outH) {
    int fmt = dxt5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    int block = dxt5 ? 16 : 8;
    int sizes[16]; int mips = 0; size_t used = 0; int mw = w, mh = h;
    while (mips < 16) {
        int sz = ((mw + 3) / 4) * ((mh + 3) / 4) * block;
        if (used + sz > n) break;
        sizes[mips++] = sz; used += sz;
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
    }
    if (!mips) return 0;
    outW = w; outH = h;
    return R::CreateTextureCompressed(w, h, fmt, d, (int)used, mips, sizes);
}

static uint32_t DecodeDDS(const std::vector<uint8_t>& v, int& w, int& h) {
    if (v.size() < 24) return 0;
    const uint8_t* d = v.data();
    if (memcmp(d, "DDS ", 4) == 0 && v.size() > 128) {
        uint32_t hh = *(uint32_t*)(d + 12), ww = *(uint32_t*)(d + 16);
        uint32_t four = *(uint32_t*)(d + 84);
        bool dxt5 = four == 0x35545844 || four == 0x34545844;   // DXT5/DXT4
        bool dxt1 = four == 0x31545844;
        if (!dxt1 && !dxt5) return 0;
        return UploadDXT(d + 128, v.size() - 128, ww, hh, dxt5, w, h);
    }
    // BioWare DDS: width, height, colours (3 = DXT1, 4 = DXT5), base level size, alpha mean (float)
    uint32_t ww = *(uint32_t*)d, hh = *(uint32_t*)(d + 4), bpp = *(uint32_t*)(d + 8);
    if (ww == 0 || hh == 0 || ww > 4096 || hh > 4096 || (bpp != 3 && bpp != 4)) return 0;
    return UploadDXT(d + 20, v.size() - 20, ww, hh, bpp == 4, w, h);
}

static uint32_t DecodeTGA(const std::vector<uint8_t>& v, int& w, int& h) {
    int n = 0;
    unsigned char* px = stbi_load_from_memory(v.data(), (int)v.size(), &w, &h, &n, 4);
    if (!px) return 0;
    uint32_t t = R::CreateTexture(w, h, px);
    stbi_image_free(px);
    return t;
}

// textures load on the render thread: a few milliseconds' worth per frame, the rest on later frames
// (an area's map tiles, a new portrait or icon set would otherwise all load in one frame)
static double g_loadMs = 0;
static int g_loads = 0;
void Res_NewFrame() { g_loadMs = 0; g_loads = 0; }
static bool LoadBudget() { return g_loads == 0 || g_loadMs < 3.0; }
static double NowMs() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / f.QuadPart; }
struct LoadTimer { double t0 = NowMs(); ~LoadTimer() { g_loadMs += NowMs() - t0; g_loads++; } };

uint32_t Res_Texture(const char* resref, int* outW, int* outH) {
    if (!resref || !resref[0]) return 0;
    char key0[40]; size_t n = 0;
    for (; resref[n] && n < sizeof key0 - 1; n++) key0[n] = (char)tolower((unsigned char)resref[n]);
    key0[n] = 0;
    std::string key(key0, n);
    auto it = g_texCache.find(key);
    if (it != g_texCache.end()) { if (outW) *outW = it->second.w; if (outH) *outH = it->second.h; return it->second.tex; }
    if (!LoadBudget()) { if (outW) *outW = 0; if (outH) *outH = 0; return 0; }   // next frame
    LoadTimer lt;
    TexEntry e{0, 0, 0};
    std::vector<uint8_t> v;
    if (Res_Load(key.c_str(), 2033, v)) e.tex = DecodeDDS(v, e.w, e.h);   // DDS
    if (!e.tex && Res_Load(key.c_str(), 3, v)) e.tex = DecodeTGA(v, e.w, e.h);   // TGA
    g_texCache[key] = e;
    if (!e.tex) Log("texture '%s' not found", key.c_str());
    if (outW) *outW = e.w;
    if (outH) *outH = e.h;
    return e.tex;
}

uint32_t Res_TextureFile(const char* path, int* outW, int* outH) {
    if (!path || !path[0]) return 0;
    std::string key = std::string("file:") + path;
    auto it = g_texCache.find(key);
    if (it != g_texCache.end()) { if (outW) *outW = it->second.w; if (outH) *outH = it->second.h; return it->second.tex; }
    if (!LoadBudget()) { if (outW) *outW = 0; if (outH) *outH = 0; return 0; }   // next frame
    LoadTimer lt;
    TexEntry e{0, 0, 0};
    FILE* f = fopen(path, "rb");
    if (f) {
        std::vector<uint8_t> v;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (32 << 20)) { v.resize(n); if (fread(v.data(), 1, n, f) == (size_t)n) e.tex = DecodeTGA(v, e.w, e.h); }
        fclose(f);
    }
    g_texCache[key] = e;
    if (outW) *outW = e.w;
    if (outH) *outH = e.h;
    return e.tex;
}

// ---- PLT (palette layered textures: armour, helmets, cloaks) ---------------------------------
namespace {
struct Pal { std::vector<uint8_t> rgba; int w = 0, h = 0; bool tried = false; };
Pal g_pals[7];
const char* PAL_NAMES[7] = {"pal_skin01", "pal_hair01", "pal_armor01", "pal_armor02", "pal_cloth01", "pal_leath01", "pal_tattoo01"};
const Pal* GetPal(int i) {
    if (i < 0 || i > 6) i = 0;
    Pal& p = g_pals[i];
    if (!p.tried) {
        p.tried = true;
        std::vector<uint8_t> v;
        if (Res_Load(PAL_NAMES[i], 3, v)) {
            int n = 0;
            unsigned char* px = stbi_load_from_memory(v.data(), (int)v.size(), &p.w, &p.h, &n, 4);
            if (px) { p.rgba.assign(px, px + p.w * p.h * 4); stbi_image_free(px); }
        }
        Log("palette %s: %dx%d", PAL_NAMES[i], p.w, p.h);
    }
    return p.rgba.empty() ? nullptr : &p;
}
}

uint32_t Res_PltTexture(const std::vector<std::string>& parts, const std::vector<uint16_t>& colors, int* outW, int* outH) {
    if (parts.empty()) return 0;
    std::string key = "plt:";
    for (auto& p : parts) { key += p; key += '|'; }
    char b[16];
    for (uint16_t c : colors) { snprintf(b, sizeof b, "%x,", c); key += b; }
    for (auto& c : key) c = (char)tolower((unsigned char)c);
    auto it = g_texCache.find(key);
    if (it != g_texCache.end()) { if (outW) *outW = it->second.w; if (outH) *outH = it->second.h; return it->second.tex; }
    if (!LoadBudget()) { if (outW) *outW = 0; if (outH) *outH = 0; return 0; }   // next frame
    LoadTimer lt;
    TexEntry e{0, 0, 0};
    std::vector<uint8_t> out;
    int W = 0, H = 0;
    for (size_t pi = 0; pi < parts.size(); pi++) {
        std::vector<uint8_t> v;
        std::string ref = parts[pi];
        for (auto& c : ref) c = (char)tolower((unsigned char)c);
        if (!Res_Load(ref.c_str(), 6, v) || v.size() < 24 || memcmp(v.data(), "PLT V1", 6) != 0) continue;
        int w = *(int*)(v.data() + 16), h = *(int*)(v.data() + 20);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || v.size() < 24 + (size_t)w * h * 2) continue;
        if (out.empty()) { W = w; H = h; out.assign((size_t)W * H * 4, 0); }
        if (w != W || h != H) continue;
        const uint16_t* cols = colors.size() >= (pi + 1) * 10 ? &colors[pi * 10] : (colors.size() >= 10 ? &colors[0] : nullptr);
        const uint8_t* px = v.data() + 24;
        for (int y = 0; y < h; y++) {
            int dy = h - 1 - y;                // PLT rows are stored bottom-up
            for (int x = 0; x < w; x++) {
                uint8_t val = px[(y * w + x) * 2], layer = px[(y * w + x) * 2 + 1];
                if (layer > 9) layer = 0;
                uint16_t c = cols ? cols[layer] : 0;
                const Pal* pal = GetPal(c >> 8);
                if (!pal) continue;
                int row = c & 0xff;
                if (row >= pal->h) row = pal->h - 1;
                const uint8_t* s = &pal->rgba[((size_t)row * pal->w + (val < pal->w ? val : pal->w - 1)) * 4];
                uint8_t* d = &out[((size_t)dy * W + x) * 4];
                float sa = s[3] / 255.0f, da = d[3] / 255.0f;
                float oa = sa + da * (1 - sa);
                if (oa <= 0) continue;
                for (int k = 0; k < 3; k++) d[k] = (uint8_t)((s[k] * sa + d[k] * da * (1 - sa)) / oa);
                d[3] = (uint8_t)(oa * 255);
            }
        }
    }
    if (!out.empty()) { e.tex = R::CreateTexture(W, H, out.data()); e.w = W; e.h = H; }
    g_texCache[key] = e;
    if (!e.tex) Log("plt texture %s not found", parts[0].c_str());
    if (outW) *outW = e.w;
    if (outH) *outH = e.h;
    return e.tex;
}
