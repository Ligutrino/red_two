// Game-side hooks (hooks.txt) for the port's options: frame rate, skips, cheats (settings.h).
#include "funcs.h"
#include "runtime.h"
#include "settings.h"
#include "hw/gx_fifo.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rt {

// GameFrameDelta (0x7FD46244): the seconds the frame advances the game by, stored at 0x7FDE8288
// by the per-frame update (0x7FC02CF0) of every main loop. The game computes
// 1 / (59.94 - lost) with `lost` the frames per second it measured missing (0x7FE0C6A4,
// from its retrace callback 0x7FD46A10, capped at 29.97): 1/59.94 s when it keeps up. At a
// higher retrace rate (settings fps) it would keep stepping 1/59.94 s per frame and run fast,
// so there the delta is the real time since the previous frame.
static void frameDelta(Ctx& c) {
    f_7FD46244_orig(c);
    const double hz = viRate();
    if (hz < 60.5) return;  // 59.94 Hz: the game's own measure
    // Real time rather than retraces: those can fire late when the host is busy, which would
    // run the game a few percent slow. Capped like the game's own measure (1/29.97 s), e.g.
    // after a load; no real lower bound, or short frames (two updates in one, as loading
    // screens' 0x7FC03BC4 can do) would add time and run the game fast.
    static u64 lastTb = 0;
    const u64 tb = tbNow();
    const double dt = lastTb ? double(tb - lastTb) / double(kTbClock) : 1.0 / hz;
    lastTb = tb;
    c.f[1] = c.ps1[1] = rsingle(std::clamp(dt, 0.00025, 1.0 / 29.97));
}

// The player's craft (0x7FDEFE14 in missions), or 0 when that pointer holds something else:
// a craft's vtable (+0x80) has CraftDestroy (0x7FC3E528).
static u32 playerCraft() {
    static u32 last = 0;
    static bool lastIsCraft = false;
    const u32 p = rd32(0x7FDEFE14);
    if (p != last) {
        last = p;
        lastIsCraft = false;
        const u32 vt = p ? rd32(p + 0x80) : 0;
        if (vt >= 0x7F000000 && vt < 0x81800000)
            for (u32 o = 0; o < 0x400 && !lastIsCraft; o += 4) lastIsCraft = rd32(vt + o) == 0x7FC3E528;
    }
    return lastIsCraft ? p : 0;
}

// Skippable cutscenes (settings): the game has no skip for the crawl and most cutscenes, so a
// press of B in a letterboxed cinematic (the VI shows 304 lines there, 450 in flight) runs the
// game flat out until play resumes: retraces at kSkipHz, kSkipStep game seconds per frame
// (8x a 60 fps frame: what cinematics were tested at), nothing drawn (gx_bridge.cpp), the
// audio clock (AI DMA -> MusyX: sequencer, voices, streams) as fast and silent, disc reads
// faster so streams keep up. It ends once the player's craft flies (state +0x370 = 0) in a
// frame that is not letterboxed: between the crawl and the mission intro the game shows full
// frames for a moment. Outside missions, after 3 s of full frames.
constexpr double kSkipStep = 8.0 / 60.0;
constexpr int kSkipHz = 600;
static bool s_skip = false;
bool cinematicSkip() { return s_skip; }

// Skip intro logos (settings): the boot screen (mode 5, screen 22 at 0x7FDE8254/0x7FDE822C, see
// the top-level loop 0x7FC00030) shows the legal notice, checks the memory card, then plays the
// 3D LucasArts intro and the logos: 16x and silent, still drawn (the card dialogs may need an
// answer), its logic (card checks, loads) as it is.
static bool bootScreen() { return rd32(0x7FDE8254) == 5 && rd32(0x7FDE822C) == 22; }

static void keepHealth();
static void keepAmmo();
static void craftDiffProbe();

void hle_GameFrameDelta(Ctx& c) {
    frameDelta(c);
    const settings::Settings& s = settings::get();
    static u64 lastTb = 0;  // real time since the previous frame
    const u64 tb = tbNow();
    const double real = lastTb ? std::max(double(tb - lastTb) / double(kTbClock), 0.0001) : 1.0 / 60.0;
    lastTb = tb;

    const bool cinematic = viXfbLines() < 400;
    const bool b = (padState(0).buttons & 0x0200) != 0;  // PAD_BUTTON_B
    static bool bWas = false;
    const bool bPressed = b && !bWas;
    bWas = b;
    if (!s_skip && s.skipCutscenes && cinematic && bPressed) {
        s_skip = true;
        logf("skip", "skipping to the next gameplay");
        viSetRate(kSkipHz);
        setDvdBoost(50.0);
    }
    static double fullFor = 0;  // real seconds of frames that are not letterboxed while skipping
    fullFor = s_skip && !cinematic ? fullFor + real : 0;
    const u32 craft = playerCraft();
    static int flyingFrames = 0;  // (the craft passes through state 0 while it is set up)
    flyingFrames = !cinematic && craft && rd32(craft + 0x370) == 0 ? flyingFrames + 1 : 0;
    if (s_skip && (!s.skipCutscenes || flyingFrames >= 10 || fullFor > 3.0)) {
        s_skip = false;
        logf("skip", "back to play");
        viSetRate(s.fps);
        setDvdBoost(1.0);
    }
    if (s_skip) {
        c.f[1] = c.ps1[1] = rsingle(kSkipStep);
        setAudioSpeed(std::min(kSkipStep / real, 64.0), true);
    } else if (s.skipLogos && bootScreen()) {
        c.f[1] = c.ps1[1] = rsingle(c.f[1] * 16.0);
        setAudioSpeed(16.0, true);
    } else {
        setAudioSpeed(1.0, false);
    }
    keepHealth();
    keepAmmo();
    craftDiffProbe();
    // widescreen: the flight HUD goes to the screen edges (not the menus: their layout is 4:3)
    gfx::setHudAnchoring(s.hudEdges && s.aspect != settings::Aspect::R4_3 && playerCraft() && !cinematic);
    // menu screens only (no player craft; not the boot screen's intro and logos, whose 2D draws
    // also look like a run of panels, nor letterboxed cinematics): the background's panels
    // repeat into the sides
    gfx::setMenuTiling(s.tileMenus && !playerCraft() && !bootScreen() && !cinematic);
    // R2_DT_LOG: every 10 s, frames and the game time they advanced against the wall clock
    static const bool dtLog = envFlag("R2_DT_LOG");
    if (!dtLog) return;
    static u64 logTb = 0, frames = 0;
    static double simulated = 0;
    if (!logTb) logTb = tb;
    frames++;
    simulated += c.f[1];
    if (tb - logTb >= 10 * kTbClock) {
        logf("dt", "%llu frames, %.2f s game time in %.2f s", (unsigned long long)frames, simulated,
             double(tb - logTb) / kTbClock);
        logTb = tb;
        frames = 0;
        simulated = 0;
    }
}

// Widescreen at the source. Aurora widens perspective projections on a wider target, but the
// game culls with its own 4:3 view volume: the world camera (0x7FCDA190) passes 4/3 from its
// small-data constant to the camera setup 0x7FC14CEC, which keeps the aspect (camera +0x12C),
// derives the horizontal field of view (+0x130) and builds the projection from them
// (0x7FD43668 -> MTXPerspective). Each camera function has its own copy of the constant; the
// ones below take the display aspect, so the game renders and culls for it. Aurora leaves
// projections that are already wide alone (Aurora patch 0009).
static std::atomic<float> s_displayAspect{4.f / 3.f};

void setGameAspect(double aspect) {
    s_displayAspect.store(float(aspect), std::memory_order_relaxed);
    // (The flight camera culls with the camera-object default. HUD cameras built from it, such
    // as the radar globe's, aim off-center; Aurora keeps that offset in the 4:3 HUD layout.
    // 0x8009935C, the HUD's own MTXPerspective calls, stays 4:3.)
    static const u32 kCameraAspect[] = {
        0x80096C64,  // 0x7FC150B0: camera object defaults (+0x274)
        0x80099444,  // 0x7FCD3C38: camera object (+0x58)
        0x800994F4,  // 0x7FCD771C
        0x80099548,  // 0x7FCDA190: the world camera
    };
    constexpr u32 k43 = 0x3FAAAAAB;  // 4/3
    static u32 written = k43;
    const float f = float(aspect);
    u32 bits;
    memcpy(&bits, &f, 4);
    if (bits == written) return;
    for (u32 a : kCameraAspect) {
        const u32 cur = rd32(a);
        if (cur == k43 || cur == written) wr32(a, bits);
        else logf("wide", "camera aspect at %08X is %08X, left alone", a, cur);
    }
    written = bits;
    logf("wide", "game camera aspect %.4f", aspect);
}

// CameraSetup (0x7FC14CEC; view r3, matrix r4, aspect f1, vertical fov f2 in degrees) derives a
// camera's culling volume (horizontal fov +0x130, frustum vectors from +0x138) from the aspect
// and fov its camera object holds (+0x274/+0x270), and keeps them (+0x12C, +0x134) for the
// projection 0x7FC153C0 builds next. Not every camera takes the patched constants above: the
// LucasArts intro's has its own 1.3333, letterboxed cinematics use 2.105 (4/3 * 480/304). Aurora
// shows such a projection opened up to fill the screen (patch 0009, widescreen_scale): with
// k = display aspect / (4/3) and v = 480 / the XFB's lines in a letterboxed frame (1 otherwise),
// r = k / v opens it r times wider if r > 1, 1/r times taller if r < 1. The culling volume is
// computed for that opened view, while the projection keeps the game's own aspect and fov:
// stormtroopers at the intro's sides and objects at a cutscene's top and bottom are no longer
// dropped. (Cameras already at the display aspect - flight, which Aurora leaves as they are -
// pass unchanged.) R2_GAME_CULLING=1 keeps the game's own volumes (for comparisons).
void hle_CameraSetup(Ctx& c) {
    const double aspect = c.f[1], fov = c.f[2];
    const double display = s_displayAspect.load(std::memory_order_relaxed);
    static const bool gameCulling = envFlag("R2_GAME_CULLING");  // debugging: the game's own volumes
    if (gameCulling || display < 4.0 / 3.0 + 0.01 || !(aspect > 0.5 && aspect < 8.0) || !(fov > 1.0 && fov < 170.0) ||
        std::abs(aspect - display) < 0.002) {
        f_7FC14CEC_orig(c);
        return;
    }
    const u32 lines = viXfbLines();
    const double k = display * 0.75, v = lines && lines < 408 ? 480.0 / lines : 1.0, r = k / v;
    const double wider = std::max(1.0, r), taller = std::max(1.0, 1.0 / r);
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    const u32 view = c.r[3];
    c.f[1] = c.ps1[1] = rsingle(aspect * wider / taller);
    c.f[2] = c.ps1[2] = rsingle(2.0 * std::atan(std::tan(fov * 0.5 * kRad) * taller) / kRad);
    f_7FC14CEC_orig(c);
    wr32(view + 0x12C, d2s(aspect));  // the projection: the game's own
    wr32(view + 0x134, d2s(fov));
}

// Infinite lives (settings): LoseLife (0x7FD7648C) decrements the lives byte at 0x800962EE
// (3 ships at a mission start; at 1 it ends the game instead: 0x800962EF = 0x21, mode 6) and
// returns 1. With the cheat it reports the life lost without taking it.
void hle_LoseLife(Ctx& c) {
    if (settings::get().infiniteLives && rd8(0x800962EF) != 0x21) {
        c.r[3] = 1;
        return;
    }
    f_7FD7648C_orig(c);
}

// Invincibility (settings): CraftDestroy (0x7FC3E528, a method every craft class shares) starts
// a craft's explosion - whatever destroyed it, shots or a crash. The player's craft
// (0x7FDEFE14) is spared.
void hle_CraftDestroy(Ctx& c) {
    if (settings::get().invincible && c.r[3] == rd32(0x7FDEFE14)) return;
    f_7FC3E528_orig(c);
}

// Infinite bombs: WeaponInit (0x7FC396D4) sets up each weapon of a craft - count (+0) and
// maximum (+4) from the craft's data, the weapon slot at +0x520. Weapon objects sit at a
// different place in each craft class (inside it or behind a pointer), so they are recorded
// here as the game initializes them. The player's secondary weapons are given back what they
// fire right away (WeaponUpdate, below) and topped up every frame (missions can also restore a
// count from a checkpoint); unlimited weapons have a maximum of 0 and are left alone.
//
// Death Star Attack fails ("You have no proton torpedoes left to fire at the exhaust port")
// from its mission script once the torpedo count runs out; the count has to stay up in every
// state the craft flies in, the trench run included, not only in free flight (+0x370 = 0).
struct WeaponRec {
    u32 craft, weapon, slot, max;
};
static std::vector<WeaponRec> s_weapons;

void hle_WeaponInit(Ctx& c) {
    const u32 weapon = c.r[3], craft = c.r[4], slot = c.r[5];
    f_7FC396D4_orig(c);
    const u32 max = rd32(weapon + 4);
    for (WeaponRec& w : s_weapons)
        if (w.weapon == weapon) {  // re-initialized (respawn), or memory reused by another craft
            w = {craft, weapon, slot, max};
            return;
        }
    if (s_weapons.size() >= 1024) s_weapons.erase(s_weapons.begin(), s_weapons.begin() + 512);
    s_weapons.push_back({craft, weapon, slot, max});
}

// fill one of the player craft's recorded weapons (a mission may have set a new maximum)
static void refill(WeaponRec& w) {
    if (w.max == 0 || rd32(w.weapon + 0x520) != w.slot) return;  // unlimited, or no longer this weapon
    const u32 max = rd32(w.weapon + 4);
    if (max == 0 || max > 999) return;
    w.max = max;
    const s32 count = s32(rd32(w.weapon));
    if (count >= 0 && count < s32(max)) wr32(w.weapon, max);  // (-1: unlimited already)
}

static void keepAmmo() {
    if (!settings::get().infiniteAmmo) return;
    const u32 craft = playerCraft();
    if (!craft) return;
    for (WeaponRec& w : s_weapons)
        if (w.craft == craft) refill(w);
}

// WeaponUpdate (0x7FC39F24, weapon r3, craft r4): a weapon's update, which fires it on the
// trigger and takes one from its count. The player's shot is given back before anything (HUD,
// mission script) reads the count.
void hle_WeaponUpdate(Ctx& c) {
    const u32 weapon = c.r[3], craft = c.r[4];
    f_7FC39F24_orig(c);
    if (!settings::get().infiniteAmmo || !craft || craft != playerCraft()) return;
    for (WeaponRec& w : s_weapons)
        if (w.weapon == weapon && w.craft == craft) {
            refill(w);
            break;
        }
}

// ...and its health (craft +0x6F4, a float: 180 for the X-wing) is kept full, so the HUD ring
// stays green. Only for an object whose vtable (+0x80) has CraftDestroy: 0x7FDEFE14 points
// at other things outside missions.
// The full value is what the craft has when it starts flying (state +0x370 = 0: the game fills it
// at a spawn and a respawn); while the craft is being set up the field holds other things, and
// keeping one of those (more than full) made the HUD's health ring vanish.
static void keepHealth() {
    static u32 craft = 0;
    static float full = 0.f;
    static bool flyingBefore = false;
    const u32 p = settings::get().invincible ? playerCraft() : 0;
    if (p != craft) {
        craft = p;
        full = 0.f;
        flyingBefore = false;
    }
    if (!craft) return;
    const bool flying = rd32(craft + 0x370) == 0;
    const float h = float(s2d(rd32(craft + 0x6F4)));
    if (!flying) {
        flyingBefore = false;
        return;
    }
    if (!flyingBefore || full <= 0.f) {
        flyingBefore = true;
        full = h > 0.f && h <= 100000.f ? h : 0.f;
        return;
    }
    if (h < full) wr32(craft + 0x6F4, d2s(full));
}

// Debugging: R2_CRAFT_DIFF=<len hex>[,<deref offset hex>...]: every frame, compare the player
// craft object's first <len> bytes (or those of objects it points to at the given offsets)
// with the previous frame and log bytes/halfwords/words that drop by exactly 1 from a small
// value (ammo counters); fields that keep changing are muted after 8 reports.
static void craftDiffProbe() {
    static const char* spec = getenv("R2_CRAFT_DIFF");
    if (!spec) return;
    static u32 len = 0;
    static std::vector<s32> derefs;
    if (!len) {
        char* end;
        len = u32(strtoul(spec, &end, 16)) & ~3u;
        while (*end == ',') derefs.push_back(s32(strtol(end + 1, &end, 16)));
        if (derefs.empty()) derefs.push_back(-1);
    }
    const u32 craft = playerCraft();
    static std::vector<std::vector<u8>> prev;
    static std::vector<u32> prevBase;
    static std::unordered_map<u64, int> reports;
    prev.resize(derefs.size());
    prevBase.resize(derefs.size());
    for (size_t d = 0; d < derefs.size(); d++) {
        u32 base = craft;
        if (craft && derefs[d] >= 0) {
            base = rd32(craft + u32(derefs[d]));
            if ((base >> 24) != 0x80 && (base >> 24) != 0x81 && (base >> 24) != 0x7F) base = 0;
        }
        std::vector<u8> cur;
        if (base) {
            cur.resize(len);
            for (u32 o = 0; o < len; o += 4) {
                const u32 w = rd32(base + o);
                cur[o] = u8(w >> 24), cur[o + 1] = u8(w >> 16), cur[o + 2] = u8(w >> 8), cur[o + 3] = u8(w);
            }
        }
        if (base && base == prevBase[d] && prev[d].size() == len) {
            for (u32 o = 0; o < len; o++) {
                for (u32 size : {1u, 2u, 4u}) {
                    if (o % size) continue;
                    u32 a = 0, b = 0;
                    for (u32 i = 0; i < size; i++) a = a << 8 | prev[d][o + i], b = b << 8 | cur[o + i];
                    if (a >= 1 && a <= 255 && b == a - 1) {
                        const u64 key = (u64(d) << 40) | (u64(o) << 8) | size;
                        if (reports[key]++ < 8)
                            logf("diff", "craft %08X (%s+%X) +%03X size %u: %u -> %u", base, derefs[d] < 0 ? "obj" : "ptr",
                                 derefs[d] < 0 ? 0 : derefs[d], o, size, a, b);
                    }
                }
            }
        }
        prev[d] = std::move(cur);
        prevBase[d] = base;
    }
}

}  // namespace rt
