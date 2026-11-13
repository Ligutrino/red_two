// Runtime internals shared by the host side (not used by generated code).
#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "ctx.h"

namespace rt {

// --- logging --------------------------------------------------------------------------
void logf(const char* tag, const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);
[[noreturn]] void quit(int code);  // terminate now (no DLL teardown)
const char* symName(u32 addr);  // nearest known name or nullptr
std::string describeAddr(u32 addr);
bool envFlag(const char* name);
std::string traceRing(int n);  // recent guest function entries (R2_TRACE)
void printTraceRing(FILE* f, int n);
void debugInit();  // R2_LOGFN, R2_WATCH
void dumpThreads();  // guest OSThread list (debugging)
u32 peekGuest32(u32 a);  // a guest word without faulting (VM pages not mapped read as 0)
std::string guestBacktrace();  // threads.cpp: the running guest thread's call chain (fatal errors)
void printHostStack(FILE* f, int skip);  // main.cpp: the host call stack, symbolized (fatal errors)

// --- memory ---------------------------------------------------------------------------
constexpr u32 kMem1Size = 0x01800000;
constexpr u32 kVmBase = 0x7E000000, kVmSize = 0x02000000;
constexpr u32 kLockedCache = 0xE0000000, kLockedCacheSize = 0x4000;
bool memInit();
inline bool isMem1(u32 a) { return (a >= 0x80000000u && a < 0x80000000u + kMem1Size) || (a >= 0xC0000000u && a < 0xC0000000u + kMem1Size); }
// Physical address (as DMA engines see it) -> host pointer into MEM1, or nullptr.
u8* physPtr(u32 phys, u32 len);

// --- VM region (vm.cpp) ---------------------------------------------------------------
void vmFlushAll();
void vmSetSdr1(u32 v);

// --- guest functions ------------------------------------------------------------------
GuestFn lookupFn(u32 addr);
GuestFn lookupOrig(u32 addr);  // translated body of a hooked function
void tableInit();
void interpret(Ctx& c, u32 addr);  // interp.cpp: code with no recompiled function
void profilerStart();  // profiler.cpp: R2_PROFILE=<seconds> (call on the guest's host thread)

// --- time and events ------------------------------------------------------------------
constexpr u64 kBusClock = 162000000, kCpuClock = 486000000, kTbClock = kBusClock / 4;
u64 tbNow();                       // guest time base (40.5 MHz)
void schedule(int id, u64 atTb);   // (re)arm event id at time base value
void unschedule(int id);
bool scheduled(int id);
void eventsInit();
void runDueEvents();               // on the guest thread: fire every due event
void wakeGuest();                  // from any host thread: make the guest poll
enum Event : int {
    EV_VI = 0,
    EV_DEC,
    EV_DI,
    EV_AI,
    EV_ARAM,
    EV_DSP,
    EV_SI,
    EV_EXI0,
    EV_EXI1,
    EV_EXI2,
    EV_COUNT
};
void decWrite(u32 v);
u32 decRead();

// --- interrupts -----------------------------------------------------------------------
// PI interrupt sources (INTSR bits)
enum : u32 {
    PI_ERROR = 1 << 0,
    PI_RSW = 1 << 1,
    PI_DI = 1 << 2,
    PI_SI = 1 << 3,
    PI_EXI = 1 << 4,
    PI_AI = 1 << 5,
    PI_DSP = 1 << 6,
    PI_MEM = 1 << 7,
    PI_VI = 1 << 8,
    PI_PE_TOKEN = 1 << 9,
    PI_PE_FINISH = 1 << 10,
    PI_CP = 1 << 11,
    PI_DEBUG = 1 << 12,
    PI_HSP = 1 << 13,
};
void piSetIrq(u32 bits, bool on);
bool piPending();
bool decPending();
void decAck();

// --- threads (threads.cpp) ------------------------------------------------------------
void threadsInit(Ctx*& bootCtx);
Ctx& curCtx();
void deliverException(Ctx& c, u32 exc, u32 pc);

// --- devices --------------------------------------------------------------------------
bool discOpen(const std::string& path);
bool discRead(u64 off, void* dst, u32 len);
std::string discFileAt(u64 off);  // "path+0xoffset" of the disc file containing off (debugging)
void hwInit();
void viFrameHook();  // called once per guest frame (XFB handoff)
bool viIsFramebuffer(u32 addr);  // hw.cpp: has the VI been told to scan out this XFB?
void viSetRate(int fps);   // hw.cpp: retrace rate (60 = 59.94 Hz NTSC, or 90/120/144)
double viRate();           // current retrace rate in Hz
u32 viRetraceCount();      // retraces so far
u32 viXfbLines();          // lines the VI scans out (304 for letterboxed cinematics, 450 in game)
void setAudioSpeed(double speed, bool mute);  // hw.cpp: AI (audio) clock speed-up, output muted
void setDvdBoost(double boost);               // hw.cpp: disc reads that much faster
bool cinematicSkip();                         // hle_game.cpp: a cinematic is being skipped
void setGameAspect(double aspect);  // hle_game.cpp: the game's camera aspect (widescreen)
// hw.cpp: a display copy to dest lands at this line of the VI's (front or back) frame buffer
bool viLocateCopy(u32 dest, u32 strideBytes, u32& line, u32& lines);
// hw.cpp: memory card images (the save importer, dolphin_import.cpp)
std::string cardImagePath(int slot);                       // the image file of slot A
bool cardCurrentImage(int slot, std::vector<u8>& out);     // the inserted card (false: none yet)
void cardDetach(int slot);  // the file was replaced: keep the game's later writes in memory only
void restartGame();         // main.cpp: start a new instance with the same arguments, then quit

namespace gfx {
bool menuOpen();  // gfx/menu.cpp: the settings menu has the controls (the game sees idle pads)
void setHudAnchoring(bool on);  // gfx/menu.cpp: widescreen HUD pieces at the screen edges (in flight)
void noteWidenedAspect(float aspect);  // gfx/menu.cpp: a projection aspect a camera was widened to
void buttonIconsUpdate();  // gfx/button_icons.cpp: Xbox button prompts per the setting and bindings
void buttonIconsNoteTexture(u32 fmt, u32 w, u32 h, const u8* data, u32 bytes);  // a bound texture (baked prompts)
void setMenuTiling(bool on);    // gfx/gx_bridge.cpp: widescreen menu backgrounds tiled into the sides
}

namespace input {
void init(bool pads);  // pads: Aurora PAD (after the window exists); false = R2_INPUT script only
void poll();  // once per frame: Aurora pads/keyboard -> SI controller state
void rumble(int chan, u32 cmd);  // SI motor bits: 0 stop, 1 rumble, 2 hard stop
// The game thread may call SDL's controller API while this is held. Not held: the input thread
// is updating the controllers, maybe a device scan of seconds - use the previous state instead.
class JoystickAccess {
public:
    JoystickAccess();
    ~JoystickAccess();
    JoystickAccess(const JoystickAccess&) = delete;
    JoystickAccess& operator=(const JoystickAccess&) = delete;
    explicit operator bool() const { return m_held; }

private:
    bool m_held = false;
};
}  // namespace input

}  // namespace rt
