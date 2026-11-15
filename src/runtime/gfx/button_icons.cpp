// On-screen button prompts as on an Xbox controller (setting xbox_buttons, on by default).
//
// The game's prompts ("PRESS [Y] FOR MISSION DESCRIPTION", SELECT CRAFT's "[X] > pilot") use
// four 32x32 RGBA8 textures it loads with the front end: the GameCube A (green), B (red) and
// the grey kidney-shaped X and Y (found by dumping memory around the Y icon, R2_TEX_DUMP and
// R2_MEM_DUMP; the C-stick icon stays). Each is replaced through Aurora's texture replacement
// registry (keyed by the XXH64 of its pixels, like a Dolphin texture pack, but with a priority
// above any pack) by a round Xbox button drawn here: the face the GameCube button is bound to
// on port 1's controller (Controls page; by default A = A, B = X, X = B, Y = Y on an Xbox pad,
// the GameCube positions), so the prompt shows the button to press.
#include "../ctx.h"
#include "../runtime.h"
#include "../settings.h"
#include "tex_dump.h"
#include "../mods.h"

#include <xxhash.h>

#include <aurora/pad.hpp>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXStruct.h>
#include <aurora/texture.hpp>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>
#include <vector>

namespace rt::gfx {
constexpr u32 kSize = 128;  // drawn at 4x the original 32x32 (the internal resolution goes to 4K)
namespace {


struct Face {
    SDL_GamepadButton button;
    char letter;
    float r, g, b;
};
// Xbox face buttons: positions as SDL names them, letters and colors as on the controller
constexpr Face kFaces[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, 'A', 0.30f, 0.72f, 0.22f},
    {SDL_GAMEPAD_BUTTON_EAST, 'B', 0.86f, 0.18f, 0.16f},
    {SDL_GAMEPAD_BUTTON_WEST, 'X', 0.13f, 0.45f, 0.90f},
    {SDL_GAMEPAD_BUTTON_NORTH, 'Y', 0.96f, 0.76f, 0.10f},
};

struct GameIcon {
    u64 hash;  // XXH64 of the 32x32 RGBA8 pixels (Dolphin name tex1_32x32_<hash>_6)
    const char* name;
    SDL_GamepadButton defaultFace;  // an Xbox pad's default binding (Aurora input/legacy.cpp)
};
constexpr GameIcon kIcons[] = {
    {0x5327238285151b94ull, "A", SDL_GAMEPAD_BUTTON_SOUTH},
    {0x3ca89f012e4cb21bull, "B", SDL_GAMEPAD_BUTTON_WEST},
    {0xf9af139ad390db09ull, "X", SDL_GAMEPAD_BUTTON_EAST},
    {0xffd2bf4300eed61aull, "Y", SDL_GAMEPAD_BUTTON_NORTH},
};

struct Seg {
    float x0, y0, x1, y1;
};

// letter strokes in a unit box (x -0.5..0.5, y -0.5..0.5 downwards)
std::vector<Seg> letterStrokes(char c) {
    std::vector<Seg> s;
    auto arc = [&](float cx, float cy, float r, float a0, float a1) {
        constexpr int n = 14;
        for (int i = 0; i < n; i++) {
            const float t0 = a0 + (a1 - a0) * float(i) / n, t1 = a0 + (a1 - a0) * float(i + 1) / n;
            s.push_back({cx + r * std::cos(t0), cy + r * std::sin(t0), cx + r * std::cos(t1), cy + r * std::sin(t1)});
        }
    };
    constexpr float pi = 3.14159265f;
    switch (c) {
    case 'A':
        s = {{-0.4f, 0.5f, 0.f, -0.5f}, {0.f, -0.5f, 0.4f, 0.5f}, {-0.24f, 0.19f, 0.24f, 0.19f}};
        break;
    case 'X':
        s = {{-0.33f, -0.5f, 0.33f, 0.5f}, {0.33f, -0.5f, -0.33f, 0.5f}};
        break;
    case 'Y':
        s = {{-0.34f, -0.5f, 0.f, 0.03f}, {0.34f, -0.5f, 0.f, 0.03f}, {0.f, 0.03f, 0.f, 0.5f}};
        break;
    case 'B':
        s = {{-0.29f, -0.5f, -0.29f, 0.5f}, {-0.29f, -0.5f, 0.04f, -0.5f}, {-0.29f, -0.02f, 0.07f, -0.02f},
             {-0.29f, 0.5f, 0.07f, 0.5f}};
        arc(0.04f, -0.26f, 0.24f, -pi / 2, pi / 2);
        arc(0.07f, 0.24f, 0.26f, -pi / 2, pi / 2);
        break;
    default:
        break;
    }
    return s;
}

float segDist(float px, float py, const Seg& s) {
    const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.f ? ((px - s.x0) * dx + (py - s.y0) * dy) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const float ex = s.x0 + t * dx - px, ey = s.y0 + t * dy - py;
    return std::sqrt(ex * ex + ey * ey);
}

// straight-alpha "over"
void over(float* dst, float r, float g, float b, float a) {
    const float da = dst[3], oa = a + da * (1.f - a);
    if (oa <= 0.f) return;
    dst[0] = (r * a + dst[0] * da * (1.f - a)) / oa;
    dst[1] = (g * a + dst[1] * da * (1.f - a)) / oa;
    dst[2] = (b * a + dst[2] * da * (1.f - a)) / oa;
    dst[3] = oa;
}

std::vector<u8> drawButton(const Face& f, u32 kSize = rt::gfx::kSize) {
    std::vector<u8> out(size_t(kSize) * kSize * 4);
    const float c = kSize * 0.5f, radius = kSize * 0.47f, rim = kSize * 0.045f;
    const auto strokes = letterStrokes(f.letter);
    const float letterH = kSize * 0.46f, half = kSize * 0.064f;
    auto letterCover = [&](float px, float py) {  // pixel coordinates
        const float ux = (px - c) / letterH, uy = (py - c) / letterH;
        float d = 1e9f;
        for (const Seg& s : strokes) d = std::min(d, segDist(ux, uy, s));
        return std::clamp(half - d * letterH + 0.5f, 0.f, 1.f);
    };
    for (u32 y = 0; y < kSize; y++)
        for (u32 x = 0; x < kSize; x++) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float dx = px - c, dy = py - c, dist = std::sqrt(dx * dx + dy * dy);
            float p[4] = {0, 0, 0, 0};
            const float body = std::clamp(radius - dist + 0.5f, 0.f, 1.f);
            if (body > 0.f) {
                // lit from the top: lighter above, darker below, a darker rim
                const float shade = 0.82f + 0.32f * (-dy / radius) * 0.5f + 0.18f * (1.f - dist / radius);
                float k = std::clamp(shade, 0.55f, 1.15f);
                if (dist > radius - rim) k *= 0.62f;
                over(p, std::min(f.r * k, 1.f), std::min(f.g * k, 1.f), std::min(f.b * k, 1.f), body);
                // gloss: a soft ellipse over the upper half
                const float gx = dx / (radius * 0.78f), gy = (py - (c - radius * 0.42f)) / (radius * 0.42f);
                const float g = 1.f - (gx * gx + gy * gy);
                if (g > 0.f) over(p, 1.f, 1.f, 1.f, std::min(g, 1.f) * 0.28f * body);
                // the letter: white, with a shadow below it for contrast
                const float shadow = letterCover(px - kSize * 0.012f, py - kSize * 0.022f);
                if (shadow > 0.f) over(p, f.r * 0.25f, f.g * 0.25f, f.b * 0.25f, shadow * 0.55f * body);
                const float lc = letterCover(px, py);
                if (lc > 0.f) over(p, 0.97f, 0.97f, 0.97f, lc * body);
            }
            u8* o = &out[(size_t(y) * kSize + x) * 4];
            for (int i = 0; i < 4; i++) o[i] = u8(std::lround(std::clamp(p[i], 0.f, 1.f) * 255.f));
        }
    return out;
}

const Face* faceFor(SDL_GamepadButton b) {
    for (const Face& f : kFaces)
        if (f.button == b) return &f;
    return nullptr;
}

// the Xbox face a GameCube button is bound to on port 1's controller (else the default)
SDL_GamepadButton boundFace(const GameIcon& icon) {
    const auto source = aurora::pad::port_controller(0);
    if (!source) return icon.defaultFace;
    const auto& c = aurora::pad::controls();
    const aurora::binding::ControlId id = icon.name[0] == 'A' ? c.a : icon.name[0] == 'B' ? c.b : icon.name[0] == 'X' ? c.x : c.y;
    for (const auto& b : aurora::pad::bindings_for_control(aurora::pad::ProfileTarget::controller(source), id))
        if (const auto* btn = b.input.control.get_if<aurora::binding::PhysicalInput::GamepadButton>())
            if (faceFor(btn->button)) return btn->button;
    return icon.defaultFace;
}

// Prompts baked into bigger textures: SELECT CRAFT's bottom row (608x48 CMPR: "[X] > pilot" at
// the left, "[C-stick] > camera" at the right). The texture is rebuilt from the game's own copy
// (or a loaded pack's DDS of it, e.g. an HD pack's 2432x192) with the GameCube icon blacked out
// and the bound Xbox button drawn over it. rect: the icon's pixels in the original texture.
// stretch: how much wider than tall the game shows the texture's pixels (the button is drawn
// that much narrower so it comes out round).
struct Baked {
    u64 hash;
    u32 w, h, fmt;
    int icon;  // kIcons index
    float x0, y0, x1, y1;
    float stretch;
};
constexpr Baked kBaked[] = {
    {0x8ce44bb872a52261ull, 608, 48, 14, 2, 12.f, 17.f, 43.f, 43.f, 1.25f},  // X > pilot
};

struct BakedState {
    std::vector<u8> base;  // the picture without changes
    u32 bw = 0, bh = 0;
    std::vector<u8> composed;
    SDL_GamepadButton face = SDL_GAMEPAD_BUTTON_INVALID;
    aurora::texture::ReplacementRegistration reg;
};
std::array<BakedState, std::size(kBaked)> s_baked;

// level 0 of a DXT1 / DXT5 .dds (packs' usual formats), else false
bool loadDds(const std::string& path, std::vector<u8>& out, u32& w, u32& h) {
    FILE* f = nullptr;
    if (!(f = fopen(path.c_str(), "rb"))) return false;
    std::vector<u8> d;
    u8 buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    auto le32 = [&](size_t o) { return u32(d[o]) | u32(d[o + 1]) << 8 | u32(d[o + 2]) << 16 | u32(d[o + 3]) << 24; };
    h = le32(12);
    w = le32(16);
    const bool dxt1 = memcmp(&d[84], "DXT1", 4) == 0, dxt5 = memcmp(&d[84], "DXT5", 4) == 0;
    if ((!dxt1 && !dxt5) || !w || !h || w > 8192 || h > 8192) return false;
    const size_t block = dxt1 ? 8 : 16, need = size_t((w + 3) / 4) * ((h + 3) / 4) * block;
    if (d.size() < 128 + need) return false;
    out.assign(size_t(w) * h * 4, 0);
    const u8* p = &d[128];
    auto c565 = [](u16 v, float* c) {
        c[0] = float((v >> 11) & 31) / 31.f;
        c[1] = float((v >> 5) & 63) / 63.f;
        c[2] = float(v & 31) / 31.f;
    };
    for (u32 by = 0; by < (h + 3) / 4; by++)
        for (u32 bx = 0; bx < (w + 3) / 4; bx++, p += block) {
            u8 alpha[16];
            const u8* cb = p;
            if (dxt5) {
                const u8 a0 = p[0], a1 = p[1];
                u8 a[8] = {a0, a1};
                for (int i = 2; i < 8; i++)
                    a[i] = a0 > a1 ? u8(((8 - i) * a0 + (i - 1) * a1) / 7)
                                   : (i < 6 ? u8(((6 - i) * a0 + (i - 1) * a1) / 5) : (i == 6 ? 0 : 255));
                u64 bits = 0;
                for (int i = 0; i < 6; i++) bits |= u64(p[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7];
                cb = p + 8;
            } else {
                memset(alpha, 255, sizeof alpha);
            }
            const u16 c0 = u16(cb[0] | cb[1] << 8), c1 = u16(cb[2] | cb[3] << 8);
            float c[4][3];
            c565(c0, c[0]);
            c565(c1, c[1]);
            const bool four = dxt5 || c0 > c1;
            for (int k = 0; k < 3; k++) {
                c[2][k] = four ? (2 * c[0][k] + c[1][k]) / 3 : (c[0][k] + c[1][k]) / 2;
                c[3][k] = four ? (c[0][k] + 2 * c[1][k]) / 3 : 0.f;
            }
            const u32 idx = u32(cb[4]) | u32(cb[5]) << 8 | u32(cb[6]) << 16 | u32(cb[7]) << 24;
            for (int i = 0; i < 16; i++) {
                const u32 x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= w || y >= h) continue;
                const u32 sel = (idx >> (2 * i)) & 3;
                u8* o = &out[(size_t(y) * w + x) * 4];
                for (int k = 0; k < 3; k++) o[k] = u8(std::lround(c[sel][k] * 255.f));
                o[3] = !four && sel == 3 ? 0 : alpha[i];
            }
        }
    return true;
}

// bilinear upscale by an integer factor
std::vector<u8> upscale(const std::vector<u8>& src, u32 w, u32 h, u32 f) {
    std::vector<u8> out(size_t(w) * f * h * f * 4);
    for (u32 y = 0; y < h * f; y++)
        for (u32 x = 0; x < w * f; x++) {
            const float sx = std::clamp((float(x) + 0.5f) / float(f) - 0.5f, 0.f, float(w - 1));
            const float sy = std::clamp((float(y) + 0.5f) / float(f) - 0.5f, 0.f, float(h - 1));
            const u32 x0 = u32(sx), y0 = u32(sy), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
            const float fx = sx - float(x0), fy = sy - float(y0);
            for (int k = 0; k < 4; k++) {
                auto at = [&](u32 xx, u32 yy) { return float(src[(size_t(yy) * w + xx) * 4 + k]); };
                const float v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                out[(size_t(y) * w * f + x) * 4 + k] = u8(std::lround(v));
            }
        }
    return out;
}

std::array<std::vector<u8>, 4> s_pixels;  // per Xbox face (kFaces order); Aurora keeps a view
std::vector<aurora::texture::ReplacementRegistration> s_registered;
std::array<SDL_GamepadButton, 4> s_shown{};
bool s_on = false;

const std::vector<u8>& pixels(const Face& f) {
    auto& p = s_pixels[size_t(&f - kFaces)];
    if (p.empty()) p = drawButton(f);
    return p;
}

// (re)builds a baked prompt for the button shown now (nothing when the setting is off)
void composeBaked(size_t i) {
    BakedState& st = s_baked[i];
    const Baked& b = kBaked[i];
    if (st.base.empty()) return;
    const SDL_GamepadButton face = s_on ? s_shown[size_t(b.icon)] : SDL_GAMEPAD_BUTTON_INVALID;
    if (face == st.face) return;
    if (st.reg.id) aurora::texture::unregister_replacement(st.reg);
    st.reg = {};
    st.face = face;
    const Face* f = faceFor(face);
    if (!f) return;
    const float sx = float(st.bw) / float(b.w), sy = float(st.bh) / float(b.h);
    st.composed = st.base;
    // the GameCube icon goes: the bar's background (a pixel left of it) over its rectangle
    const u32 x0 = u32(b.x0 * sx), y0 = u32(b.y0 * sy);
    const u32 x1 = std::min(u32(b.x1 * sx), st.bw), y1 = std::min(u32(b.y1 * sy), st.bh);
    u8 bg[4];
    memcpy(bg, &st.base[(size_t(y0) * st.bw + (x0 > 2 ? x0 - 2 : 0)) * 4], 4);
    for (u32 y = y0; y < y1; y++)
        for (u32 x = x0; x < x1; x++) memcpy(&st.composed[(size_t(y) * st.bw + x) * 4], bg, 4);
    // the Xbox button in its place, as tall as the rectangle
    const u32 d = std::max(8u, u32(std::min((b.x1 - b.x0) * sx * b.stretch, (b.y1 - b.y0) * sy)));
    const std::vector<u8> sq = drawButton(*f, d);
    const u32 dw = std::max(4u, u32(std::lround(float(d) / b.stretch)));  // narrower by the stretch
    std::vector<u8> btn(size_t(dw) * d * 4);
    for (u32 y = 0; y < d; y++)
        for (u32 x = 0; x < dw; x++) {
            const float fx = std::clamp((float(x) + 0.5f) * float(d) / float(dw) - 0.5f, 0.f, float(d - 1));
            const u32 x0 = u32(fx), x1 = std::min(x0 + 1, d - 1);
            const float t = fx - float(x0);
            for (int k = 0; k < 4; k++)
                btn[(size_t(y) * dw + x) * 4 + k] =
                    u8(std::lround(float(sq[(size_t(y) * d + x0) * 4 + k]) * (1 - t) + float(sq[(size_t(y) * d + x1) * 4 + k]) * t));
        }
    const int ox = int((b.x0 + b.x1) * 0.5f * sx - float(dw) * 0.5f), oy = int((b.y0 + b.y1) * 0.5f * sy - float(d) * 0.5f);
    for (u32 y = 0; y < d; y++)
        for (u32 x = 0; x < dw; x++) {
            const int tx = ox + int(x), ty = oy + int(y);
            if (tx < 0 || ty < 0 || u32(tx) >= st.bw || u32(ty) >= st.bh) continue;
            const u8* sp = &btn[(size_t(y) * dw + x) * 4];
            u8* dp = &st.composed[(size_t(ty) * st.bw + u32(tx)) * 4];
            const float a = float(sp[3]) / 255.f;
            for (int k = 0; k < 3; k++) dp[k] = u8(std::lround(float(sp[k]) * a + float(dp[k]) * (1.f - a)));
            dp[3] = u8(std::max(float(dp[3]), float(sp[3])));
        }
    aurora::texture::TextureSourceKey key{.textureHash = b.hash, .width = b.w, .height = b.h, .format = b.fmt};
    aurora::texture::RawTextureReplacement r{.bytes = {st.composed.data(), st.composed.size()},
                                             .width = st.bw,
                                             .height = st.bh,
                                             .mipCount = 1,
                                             .gxFormat = GX_TF_RGBA8_PC,
                                             .label = "Xbox prompt"};
    st.reg = aurora::texture::register_replacement(key, r, {.priority = 1000});
    logf("gfx", "button prompts: %ux%u prompt texture rebuilt (%ux%u) with Xbox %c", b.w, b.h, st.bw, st.bh, f->letter);
}

}  // namespace

// The bridge shows every texture it binds whose size and format match a baked prompt
// (cheap: a size check); the first time one is seen its picture is taken - from a loaded
// pack's .dds of it if there is one (an HD pack's redraw), else the game's own, upscaled 4x.
void buttonIconsNoteTexture(u32 fmt, u32 w, u32 h, const u8* data, u32 bytes) {
    for (size_t i = 0; i < std::size(kBaked); i++) {
        const Baked& b = kBaked[i];
        if (b.w != w || b.h != h || b.fmt != fmt || !s_baked[i].base.empty() || !data) continue;
        if (XXH64(data, bytes, 0) != b.hash) continue;
        BakedState& st = s_baked[i];
        char name[64];
        snprintf(name, sizeof name, "tex1_%ux%u_%016llx_%u.dds", w, h, (unsigned long long)b.hash, fmt);
        const std::string pack = mods::loadedTextureCount() ? mods::findPackFile(name) : std::string();
        if (!pack.empty() && loadDds(pack, st.base, st.bw, st.bh) && st.bw % w == 0 && st.bh % h == 0) {
            logf("gfx", "button prompts: %s from the texture pack", name);
        } else {
            std::vector<u8> own;
            if (!texdump::decodeRgba(fmt, w, h, data, 0, nullptr, 0, own)) continue;
            st.base = upscale(own, w, h, 4);
            st.bw = w * 4;
            st.bh = h * 4;
        }
        st.face = SDL_GAMEPAD_BUTTON_INVALID;
        composeBaked(i);
    }
}

// (re)applies the setting and the current bindings; cheap when nothing changed
void buttonIconsUpdate() {
    static bool previewed = false;  // R2_BUTTON_PREVIEW=<dir>: the drawn buttons as PNGs
    if (!previewed) {
        previewed = true;
        if (const char* dir = getenv("R2_BUTTON_PREVIEW"))
            for (const Face& f : kFaces)
                texdump::writePng(std::string(dir) + "/xbox_" + f.letter + ".png", kSize, kSize, pixels(f).data());
    }
    const bool on = settings::get().xboxButtons;
    std::array<SDL_GamepadButton, 4> want{};
    if (on)
        for (size_t i = 0; i < 4; i++) want[i] = boundFace(kIcons[i]);
    if (on == s_on && want == s_shown) return;
    if (!s_registered.empty()) aurora::texture::unregister_replacements(s_registered);
    s_registered.clear();
    s_on = on;
    s_shown = want;
    for (size_t i = 0; i < std::size(kBaked); i++) composeBaked(i);
    if (!on) {
        logf("gfx", "button prompts: GameCube");
        return;
    }
    for (size_t i = 0; i < 4; i++) {
        const Face* f = faceFor(want[i]);
        if (!f) continue;
        const auto& px = pixels(*f);
        aurora::texture::TextureSourceKey key{.textureHash = kIcons[i].hash, .width = 32, .height = 32, .format = GX_TF_RGBA8};
        aurora::texture::RawTextureReplacement r{.bytes = {px.data(), px.size()},
                                                 .width = kSize,
                                                 .height = kSize,
                                                 .mipCount = 1,
                                                 .gxFormat = GX_TF_RGBA8_PC,
                                                 .label = "Xbox button"};
        s_registered.push_back(aurora::texture::register_replacement(key, r, {.priority = 1000}));
    }
    logf("gfx", "button prompts: Xbox (GameCube A/B/X/Y show %c %c %c %c)", faceFor(want[0])->letter,
         faceFor(want[1])->letter, faceFor(want[2])->letter, faceFor(want[3])->letter);
}


}  // namespace rt::gfx
