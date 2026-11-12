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
static u32 s_craftLast = 0;  // playerCraft's cache (a state load clears it)
static bool s_craftLastIsCraft = false;
static u32 playerCraft() {
    u32& last = s_craftLast;
    bool& lastIsCraft = s_craftLastIsCraft;
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
static void resultsTest(Ctx& c);

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
    resultsTest(c);
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
// Aspects written into the constants before (the window was resized, or the setting changed):
// camera objects copied the default when they were created and keep passing the old value.
static std::atomic<float> s_oldAspects[8];
static std::atomic<u32> s_oldAspectCount{0};
constexpr u32 k43 = 0x3FAAAAAB;  // 4/3
static u32 s_aspectWritten = k43;  // the value setGameAspect last put into the constants

static bool oldGameAspect(double aspect) {
    const u32 n = std::min<u32>(s_oldAspectCount.load(std::memory_order_relaxed), 8);
    for (u32 i = 0; i < n; i++)
        if (std::abs(aspect - double(s_oldAspects[i].load(std::memory_order_relaxed))) < 0.0005) return true;
    return false;
}

void setGameAspect(double aspect) {
    const float previous = s_displayAspect.exchange(float(aspect), std::memory_order_relaxed);
    if (previous != float(aspect) && std::abs(previous - 4.0 / 3.0) > 0.0005 && !oldGameAspect(previous)) {
        const u32 i = s_oldAspectCount.fetch_add(1, std::memory_order_relaxed);
        s_oldAspects[i % 8].store(previous, std::memory_order_relaxed);
    }
    // (The flight camera culls with the camera-object default. HUD cameras built from it, such
    // as the radar globe's, aim off-center; Aurora keeps that offset in the 4:3 HUD layout.
    // 0x8009935C, the HUD's own MTXPerspective calls, stays 4:3.)
    static const u32 kCameraAspect[] = {
        0x80096C64,  // 0x7FC150B0: camera object defaults (+0x274)
        0x80099444,  // 0x7FCD3C38: camera object (+0x58)
        0x800994F4,  // 0x7FCD771C
        0x80099548,  // 0x7FCDA190: the world camera
    };
    u32& written = s_aspectWritten;
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
// camera's culling volume - a pyramid from the camera to the far plane's corners (+0x138..+0x168,
// side planes after them; horizontal fov +0x130) - from the aspect and fov its camera object
// holds (+0x274/+0x270), and keeps them (+0x12C, +0x134) for the projection 0x7FC153C0 builds
// next. Not every camera takes the patched constants above: the LucasArts intro's has its own
// 1.3333, letterboxed cinematics and the hangar use 2.105 (4/3 * 480/304) and 4/3. Aurora shows
// such a projection opened up to fill the screen (patch 0009, widescreen_scale): with k = display
// aspect / (4/3) and v = 480 / the XFB's lines in a letterboxed frame (1 otherwise), r = k / v
// opens it r times wider if r > 1, 1/r times taller if r < 1.
//
// So the culling volume is computed for that opened view, and outside free flight (intro,
// menus, the hangar, cinematics) kSceneMargin times wider and taller still for objects (the
// frustum planes): objects whose bounds sit just outside the edge while part of them shows (a
// trooper's feet below the screen) no longer drop out. The margin stays small and the
// horizontal fov (+0x130, which e.g. the terrain grid walk 0x7FC269C4 sizes its area by) stays
// the screen's: a 3x margin overflowed the game's per-frame buffers (heap chunk lists freed
// with bad pointers in the logos and the menus). In missions the flight camera (already at the
// display aspect: setGameAspect) and the rest take kTerrainMargin, the horizontal fov included
// (the terrain, below).
// The projection: a full frame opened sideways gets the game's aspect widened r times with the
// game's fov (4:3 -> the display's aspect; a cinematic camera's 2.105 drawn inside a full
// 450-line frame, like the take-off, -> 2.105 r), which Aurora shows as it is (the same
// picture; anything else the game tests against the projection sees the whole screen);
// otherwise (taller, letterboxed) the game's own aspect and fov, which Aurora opens. The
// camera's own copy of the projection (+0xA4) follows: the game places the engine glows with
// it. R2_GAME_CULLING=1 keeps the game's own volumes, R2_CULL_MARGIN=<f> sets the margin (1 =
// exactly the screen), R2_CAM_LOG=1 logs the camera setups.
constexpr double kSceneMargin = 1.2;
constexpr double kTerrainMargin = 1.2;

// CameraHFovAtan (0x7FC8DE3C; only caller CameraSetup, 0x7FC14D30): atan of f1 clamped to
// [-1, 1]. CameraSetup takes the horizontal fov (+0x130) as 2 * this(aspect * tan(fovY / 2)), so
// it never passes 90 degrees, and the far-plane corners and side planes derived from it neither:
// plenty at 4:3 (the cockpit's 55-degree view is 69.5 wide), but the cockpit at 2.13:1 shows 96
// degrees (106 with the margin) and the Death Star's terrain (TerrainDraw 0x7FCBCCC4 lays out
// the tiles inside the hull of the camera and the far corners seen from above) left the sides
// out in a sawtooth, at 32:9 all but a wedge in the middle. While hle_CameraSetup sets up a widened volume in a mission the full atan
// (0x8005348C, the SDK's) is taken, up to 70 degrees (140 wide): the terrain span array (4x its
// rows, hle_HeapAlloc) holds a footprint twice the far corners' distance across, which stays
// under ~250 of its 284 rows there.
static bool s_wideVolume = false;
constexpr double kMaxHalfTan = 2.7474774;  // tan(70 degrees)

void hle_CameraHFovAtan(Ctx& c) {
    if (s_wideVolume && std::abs(c.f[1]) > 1.0) {
        c.f[1] = std::clamp(c.f[1], -kMaxHalfTan, kMaxHalfTan);
        f_8005348C(c);
        return;
    }
    f_7FC8DE3C_orig(c);
}

// MTXPerspective (SDK 0x8002FEA4; fovY f1, aspect f2, near f3, far f4). The HUD's cameras (the
// radar globe: 0x7FCD22C4, 0x7FCD4F60, 0x7FCD50F4) build their projections here, not through
// CameraSetup, from the aspect their objects copied when the mission started: after a window
// resize they kept the old window's (2.37 at 1280x1024: the globe drew squeezed). An aspect
// setGameAspect wrote before is a widened 4:3 one, like in hle_CameraSetup: today's instead.
void hle_MTXPerspective(Ctx& c) {
    const double aspect = c.f[2];
    const double display = s_displayAspect.load(std::memory_order_relaxed);
    if (std::abs(aspect - display) >= 0.0005 && oldGameAspect(aspect)) c.f[2] = c.ps1[2] = rsingle(display);
    f_8002FEA4_orig(c);
}

void hle_CameraSetup(Ctx& c) {
    double aspect = c.f[1];
    const double fov = c.f[2];
    const double display = s_displayAspect.load(std::memory_order_relaxed);
    // A camera object made before the window was resized (or the aspect setting changed) still
    // holds the aspect it copied then: it is a widened 4:3 camera like the rest, so it takes
    // today's. (It kept the old one: the flight camera's culling was opened from it and the
    // radar globe drew squeezed.)
    if (std::abs(aspect - display) >= 0.0005 && oldGameAspect(aspect)) {
        aspect = display;
        c.f[1] = c.ps1[1] = rsingle(display);
    }
    // R2_CAM_LOG=1: each camera setup (view, caller, aspect, fov) when it changes
    static const bool camLog = envFlag("R2_CAM_LOG");
    if (camLog) {
        struct Seen {
            u32 view, lr;
            float aspect, fov;
        };
        static std::vector<Seen> seen;
        const Seen s{c.r[3], c.lr, float(aspect), float(fov)};
        Seen* e = nullptr;
        for (Seen& x : seen)
            if (x.view == s.view && x.lr == s.lr) e = &x;
        if (!e || e->aspect != s.aspect || e->fov != s.fov) {
            if (e) *e = s;
            else if (seen.size() < 256) seen.push_back(s);
            logf("cam", "setup view %08X from %08X: aspect %.4f fov %.2f (display %.4f, xfb %u lines)", s.view, s.lr,
                 aspect, fov, display, viXfbLines());
        }
    }
    static const bool gameCulling = envFlag("R2_GAME_CULLING");
    if (gameCulling || display < 4.0 / 3.0 + 0.01 || !(aspect > 0.5 && aspect < 8.0) || !(fov > 1.0 && fov < 170.0)) {
        f_7FC14CEC_orig(c);
        return;
    }
    const u32 view = c.r[3];
    const u32 matrix = c.r[4];
    const u32 craft = playerCraft();
    // already at the display's aspect (setGameAspect: the flight and HUD cameras): not opened
    const bool widened = std::abs(aspect - display) < 0.002;
    const u32 lines = viXfbLines();
    const double k = widened ? 1.0 : display * 0.75;
    const double v = !widened && lines && lines < 408 ? 480.0 / lines : 1.0, r = k / v;
    static const double sceneMargin = [] {
        const char* e = getenv("R2_CULL_MARGIN");
        return e ? std::clamp(atof(e), 1.0, 20.0) : kSceneMargin;
    }();
    static const double terrainMargin = [] {
        const char* e = getenv("R2_TERRAIN_MARGIN");
        return e ? std::clamp(atof(e), 1.0, 3.0) : kTerrainMargin;
    }();
    // In a letterboxed frame every camera culls for at least the letterboxed picture: the hangar
    // (ship select) renders at 2.105 but sets the camera up once more per frame at 4:3, and that
    // volume decides what is there (the bay's sides popped in as the camera panned).
    const double base = v > 1.0 ? std::max(aspect, 4.0 / 3.0 * v) : aspect;
    const double margin = craft ? terrainMargin : sceneMargin;
    // in missions the horizontal fov may pass 90 degrees (hle_CameraHFovAtan)
    struct WideVolume {
        explicit WideVolume(bool on) { s_wideVolume = on; }
        ~WideVolume() { s_wideVolume = false; }
    } wideVolume(craft != 0);
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    auto setup = [&](double m) {  // the volume opened for the screen, m times more
        const double wider = std::max(1.0, r) * m, taller = std::max(1.0, 1.0 / r) * m;
        c.r[3] = view;  // (the call keeps the non-volatile registers; arguments again)
        c.r[4] = matrix;
        c.f[1] = c.ps1[1] = rsingle(base * wider / taller);
        c.f[2] = c.ps1[2] = rsingle(std::min(2.0 * std::atan(std::tan(fov * 0.5 * kRad) * taller) / kRad, 170.0));
        f_7FC14CEC_orig(c);
    };
    if (widened && margin == 1.0) {
        f_7FC14CEC_orig(c);
        return;
    }
    // Outside missions the horizontal fov stays the screen's (see above); in missions it takes
    // the margin too: the terrain (the Death Star's surface) is laid out from the volume, whole
    // tiles inside it, and the tiles at the sides dropped out in a sawtooth just inside the edges
    // (and past 90 degrees everywhere outside that: hle_CameraHFovAtan).
    u32 hfov = 0;
    if (margin != 1.0 && !craft) {
        setup(1.0);
        hfov = rd32(view + 0x130);
    }
    setup(margin);
    if (hfov) wr32(view + 0x130, hfov);
    // The projection. A full frame opened sideways: the game's view widened r times (4:3 ->
    // the display's aspect; a cinematic camera's 2.105 inside a full frame -> 2.105 r), which
    // Aurora is told to show as it is. Otherwise (letterboxed, or a display taller than the
    // camera) the game's own, which Aurora opens.
    const bool sideways = v == 1.0 && r >= 1.0;
    const double projAspect = sideways ? aspect * r : aspect;
    if (sideways) gfx::noteWidenedAspect(float(projAspect));
    wr32(view + 0x12C, d2s(projAspect));
    wr32(view + 0x134, d2s(fov));
    // The camera keeps its own copy of the projection (+0xA4, and +0xE4 derived from it; built
    // by 0x7FC14B8C from the aspect, fov and near/far): the game projects points with it, e.g.
    // to place the engine glows. Built for the culling volume above, it put them closer to the
    // center than the ships' engines; rebuilt here for the projection that is shown.
    c.r[3] = view;
    c.r[4] = view + 0xA4;
    c.r[5] = view + 0xE4;
    f_7FC14B8C(c);
}

// The terrain's span array. TerrainSetup (0x7FD89210) allocates it at a mission's start with
// one 4-byte (left, right) pair per grid row, as many rows as the view distance spans for the
// game's own 4:3 view; every frame TerrainSpans (0x7FCBAA0C) rasterizes the view footprint onto
// the terrain grid and writes one pair per row it covers, with no bound. Widened views (the
// display's aspect, the terrain margin above) cover more rows: the pairs ran past the array
// into the next heap block's header (the tile records, 0x80095FA4), and the game crashed in
// HeapFree when the mission ended (or later). That one allocation (this return address) gets
// four times the room, enough for 32:9 with the margin.
void hle_HeapAlloc(Ctx& c) {
    if (c.lr == 0x7FD893E8u) [[unlikely]] {
        logf("wide", "terrain span array: room for %u rows instead of %u", c.r[3], c.r[3] / 4);
        c.r[3] *= 4;
    }
    f_800070C4_orig(c);
}

// Perfect mission stats (settings). The mission's running stats live at 0x7FDF04F0 (StatsReset
// 0x7FC464B0 at a mission's start): +0x00/+0x04 shots fired (lasers / other), +0x0C hits, +0x10
// enemies that appeared, +0x14 enemies the player destroyed, +0x18 friendlies, +0x1C friendlies
// lost (StatsObjectDestroyed 0x7FC46840), +0x20 lives lost, +0x24 mission time (seconds; weapons
// and respawns read it, so it is left alone during play), +0x28/+0x2C targeting computer uses
// and seconds. At the end MissionResults (0x7FD76514) turns them into the record the results
// screen shows and MedalRank (0x7FD765F4) compares with the mission's requirements: +0x00 time
// (minutes, float), +0x04 enemies destroyed, +0x06 accuracy %, +0x08 friendlies lost (u16 each),
// +0x0C lives lost (u32), +0x10 targeting computer efficiency % (float). With the cheat the
// record says 0:01, every enemy that appeared destroyed, 100%, 0, 0 and 100%. (Mission 15's
// +0x08 counts something where more is better: left as it is.)
void hle_MissionResults(Ctx& c) {
    const u32 rec = c.r[4];
    f_7FD76514_orig(c);
    if (!settings::get().perfectStats) return;
    constexpr u32 kStats = 0x7FDF04F0;
    const u32 enemies = std::max(rd32(kStats + 0x10), rd32(kStats + 0x14));
    wr32(rec + 0x00, fbits(1.25f / 60.f));  // 1.25 s: shown as 0:01, compared in whole seconds
    wr16(rec + 0x04, std::min<u32>(enemies, 0xFFFF));
    wr16(rec + 0x06, 100);
    if (rd8(0x800962EC) != 15) wr16(rec + 0x08, 0);  // the mission (r13-0x4AB4)
    wr32(rec + 0x0C, 0);
    wr32(rec + 0x10, fbits(100.f));
    logf("cheat", "perfect mission stats: 0:01, %u enemies destroyed, 100%% accuracy, nothing lost, 100%% targeting computer",
         enemies);
}

// R2_RESULTS_TEST=<seconds>: once, that long after the start, MissionResults into a scratch
// record below the guest stack, logged before and after the cheat (missions are long to finish
// in a test). Called from GameFrameDelta, an ordinary guest call.
static void resultsTest(Ctx& c) {
    static const double at = getenv("R2_RESULTS_TEST") ? atof(getenv("R2_RESULTS_TEST")) : -1.0;
    static bool done = false;
    if (at < 0 || done) return;
    static const u64 t0 = tbNow();
    if (double(tbNow() - t0) / double(kTbClock) < at) return;
    done = true;
    for (int pass = 0; pass < 2; pass++) {
        settings::Settings& s = settings::get();
        const bool keep = s.perfectStats;
        s.perfectStats = pass == 1;
        Ctx t = c;
        t.r[1] = c.r[1] - 0x200;
        const u32 rec = t.r[1] + 0x100;
        t.r[4] = rec;
        t.lr = 0;
        hle_MissionResults(t);
        s.perfectStats = keep;
        logf("cheat", "results test (%s): time %.3f min, enemies %u, accuracy %u%%, friendlies lost %u, lives lost %u, targeting computer %.1f%%",
             pass ? "cheat" : "game's", s2d(rd32(rec)), rd16(rec + 4), rd16(rec + 6), rd16(rec + 8), rd32(rec + 12),
             s2d(rd32(rec + 16)));
    }
}

// The targeting computer (cockpit view, Y). TargetingRect (0x7FCD5D08) projects the corners of
// its screen with the camera's projection (+0xA4) to 640x480 screen units, and TargetingPass
// (0x7FCD7D28, each frame pass) draws the orange screen there as a 2D quad and copies the EFB
// under it. In widescreen those
// are positions on the widened screen; Aurora laid the quad out like any 4:3 2D element and
// squeezed it a second time. The drawing is bracketed for the bridge (gfx/gx_bridge.cpp,
// screenSpace2D) by BP writes to an unused register, through the gather pipe so they stay in
// order with the game's commands.
static void gatherBp(u32 v) {
    wr8(0xCC008000, 0x61);
    wr32(0xCC008000, v);
}

void hle_TargetingPass(Ctx& c) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("wide", "targeting computer screen drawn in screen space");
    }
    gatherBp(0x7E5C5201);
    f_7FCD7D28_orig(c);
    gatherBp(0x7E5C5200);
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
static u32 s_healthCraft = 0;  // keepHealth's craft and its full health
static float s_healthFull = 0.f;
static bool s_healthFlying = false;
static void keepHealth() {
    u32& craft = s_healthCraft;
    float& full = s_healthFull;
    bool& flyingBefore = s_healthFlying;
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

// --- save states ----------------------------------------------------------------------------
#include "savestate.h"
namespace rt {
void hleSaveState(state::Writer& w) {
    w.section("HLE ");
    w.vec(s_weapons);
}

void hleLoadState(state::Reader& r) {
    r.section("HLE ");
    r.vec(s_weapons);
}

// After a load (on the guest's fiber): caches of guest objects start over, a skip in progress
// ends, and the camera aspect constants hold what the saved session wrote (a stale aspect for
// the camera objects of the state if the window has another shape now).
void hleAfterLoad() {
    s_craftLast = 0;
    s_craftLastIsCraft = false;
    s_healthCraft = 0;
    s_healthFull = 0.f;
    s_healthFlying = false;
    if (s_skip) {
        s_skip = false;
        viSetRate(settings::get().fps);
        setDvdBoost(1.0);
        setAudioSpeed(1.0, false);
    }
    const u32 bits = rd32(0x80099548);  // the world camera's (see setGameAspect)
    s_aspectWritten = bits;
    float f;
    memcpy(&f, &bits, 4);
    const float display = s_displayAspect.load(std::memory_order_relaxed);
    if (bits != k43 && f > 0.5f && f < 8.f && std::abs(f - display) >= 0.0005f && !oldGameAspect(f)) {
        const u32 i = s_oldAspectCount.fetch_add(1, std::memory_order_relaxed);
        s_oldAspects[i % 8].store(f, std::memory_order_relaxed);
    }
}
}  // namespace rt
