// NWNPad - hub "Map" page: explored tiles of the current area, hero / party markers and map notes
#include "hub_pages.h"
#include "hub.h"
#include "render.h"
#include "game.h"
#include "gamedata.h"
#include "res.h"
#include "text.h"
#include "pad.h"
#include "log.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

using namespace G;
using namespace R;

namespace {
// CNWCArea (Win): width +0xc, height +0x10 (tiles), name CExoString +0x238, tiles +0x298 (0x60 each),
//   map pins +0x2b8 / count +0x2c0
// CNWCTile: orientation +0x24, explored +0x30, tile data +0x40 -> map icon resref +0x21
// CNWCMapPin: position (object) +0x38, enabled +0x170, note CExoString +0x178
const float TILE_M = 10.0f;

struct Pin { std::string text; Vec3 pos; };
float g_zoom = 1.0f;        // 1 = whole area fits
float g_cx = -1, g_cy = -1; // view centre in metres (-1 = follow the hero)
bool g_follow = true;
int g_pinSel = -1;
uint32_t g_area = 0;

void* Area() { void* pl = Player(); return pl ? F<void*>(pl, OFS::Obj_Area) : nullptr; }

std::vector<Pin> Pins(void* ar) {
    std::vector<Pin> v;
    int n = F<int>(ar, 0x2c0);
    void** arr = F<void**>(ar, 0x2b8);
    if (!arr || n <= 0 || n > 500) return v;
    for (int i = 0; i < n; i++) {
        void* p = arr[i];
        if (!p || !Readable(p, 0x190) || !F<int>(p, 0x170)) continue;
        Pin q; q.text = ExoStr((char*)p + 0x178); q.pos = F<Vec3>(p, OFS::Obj_Pos);
        if (q.text.empty()) q.text = "Note";
        v.push_back(q);
    }
    return v;
}

void Open() {
    void* ar = Area();
    uint32_t id = ar ? F<uint32_t>(ar, OFS::Area_Id) : 0;
    if (id != g_area) { g_area = id; g_zoom = 1.0f; g_pinSel = -1; }
    g_follow = true;
    if (ar) Log("map: area %08x %dx%d '%s'", id, F<int>(ar, 0xc), F<int>(ar, 0x10), ExoStr((char*)ar + 0x238).c_str());
}

void Input() {
    void* ar = Area();
    if (!ar) { if (g_pad.Pressed(PB_B)) Hub_Close(); return; }
    float dt = (float)g_pad.dt;
    // zoom: triggers
    float z = 0;
    if (g_pad.Down(PB_RT)) z += 1;
    if (g_pad.Down(PB_LT)) z -= 1;
    if (z != 0) g_zoom = fminf(6.0f, fmaxf(1.0f, g_zoom * expf(z * 1.6f * dt)));
    // pan: left stick (metres per second scale with the visible area)
    float aw = F<int>(ar, 0xc) * TILE_M, ah = F<int>(ar, 0x10) * TILE_M;
    float lx = g_pad.lx, ly = g_pad.ly;
    if (fabsf(lx) > 0.2f || fabsf(ly) > 0.2f) {
        if (g_follow) { Vec3 p = Pos(Player()); g_cx = p.x; g_cy = p.y; g_follow = false; }
        float sp = fmaxf(aw, ah) / g_zoom * 0.8f;
        g_cx = fminf(aw, fmaxf(0.0f, g_cx + lx * sp * dt));
        g_cy = fminf(ah, fmaxf(0.0f, g_cy + ly * sp * dt));
    }
    // notes: d-pad cycles and centres on them
    std::vector<Pin> pins = Pins(ar);
    int n = (int)pins.size();
    bool dn = g_pad.Repeat(PB_DDOWN), up = g_pad.Repeat(PB_DUP);
    if (n > 0 && (dn || up)) {
        if (dn) g_pinSel = g_pinSel < 0 ? 0 : (g_pinSel + 1) % n;
        else g_pinSel = g_pinSel < 0 ? n - 1 : (g_pinSel + n - 1) % n;
        g_cx = pins[g_pinSel].pos.x; g_cy = pins[g_pinSel].pos.y; g_follow = false;
        if (g_zoom < 2.0f) g_zoom = 2.0f;
    }
    if (g_pinSel >= n) g_pinSel = n - 1;
    if (g_pad.Pressed(PB_Y)) { g_follow = true; g_pinSel = -1; }
    if (g_pad.Pressed(PB_X)) { g_zoom = 1.0f; g_follow = true; }
    if (g_pad.Pressed(PB_B)) Hub_Close();
}

void Arrow(float cx, float cy, float r, float ang, Col c) {
    // ang: radians, 0 = screen up, clockwise
    float s = sinf(ang), co = cosf(ang);
    auto P = [&](float x, float y, float& ox, float& oy) { ox = cx + x * co - y * s; oy = cy + x * s + y * co; };
    float x0, y0, x1, y1, x2, y2, x3, y3;
    P(0, -r, x0, y0); P(r * 0.7f, r * 0.75f, x1, y1); P(0, r * 0.35f, x2, y2); P(-r * 0.7f, r * 0.75f, x3, y3);
    Triangle(x0, y0, x1, y1, x2, y2, c);
    Triangle(x0, y0, x2, y2, x3, y3, c);
}

void Draw(float x, float y, float w, float h, float a) {
    void* ar = Area();
    void* pl = Player();
    if (!ar || !pl) return;
    int tw = F<int>(ar, 0xc), th = F<int>(ar, 0x10);
    char* tiles = F<char*>(ar, 0x298);
    if (tw <= 0 || th <= 0 || tw > 64 || th > 64 || !tiles) return;
    // layout: map canvas left, info column right
    float iw = px(400);
    float mx = x + px(36), my = y + px(18), mw = w - iw - px(36) * 2 - px(30), mh = h - px(38);
    RoundRect(mx, my, mw, mh, px(12), Col(4, 4, 3, (int)(200 * a)));
    float aw = tw * TILE_M, ah = th * TILE_M;
    float fit = fminf((mw - px(40)) / aw, (mh - px(40)) / ah);   // px per metre at zoom 1
    float sc = fit * g_zoom;
    Vec3 pp = Pos(pl);
    float cx = g_follow ? pp.x : g_cx, cy = g_follow ? pp.y : g_cy;
    if (g_zoom <= 1.001f) { cx = aw * 0.5f; cy = ah * 0.5f; }
    // keep the area on screen when zoomed
    float halfW = (mw * 0.5f) / sc, halfH = (mh * 0.5f) / sc;
    if (aw > 2 * halfW) cx = fminf(aw - halfW, fmaxf(halfW, cx)); else cx = aw * 0.5f;
    if (ah > 2 * halfH) cy = fminf(ah - halfH, fmaxf(halfH, cy)); else cy = ah * 0.5f;
    float ox = mx + mw * 0.5f - cx * sc, oy = my + mh * 0.5f + cy * sc;   // screen = (ox + X*sc, oy - Y*sc)
    PushClip(mx, my, mw, mh);
    float ts = TILE_M * sc;
    int explored = 0;
    for (int ty = 0; ty < th; ty++)
        for (int tx = 0; tx < tw; tx++) {
            char* t = tiles + (size_t)(ty * tw + tx) * 0x60;
            float sx = ox + tx * ts, sy = oy - (ty + 1) * ts;
            if (sx > mx + mw || sy > my + mh || sx + ts < mx || sy + ts < my) continue;
            if (!F<int>(t, 0x30)) continue;
            explored++;
            void* td = F<void*>(t, 0x40);
            uint32_t tex = 0;
            if (td && Readable((char*)td + 0x21, 16)) {
                char ref[17] = {}; memcpy(ref, (char*)td + 0x21, 16);
                tex = Res_Texture(ref);
            }
            int ori = F<int>(t, 0x24) & 3;
            if (tex) ImageRot(tex, sx, sy, ts + 0.5f, ts + 0.5f, ori, Col(236, 222, 196, (int)(255 * a)));
            else R::Rect(sx, sy, ts, ts, Col(70, 62, 50, (int)(255 * a)));
        }
    // pins
    std::vector<Pin> pins = Pins(ar);
    for (int i = 0; i < (int)pins.size(); i++) {
        float px0 = ox + pins[i].pos.x * sc, py0 = oy - pins[i].pos.y * sc;
        bool sel = i == g_pinSel;
        Circle(px0, py0, px(sel ? 9 : 7), Col(30, 20, 10, (int)(230 * a)));
        Circle(px0, py0, px(sel ? 7 : 5), (sel ? Theme::LineStrong : Theme::Gold).alpha(a));
        if (sel || g_zoom >= 2.5f) {
            float lw = TextWidth(FONT_BODY_BOLD, px(17), pins[i].text.c_str());
            RoundRect(px0 + px(12), py0 - px(13), lw + px(14), px(26), px(6), Col(10, 8, 6, (int)(210 * a)));
            Text(FONT_BODY_BOLD, px(17), px0 + px(19), py0 - px(11), pins[i].text.c_str(), Theme::Text.alpha(a));
        }
    }
    // party
    for (void* m : GD::PartyMembers(pl)) {
        Vec3 q = Pos(m);
        Circle(ox + q.x * sc, oy - q.y * sc, px(6), Col(20, 40, 20, (int)(230 * a)));
        Circle(ox + q.x * sc, oy - q.y * sc, px(4.5f), Theme::Friendly.alpha(a));
    }
    // hero (orientation: x/y direction in the world, screen y flipped)
    Vec3 o = F<Vec3>(pl, OFS::Obj_Orient);
    float ang = atan2f(o.x, o.y);
    float hx = ox + pp.x * sc, hy = oy - pp.y * sc;
    float pulse = 0.5f + 0.5f * sinf((float)g_pad.now * 4.0f);
    Ring(hx, hy, px(20) + px(5) * pulse, px(2.5f), Theme::Gold.alpha(0.55f * a));
    Arrow(hx, hy, px(18), ang, Col(20, 14, 6, (int)(255 * a)));
    Arrow(hx, hy, px(14), ang, Theme::LineStrong.alpha(a));
    PopClip();
    if (!explored)
        TextCentered(FONT_BODY, px(22), mx + mw * 0.5f, my + mh * 0.5f - px(12), "Nothing explored yet", Theme::TextFaint.alpha(a));
    RoundRectBorder(mx, my, mw, mh, px(12), px(1), Theme::Line.alpha(0.5f * a));
    // compass
    TextCentered(FONT_TITLE, px(20), mx + mw - px(30), my + px(12), "N", Theme::GoldDim.alpha(a));
    Triangle(mx + mw - px(30), my + px(38), mx + mw - px(36), my + px(52), mx + mw - px(24), my + px(52), Theme::GoldDim.alpha(a));

    // info column
    float ix = mx + mw + px(30), iy = y + px(22);
    std::string name = G2UClean(F<char*>(ar, 0x238) ? F<char*>(ar, 0x238) : "", F<int>(ar, 0x240));
    if (name.empty()) name = "Unknown area";
    iy += TextWrap(FONT_TITLE, px(30), ix, iy, iw, name.c_str(), Theme::Gold.alpha(a)) + px(10);
    int total = tw * th, ex = 0;
    for (int i = 0; i < total; i++) if (F<int>(tiles + (size_t)i * 0x60, 0x30)) ex++;
    char b[96]; snprintf(b, sizeof b, "Explored %d%%", total ? ex * 100 / total : 0);
    Text(FONT_BODY, px(20), ix, iy, b, Theme::TextDim.alpha(a)); iy += px(40);
    RoundRect(ix, iy, iw, px(1), 0, Theme::Line.alpha(0.4f * a)); iy += px(18);
    Text(FONT_TITLE, px(22), ix, iy, "Map notes", Theme::GoldDim.alpha(a)); iy += px(38);
    if (pins.empty()) {
        TextWrap(FONT_BODY, px(19), ix, iy, iw, "No notes in this area.", Theme::TextFaint.alpha(a));
    } else {
        float rowH = px(44);
        int vis = (int)((y + h - px(30) - iy) / rowH);
        int first = g_pinSel >= vis ? g_pinSel - vis + 1 : 0;
        for (int i = first; i < (int)pins.size() && i < first + vis; i++) {
            float ry = iy + (i - first) * rowH;
            UIK::RowSel(ix - px(6), ry, iw + px(6), rowH - px(4), i == g_pinSel);
            Circle(ix + px(10), ry + rowH * 0.5f - px(2), px(4), Theme::Gold.alpha(a));
            TextFit(FONT_BODY, px(20), ix + px(26), ry + px(9), iw - px(34), pins[i].text.c_str(), Theme::Text.alpha(a));
        }
    }
}

int Hints(HintItem* o) {
    int n = 0;
    o[n++] = {GL_LS, "Pan"};
    o[n++] = {GL_RT, "Zoom"};
    o[n++] = {GL_DPAD, "Notes"};
    o[n++] = {GL_Y, "Centre"};
    return n;
}
}  // namespace

const HubPage PAGE_MAP = {Open, Input, Draw, Hints, nullptr};

// HUD minimap: north-up square around the hero (explored tiles, notes, party, hostiles)
void Map_DrawMinimap(float x, float y, float size, float a) {
    void* ar = Area();
    void* pl = Player();
    if (!ar || !pl) return;
    int tw = F<int>(ar, 0xc), th = F<int>(ar, 0x10);
    char* tiles = F<char*>(ar, 0x298);
    if (tw <= 0 || th <= 0 || tw > 64 || th > 64 || !tiles) return;
    Shadow(x, y, size, size, px(14), px(16), Col(0, 0, 0, (int)(150 * a)));
    RoundRect(x, y, size, size, px(14), Col(6, 5, 4, (int)(215 * a)));
    const float VIEW = 44.0f;                         // metres across
    float sc = size / VIEW;
    Vec3 pp = Pos(pl);
    float ox = x + size * 0.5f - pp.x * sc, oy = y + size * 0.5f + pp.y * sc;
    PushClip(x + px(3), y + px(3), size - px(6), size - px(6));
    float ts = TILE_M * sc;
    int tx0 = std::max(0, (int)((pp.x - VIEW) / TILE_M)), tx1 = std::min(tw - 1, (int)((pp.x + VIEW) / TILE_M));
    int ty0 = std::max(0, (int)((pp.y - VIEW) / TILE_M)), ty1 = std::min(th - 1, (int)((pp.y + VIEW) / TILE_M));
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            char* t = tiles + (size_t)(ty * tw + tx) * 0x60;
            if (!F<int>(t, 0x30)) continue;
            float sx = ox + tx * ts, sy = oy - (ty + 1) * ts;
            void* td = F<void*>(t, 0x40);
            uint32_t tex = 0;
            if (td && Readable((char*)td + 0x21, 16)) { char ref[17] = {}; memcpy(ref, (char*)td + 0x21, 16); tex = Res_Texture(ref); }
            if (tex) ImageRot(tex, sx, sy, ts + 0.5f, ts + 0.5f, F<int>(t, 0x24) & 3, Col(236, 222, 196, (int)(235 * a)));
            else R::Rect(sx, sy, ts, ts, Col(70, 62, 50, (int)(235 * a)));
        }
    for (const Pin& p : Pins(ar)) {
        float px0 = ox + p.pos.x * sc, py0 = oy - p.pos.y * sc;
        Circle(px0, py0, px(5), Col(30, 20, 10, (int)(230 * a)));
        Circle(px0, py0, px(3.5f), Theme::Gold.alpha(a));
    }
    // creatures nearby: hostiles red, party green
    int n = F<int>(ar, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(ar, OFS::Area_ObjIds);
    uint32_t pid = Id(pl);
    for (int i = 0; ids && i < n && i < 5000; i++) {
        void* o = GetObj(ids[i]);
        if (!o || Type(o) != OT_CREATURE || Id(o) == pid || GD::IsDead(o)) continue;
        Vec3 q = Pos(o);
        if (fabsf(q.x - pp.x) > VIEW * 0.5f || fabsf(q.y - pp.y) > VIEW * 0.5f) continue;
        bool party = GD::MasterId(o) == pid;
        int r = GD::Reaction(o);
        float cx = ox + q.x * sc, cy = oy - q.y * sc;
        if (!party && r != 2) {          // other people: small neutral dot
            Circle(cx, cy, px(3.5f), Col(10, 8, 6, (int)(200 * a)));
            Circle(cx, cy, px(2.3f), Col(214, 200, 160, (int)(220 * a)));
            continue;
        }
        Circle(cx, cy, px(5), Col(10, 8, 6, (int)(230 * a)));
        Circle(cx, cy, px(3.5f), (party ? Theme::Friendly : Theme::Hostile).alpha(a));
    }
    // hero + camera view direction
    float hx = x + size * 0.5f, hy = y + size * 0.5f;
    Vec3 o = F<Vec3>(pl, OFS::Obj_Orient);
    Arrow(hx, hy, px(12), atan2f(o.x, o.y), Col(20, 14, 6, (int)(255 * a)));
    Arrow(hx, hy, px(9), atan2f(o.x, o.y), Theme::LineStrong.alpha(a));
    PopClip();
    RoundRectBorder(x, y, size, size, px(14), px(1.5f), Theme::Line.alpha(a));
    // north marker on the frame
    float nx = x + size * 0.5f;
    Circle(nx, y, px(11), Col(20, 16, 12, (int)(240 * a)));
    Ring(nx, y, px(11), px(1.2f), Theme::Line.alpha(a));
    TextCentered(FONT_TITLE, px(14), nx, y - px(8), "N", Theme::Gold.alpha(a));
    // area name under the map
    std::string name = F<char*>(ar, 0x238) ? G2UClean(F<char*>(ar, 0x238), F<int>(ar, 0x240)) : std::string();
    if (!name.empty()) {
        float fs = px(17);
        float w = TextWidth(FONT_BODY_BOLD, fs, name.c_str());
        if (w > size) { TextFit(FONT_BODY_BOLD, fs, x, y + size + px(8), size, name.c_str(), Theme::TextDim.alpha(a)); }
        else TextCentered(FONT_BODY_BOLD, fs, x + size * 0.5f, y + size + px(8), name.c_str(), Theme::TextDim.alpha(a));
    }
}

