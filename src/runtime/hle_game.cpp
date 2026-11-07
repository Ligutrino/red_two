// Game-side hooks (hooks.txt) for the port's options: frame rate, skips, cheats (settings.h).
#include "funcs.h"
#include "runtime.h"
#include "settings.h"
#include "hw/gx_fifo.h"

#include <algorithm>
#include <cstring>

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

// Skippable cutscenes (settings): holding B in a letterboxed cinematic (the VI shows 304 lines
// there, 450 in flight) runs it 8x fast. The game has no skip for the crawl and most cutscenes.
static void fastForward(Ctx& c) {
    if (!settings::get().skipCutscenes || viXfbLines() >= 400 || !(padState(0).buttons & 0x0200)) return;
    c.f[1] = c.ps1[1] = rsingle(c.f[1] * 8.0);
}

// Skip intro logos (settings): the boot screen (mode 5, screen 22 at 0x7FDE8254/0x7FDE822C, see
// the top-level loop 0x7FC00030) shows the legal notice, checks the memory card, then plays the
// 3D LucasArts intro and the logos. Running it fast gets to the title in seconds and leaves its
// logic (card checks, loads) as it is.
static void skipLogos(Ctx& c) {
    if (!settings::get().skipLogos || rd32(0x7FDE8254) != 5 || rd32(0x7FDE822C) != 22) return;
    c.f[1] = c.ps1[1] = rsingle(c.f[1] * 16.0);
}

static void keepHealth();

void hle_GameFrameDelta(Ctx& c) {
    frameDelta(c);
    fastForward(c);
    skipLogos(c);
    keepHealth();
    // R2_DT_LOG: every 10 s, frames and the game time they advanced against the wall clock
    static const bool dtLog = envFlag("R2_DT_LOG");
    if (!dtLog) return;
    static u64 logTb = 0, frames = 0;
    static double simulated = 0;
    const u64 tb = tbNow();
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
void setGameAspect(double aspect) {
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

// ...and its health (craft +0x6F4, a float: 180 for the X-wing) is kept full, so the HUD ring
// stays green. Only for an object whose vtable (+0x80) has CraftDestroy: 0x7FDEFE14 points
// at other things outside missions.
static void keepHealth() {
    static u32 craft = 0;
    static bool isCraft = false;
    static float maxHealth = 0.f;
    const u32 p = settings::get().invincible ? rd32(0x7FDEFE14) : 0;
    if (p != craft) {
        craft = p;
        isCraft = false;
        maxHealth = 0.f;
        const u32 vt = p ? rd32(p + 0x80) : 0;
        if (vt >= 0x7F000000 && vt < 0x81800000)
            for (u32 o = 0; o < 0x400 && !isCraft; o += 4) isCraft = rd32(vt + o) == 0x7FC3E528;
    }
    if (!isCraft) return;
    const float h = float(s2d(rd32(craft + 0x6F4)));
    if (!(h >= 0.f && h <= 100000.f)) return;
    if (h > maxHealth) maxHealth = h;
    else if (h < maxHealth) wr32(craft + 0x6F4, d2s(maxHealth));
}

}  // namespace rt
