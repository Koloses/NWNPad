// NWNPad - hub "Map" page: explored tiles of the current area, hero / party markers and map notes
#include "hub_pages.h"
#include "config.h"
#include "questmark.h"
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
int g_sel = -1;             // selection in the list: quests first, then map notes
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

// what the map lists: active quests (with a lead in this area or an exit towards it) and map notes
struct Entry { bool quest; std::string text; Vec3 pos; bool hasPos; QuestMark q; };
std::vector<Entry> Entries(void* ar) {
    std::vector<Entry> v;
    if (g_cfg.questMarker)
        for (auto& q : Quest_All()) v.push_back({true, q.title, q.point, q.found, q});
    for (auto& p : Pins(ar)) v.push_back({false, p.text, p.pos, true, QuestMark()});
    return v;
}


void Open() {
    void* ar = Area();
    uint32_t id = ar ? F<uint32_t>(ar, OFS::Area_Id) : 0;
    if (id != g_area) { g_area = id; g_zoom = 1.0f; g_sel = -1; }
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
    // quests and notes: d-pad cycles and centres on them, A follows the chosen quest
    std::vector<Entry> ents = Entries(ar);
    int n = (int)ents.size();
    bool dn = g_pad.Repeat(PB_DDOWN), up = g_pad.Repeat(PB_DUP);
    if (n > 0 && (dn || up)) {
        if (dn) g_sel = g_sel < 0 ? 0 : (g_sel + 1) % n;
        else g_sel = g_sel < 0 ? n - 1 : (g_sel + n - 1) % n;
        if (ents[g_sel].hasPos) {
            g_cx = ents[g_sel].pos.x; g_cy = ents[g_sel].pos.y; g_follow = false;
            if (g_zoom < 2.0f) g_zoom = 2.0f;
        }
    }
    if (g_sel >= n) g_sel = n - 1;
    if (g_sel >= 0 && g_sel < n && ents[g_sel].quest && g_pad.Pressed(PB_A)) {
        const QuestMark& q = ents[g_sel].q;
        Quest_Track(q.followed && Quest_FollowChosen() ? std::string() : q.tag);
    }
    if (g_pad.Pressed(PB_Y)) { g_follow = true; g_sel = -1; }
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
    // map notes and quest markers, names above them (a label that would cover another is left out)
    std::vector<Entry> ents = Entries(ar);
    std::vector<std::pair<Vec3, Vec3>> taken;              // label rectangles (x0, y0) - (x1, y1)
    auto label = [&](float cx, float cy, const std::string& t, Col tc, bool force, float fs) {
        float lw = TextWidth(FONT_BODY_BOLD, fs, t.c_str());
        float x0 = cx - lw * 0.5f - px(7), y0 = cy - fs - px(14);
        x0 = fminf(fmaxf(x0, mx + px(4)), mx + mw - lw - px(18));          // keep it inside the map
        if (y0 < my + px(4)) y0 = cy + px(12);                               // no room above: below the marker
        float x1 = x0 + lw + px(14), y1 = y0 + fs + px(8);
        if (!force) for (auto& r : taken) if (x0 < r.second.x && x1 > r.first.x && y0 < r.second.y && y1 > r.first.y) return;
        taken.push_back({Vec3{x0, y0, 0}, Vec3{x1, y1, 0}});
        RoundRect(x0, y0, x1 - x0, y1 - y0, px(6), Col(10, 8, 6, (int)(205 * a)));
        Text(FONT_BODY_BOLD, fs, x0 + px(7), y0 + px(3), t.c_str(), tc.alpha(a));
    };
    auto diamond = [&](float qx, float qy, float r, Col c) {
        Triangle(qx, qy - r * 1.3f, qx + r, qy, qx, qy + r * 1.3f, c);
        Triangle(qx, qy - r * 1.3f, qx - r, qy, qx, qy + r * 1.3f, c);
    };
    // selected / followed first so their labels win
    std::vector<int> order;
    for (int i = 0; i < (int)ents.size(); i++) if (i == g_sel || (ents[i].quest && ents[i].q.followed)) order.push_back(i);
    for (int i = 0; i < (int)ents.size(); i++) if (!(i == g_sel || (ents[i].quest && ents[i].q.followed))) order.push_back(i);
    for (int i : order) {
        const Entry& e = ents[i];
        if (!e.hasPos) continue;
        float ex = ox + e.pos.x * sc, ey = oy - e.pos.y * sc;
        bool sel = i == g_sel;
        if (e.quest) {
            Col qc = e.q.here ? Col(255, 168, 40) : Col(90, 200, 255);
            float r = px(e.q.followed ? 8 : 6);
            if (e.q.followed) Ring(ex, ey, r * 2.0f + px(3) * (0.5f + 0.5f * sinf((float)g_pad.now * 4.0f)), px(2), qc.alpha(0.6f * a));
            diamond(ex, ey, r + px(2), Col(20, 14, 6, (int)(240 * a)));
            diamond(ex, ey, r, (sel ? Col(255, 244, 222) : qc).alpha(a));
            label(ex, ey - r * 0.6f, e.q.here ? e.text : e.text + "  (exit)", e.q.here ? Col(255, 200, 120) : Col(160, 220, 255), sel || e.q.followed, px(16));
        } else {
            Circle(ex, ey, px(sel ? 9 : 7), Col(30, 20, 10, (int)(230 * a)));
            Circle(ex, ey, px(sel ? 7 : 5), (sel ? Theme::LineStrong : Theme::Gold).alpha(a));
            label(ex, ey, e.text, sel ? Col(255, 244, 222) : Theme::Text, sel, px(sel ? 17 : 15));
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
    // quests (followed one marked) and map notes, one scrolling list
    {
        float rowH = px(42);
        float listTop = iy, listBottom = y + h - px(30);
        int nq = 0; for (auto& e : ents) nq += e.quest;
        int np = (int)ents.size() - nq;
        int vis = (int)((listBottom - listTop - px(76)) / rowH);
        if (vis < 3) vis = 3;
        int first = g_sel >= vis ? g_sel - vis + 1 : 0;
        float ry = listTop;
        bool hdrQ = false, hdrP = false;
        if (nq == 0 && g_cfg.questMarker) {
            Text(FONT_TITLE, px(22), ix, ry, "Quests", Theme::GoldDim.alpha(a)); ry += px(34);
            Text(FONT_BODY, px(18), ix, ry, "No active quests.", Theme::TextFaint.alpha(a)); ry += px(36);
        }
        for (int i = first; i < (int)ents.size() && i < first + vis; i++) {
            const Entry& e = ents[i];
            if (e.quest && !hdrQ) { hdrQ = true; Text(FONT_TITLE, px(22), ix, ry, "Quests", Theme::GoldDim.alpha(a)); ry += px(34); }
            if (!e.quest && !hdrP) { hdrP = true; if (hdrQ) ry += px(6); Text(FONT_TITLE, px(22), ix, ry, "Map notes", Theme::GoldDim.alpha(a)); ry += px(34); }
            if (ry + rowH > listBottom) break;
            UIK::RowSel(ix - px(6), ry, iw + px(6), rowH - px(4), i == g_sel);
            float my = ry + rowH * 0.5f - px(2);
            if (e.quest) {
                Col qc = !e.q.found ? Theme::TextFaint : e.q.here ? Col(255, 168, 40) : Col(90, 200, 255);
                float r = px(5);
                Triangle(ix + px(10), my - r * 1.3f, ix + px(10) + r, my, ix + px(10), my + r * 1.3f, qc.alpha(a));
                Triangle(ix + px(10), my - r * 1.3f, ix + px(10) - r, my, ix + px(10), my + r * 1.3f, qc.alpha(a));
                float tw = iw - px(34);
                if (e.q.followed) {
                    const char* tag = Quest_FollowChosen() ? "Following" : "Latest";
                    float fw = TextWidth(FONT_BODY_BOLD, px(15), tag) + px(14);
                    RoundRect(ix + iw - fw, ry + px(8), fw, px(24), px(6), Theme::Gold.alpha(0.85f * a));
                    Text(FONT_BODY_BOLD, px(15), ix + iw - fw + px(7), ry + px(11), tag, Col(30, 22, 12, (int)(255 * a)));
                    tw -= fw + px(6);
                }
                TextFit(FONT_BODY, px(19), ix + px(26), ry + px(9), tw, e.text.c_str(), (e.q.found ? Theme::Text : Theme::TextDim).alpha(a));
            } else {
                Circle(ix + px(10), my, px(4), Theme::Gold.alpha(a));
                TextFit(FONT_BODY, px(19), ix + px(26), ry + px(9), iw - px(34), e.text.c_str(), Theme::Text.alpha(a));
            }
            ry += rowH;
        }
        if (np == 0 && ry + px(70) < listBottom) {
            if (!hdrP) { ry += px(6); Text(FONT_TITLE, px(22), ix, ry, "Map notes", Theme::GoldDim.alpha(a)); ry += px(34); }
            TextWrap(FONT_BODY, px(18), ix, ry, iw, "No notes in this area.", Theme::TextFaint.alpha(a));
        }
    }
}

int Hints(HintItem* o) {
    int n = 0;
    o[n++] = {GL_LS, "Pan"};
    o[n++] = {GL_RT, "Zoom"};
    o[n++] = {GL_DPAD, "Quests and notes"};
    void* ar = Area();
    if (ar && g_sel >= 0) {
        std::vector<Entry> e = Entries(ar);
        if (g_sel < (int)e.size() && e[g_sel].quest) o[n++] = {GL_A, e[g_sel].q.followed && Quest_FollowChosen() ? "Unfollow" : "Follow"};
    }
    o[n++] = {GL_Y, "Centre"};
    return n;
}
}  // namespace

const HubPage PAGE_MAP = {Open, Input, Draw, Hints, nullptr};

// HUD minimap: square around the hero, turned with the camera (what is ahead of the camera is up;
// explored tiles, notes, party, hostiles, quest marker), north marker on the rim
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
    float mcx = x + size * 0.5f, mcy = y + size * 0.5f;
    // camera forward on the ground = screen up (the camera yaw is in degrees)
    float yaw = CameraYaw() * 0.01745329f;
    float fx = -sinf(yaw), fy = cosf(yaw);
    auto W2S = [&](float wx, float wy, float& sx, float& sy) {
        float dx = wx - pp.x, dy = wy - pp.y;
        float fwd = dx * fx + dy * fy, right = dx * fy - dy * fx;
        sx = mcx + right * sc; sy = mcy - fwd * sc;
    };
    auto ScreenAngle = [&](float wx, float wy) {      // world direction -> screen angle (0 = up, clockwise)
        float fwd = wx * fx + wy * fy, right = wx * fy - wy * fx;
        return atan2f(right, fwd);
    };
    PushClip(x + px(3), y + px(3), size - px(6), size - px(6));
    float R2 = VIEW * 0.75f;                          // tiles within the rotated square's reach
    int tx0 = std::max(0, (int)((pp.x - R2) / TILE_M)), tx1 = std::min(tw - 1, (int)((pp.x + R2) / TILE_M));
    int ty0 = std::max(0, (int)((pp.y - R2) / TILE_M)), ty1 = std::min(th - 1, (int)((pp.y + R2) / TILE_M));
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            char* t = tiles + (size_t)(ty * tw + tx) * 0x60;
            if (!F<int>(t, 0x30)) continue;
            void* td = F<void*>(t, 0x40);
            uint32_t tex = 0;
            if (td && Readable((char*)td + 0x21, 16)) { char ref[17] = {}; memcpy(ref, (char*)td + 0x21, 16); tex = Res_Texture(ref); }
            // corners as drawn north-up: top-left = (x0, y1), top-right = (x1, y1), bottom-right = (x1, y0), bottom-left = (x0, y0)
            float e = 0.02f;                          // a hair of overlap against seams
            float X0 = tx * TILE_M - e, X1 = (tx + 1) * TILE_M + e, Y0 = ty * TILE_M - e, Y1 = (ty + 1) * TILE_M + e;
            float qx[4], qy[4];
            W2S(X0, Y1, qx[0], qy[0]); W2S(X1, Y1, qx[1], qy[1]); W2S(X1, Y0, qx[2], qy[2]); W2S(X0, Y0, qx[3], qy[3]);
            ImageQuad(tex, qx, qy, tex ? (F<int>(t, 0x24) & 3) : 0, tex ? Col(236, 222, 196, (int)(235 * a)) : Col(70, 62, 50, (int)(235 * a)));
        }
    for (const Pin& p : Pins(ar)) {
        float px0, py0; W2S(p.pos.x, p.pos.y, px0, py0);
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
        if (fabsf(q.x - pp.x) > R2 || fabsf(q.y - pp.y) > R2) continue;
        bool party = GD::MasterId(o) == pid;
        int r = GD::Reaction(o);
        float cx, cy; W2S(q.x, q.y, cx, cy);
        if (!party && r != 2) {          // other people: small neutral dot
            Circle(cx, cy, px(3.5f), Col(10, 8, 6, (int)(200 * a)));
            Circle(cx, cy, px(2.3f), Col(214, 200, 160, (int)(220 * a)));
            continue;
        }
        Circle(cx, cy, px(5), Col(10, 8, 6, (int)(230 * a)));
        Circle(cx, cy, px(3.5f), (party ? Theme::Friendly : Theme::Hostile).alpha(a));
    }
    // quest marker: the target, or the exit towards it; outside the map an arrow on the rim
    Vec3 qp; bool qhere = false;
    bool quest = g_cfg.questMarker && Quest_Marker(qp, qhere);
    if (quest) {
        float qx, qy; W2S(qp.x, qp.y, qx, qy);
        float cx = mcx, cy = mcy, half = size * 0.5f - px(14);
        float dx = qx - cx, dy = qy - cy;
        float m = fmaxf(fabsf(dx), fabsf(dy));
        Col qc = qhere ? Col(255, 168, 40) : Col(90, 200, 255);
        float pulse = 0.75f + 0.25f * sinf((float)g_pad.now * 4.0f);
        if (m <= half) {
            float r = px(7);
            Triangle(qx, qy - r * 1.3f, qx + r, qy, qx, qy + r * 1.3f, Col(20, 14, 6, (int)(240 * a)));
            Triangle(qx, qy - r * 1.3f, qx - r, qy, qx, qy + r * 1.3f, Col(20, 14, 6, (int)(240 * a)));
            r = px(5);
            Triangle(qx, qy - r * 1.3f, qx + r, qy, qx, qy + r * 1.3f, qc.alpha(a * pulse));
            Triangle(qx, qy - r * 1.3f, qx - r, qy, qx, qy + r * 1.3f, qc.alpha(a * pulse));
        } else {
            float k = half / m;
            float ax = cx + dx * k, ay = cy + dy * k;
            float ang = atan2f(dx, -dy);
            Arrow(ax, ay, px(11), ang, Col(20, 14, 6, (int)(255 * a)));
            Arrow(ax, ay, px(8), ang, qc.alpha(a * pulse));
        }
    }
    // hero, facing
    Vec3 o = F<Vec3>(pl, OFS::Obj_Orient);
    float face = ScreenAngle(o.x, o.y);
    Arrow(mcx, mcy, px(12), face, Col(20, 14, 6, (int)(255 * a)));
    Arrow(mcx, mcy, px(9), face, Theme::LineStrong.alpha(a));
    PopClip();
    RoundRectBorder(x, y, size, size, px(14), px(1.5f), Theme::Line.alpha(a));
    // north marker where north meets the frame
    {
        float ang = ScreenAngle(0, 1);
        float dx = sinf(ang), dy = -cosf(ang);
        float k = (size * 0.5f) / fmaxf(fabsf(dx), fabsf(dy));
        float nx = mcx + dx * k, ny = mcy + dy * k;
        Circle(nx, ny, px(11), Col(20, 16, 12, (int)(240 * a)));
        Ring(nx, ny, px(11), px(1.2f), Theme::Line.alpha(a));
        TextCentered(FONT_TITLE, px(14), nx, ny - px(8), "N", Theme::Gold.alpha(a));
    }
    // tracked quest under the map
    float below = 0;
    if (quest) {
        std::string qn = Quest_Name();
        if (!qn.empty()) {
            float fs = px(16);
            Col qc2 = (qhere ? Col(255, 188, 90) : Col(150, 215, 255)).alpha(a);
            if (TextWidth(FONT_BODY, fs, qn.c_str()) > size) TextFit(FONT_BODY, fs, x, y + size + px(30), size, qn.c_str(), qc2);
            else TextCentered(FONT_BODY, fs, x + size * 0.5f, y + size + px(30), qn.c_str(), qc2);
        }
    }
    (void)below;
    // area name under the map
    std::string name = F<char*>(ar, 0x238) ? G2UClean(F<char*>(ar, 0x238), F<int>(ar, 0x240)) : std::string();
    if (!name.empty()) {
        float fs = px(17);
        float w = TextWidth(FONT_BODY_BOLD, fs, name.c_str());
        if (w > size) { TextFit(FONT_BODY_BOLD, fs, x, y + size + px(8), size, name.c_str(), Theme::TextDim.alpha(a)); }
        else TextCentered(FONT_BODY_BOLD, fs, x + size * 0.5f, y + size + px(8), name.c_str(), Theme::TextDim.alpha(a));
    }
}

