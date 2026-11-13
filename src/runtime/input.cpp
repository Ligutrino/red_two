// Controllers: Aurora reads gamepads (SDL3, GameCube adapters) and the keyboard; the result
// feeds the SI model (hw/hw.cpp), which answers the game's own controller polling.
#include <windows.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_joystick.h>
#include <aurora/pad.hpp>
#include <dolphin/pad.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hw/gx_fifo.h"
#include "hotkeys.h"
#include "runtime.h"

namespace rt::input {

namespace {
bool s_ready = false;

// R2_INPUT="<vblank>:<BUTTON>[*frames],...": scripted presses for automated runs.
struct Press {
    u64 start, frames;
    u32 button;  // GameCube buttons; kStick* above them
};
// script names SUP / SDOWN / SLEFT / SRIGHT: the main stick pushed fully that way
constexpr u32 kStickUp = 1u << 16, kStickDown = 1u << 17, kStickLeft = 1u << 18, kStickRight = 1u << 19;
std::vector<Press> s_script;
u64 s_vblank = 0;

void parseScript(const char* s) {
    static const struct {
        const char* name;
        u32 bit;
    } names[] = {{"LEFT", 0x0001},      {"RIGHT", 0x0002},     {"DOWN", 0x0004},      {"UP", 0x0008},
                 {"Z", 0x0010},         {"R", 0x0020},         {"L", 0x0040},         {"A", 0x0100},
                 {"B", 0x0200},         {"X", 0x0400},         {"Y", 0x0800},         {"START", 0x1000},
                 {"SUP", kStickUp},     {"SDOWN", kStickDown}, {"SLEFT", kStickLeft}, {"SRIGHT", kStickRight}};
    while (*s) {
        char* end;
        const u64 start = strtoull(s, &end, 10);
        if (*end != ':') break;
        s = end + 1;
        std::string name;
        while (*s && *s != '*' && *s != ',') name += *s++;
        u64 frames = 6;
        if (*s == '*') frames = strtoull(s + 1, const_cast<char**>(&s), 10);
        for (auto& n : names)
            if (name == n.name) s_script.push_back({start, frames, n.bit});
        while (*s == ',') s++;
    }
    logf("input", "%zu scripted presses", s_script.size());
}

void defaultKeyboard() {
    using aurora::binding::Binding;
    using aurora::binding::PhysicalInput;
    const auto& c = aurora::pad::controls();
    std::vector<Binding> keys;
    auto key = [&](aurora::binding::ControlId target, SDL_Scancode k, float scale = 1.f) {
        keys.push_back({.input = {.control = PhysicalInput::Key{k}}, .target = target, .scale = scale});
    };
    // Flight: WASD or the arrow keys; C-stick: IJKL.
    key(c.leftX, SDL_SCANCODE_A, -1.f);
    key(c.leftX, SDL_SCANCODE_D, 1.f);
    key(c.leftY, SDL_SCANCODE_W, 1.f);
    key(c.leftY, SDL_SCANCODE_S, -1.f);
    key(c.leftX, SDL_SCANCODE_LEFT, -1.f);
    key(c.leftX, SDL_SCANCODE_RIGHT, 1.f);
    key(c.leftY, SDL_SCANCODE_UP, 1.f);
    key(c.leftY, SDL_SCANCODE_DOWN, -1.f);
    key(c.rightX, SDL_SCANCODE_J, -1.f);
    key(c.rightX, SDL_SCANCODE_L, 1.f);
    key(c.rightY, SDL_SCANCODE_I, 1.f);
    key(c.rightY, SDL_SCANCODE_K, -1.f);
    key(c.a, SDL_SCANCODE_SPACE);
    key(c.b, SDL_SCANCODE_LSHIFT);
    key(c.x, SDL_SCANCODE_E);
    key(c.y, SDL_SCANCODE_Q);
    key(c.z, SDL_SCANCODE_R);
    key(c.l, SDL_SCANCODE_1);
    key(c.triggerL, SDL_SCANCODE_1);
    key(c.r, SDL_SCANCODE_2);
    key(c.triggerR, SDL_SCANCODE_2);
    key(c.start, SDL_SCANCODE_RETURN);
    key(c.up, SDL_SCANCODE_T);
    key(c.down, SDL_SCANCODE_G);
    key(c.left, SDL_SCANCODE_F);
    key(c.right, SDL_SCANCODE_H);
    aurora::pad::set_default_keyboard_bindings(0, std::move(keys));
}

// --- controller updates off the game thread ---------------------------------------------------
// SDL rescans its controllers inside SDL_UpdateJoysticks when Windows reports a device change:
// DirectInput, SDL's HIDAPI driver and its raw input driver each open every HID device and ask
// it for its names (on the user's machine 52 interfaces, ~0.5 s a pass: a Steam Controller
// puck's 13 at 18 ms each, wireless receivers, RGB controllers), holding SDL's joystick lock.
// SDL's event pump ran that on the game's thread: frames of 3-4 s at random moments, mid-cutscene
// too (Battle of Hoth; the profile showed hid.dll under dinput8.dll and SDL3.dll). The input
// thread now updates the controllers (SDL_HINT_AUTO_UPDATE_JOYSTICKS is off, gx_bridge.cpp)
// under s_joyMutex, and the game thread's own SDL controller calls (pads, rumble, hotkeys, menu)
// only try that mutex (JoystickAccess): while a scan runs they keep the previous state. Aurora's
// controller state itself comes from SDL events, which the window's event pump still delivers.
std::mutex s_joyMutex;

void joystickThread() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -20000;  // 2 ms (100 ns units), then every 2 ms: 500 Hz
    SetWaitableTimer(timer, &due, 2, nullptr, nullptr, FALSE);
    u64 lastLog = 0;
    for (;;) {
        WaitForSingleObject(timer, INFINITE);
        // A controller came or went: Aurora opens or closes it on the game thread (SDL calls that
        // wait for the joystick lock), so no new scan starts before it has.
        if (SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMAPPED) > 0) continue;
        const u64 t0 = tbNow();
        {
            std::lock_guard lock(s_joyMutex);
            SDL_UpdateJoysticks();
        }
        const u64 t1 = tbNow();
        if (t1 - t0 > kTbClock / 10 && t1 - lastLog > kTbClock) {
            lastLog = t1;
            logf("input", "controller scan: %.0f ms on the input thread (the game kept running)",
                 double(t1 - t0) * 1000.0 / double(kTbClock));
        }
    }
}

// R2_JOY_SCAN_TEST=<seconds>: from then on, every 5 s the input thread holds SDL's joystick lock
// for 3 s, as a slow device scan does (the game thread must keep its frame rate)
void scanTest() {
    const char* e = getenv("R2_JOY_SCAN_TEST");
    if (!e) return;
    const double start = atof(e);
    std::thread([start] {
        Sleep(DWORD(start * 1000));
        for (int i = 0; i < 6; i++) {
            {
                std::lock_guard lock(s_joyMutex);
                SDL_LockJoysticks();
                logf("input", "scan test: joystick lock held for 3 s");
                Sleep(3000);
                SDL_UnlockJoysticks();
            }
            Sleep(2000);
        }
    }).detach();
}

PADStatus s_lastPads[PAD_CHANMAX] = {};  // what PADRead gave last (reused while a scan runs)
u32 s_rumbleWant[PAD_CHANMAX] = {}, s_rumbleSent[PAD_CHANMAX] = {};

void applyRumble() {  // JoystickAccess held
    for (u32 i = 0; i < PAD_CHANMAX; i++)
        if (s_rumbleWant[i] != s_rumbleSent[i]) {
            s_rumbleSent[i] = s_rumbleWant[i];
            PADControlMotor(i, s_rumbleWant[i]);
        }
}
}  // namespace

JoystickAccess::JoystickAccess() : m_held(s_joyMutex.try_lock()) {}
JoystickAccess::~JoystickAccess() {
    if (m_held) s_joyMutex.unlock();
}

bool s_pads = false;  // Aurora PAD available (not with R2_NO_GFX)

void init(bool pads) {
    s_pads = pads;
    for (auto& e : s_lastPads) e.err = PAD_ERR_NO_CONTROLLER;
    if (pads) {
        defaultKeyboard();
        PADInit();
        std::thread(joystickThread).detach();
        scanTest();
    }
    if (const char* s = getenv("R2_INPUT")) parseScript(s);
    s_ready = true;
}

void rumble(int chan, u32 cmd) {
    if (!s_pads || chan < 0 || chan > 3) return;
    s_rumbleWant[chan] = cmd;
    if (JoystickAccess joy; joy) applyRumble();  // else at the next poll
}

void poll() {
    if (!s_ready) return;
    s_vblank++;
    PADStatus st[PAD_CHANMAX] = {};
    if (!s_pads) {
        for (auto& e : st) e.err = PAD_ERR_NO_CONTROLLER;
    } else if (JoystickAccess joy; joy) {
        PADRead(st);
        memcpy(s_lastPads, st, sizeof st);
        applyRumble();
    } else {
        memcpy(st, s_lastPads, sizeof st);
    }
    // a hotkey's modifier (Back) is down, or a hotkey just fired: its buttons are not the game's
    if (hotkeys::muteGame())
        for (auto& e : st) {
            e.button = 0;
            e.triggerLeft = e.triggerRight = 0;
        }
    u32 scripted = 0;
    // script times count 59.94 Hz retraces, whatever the retrace rate (settings fps; it also
    // changes while a cinematic is skipped)
    static double scriptTime = 0;
    scriptTime += (60000.0 / 1001.0) / viRate();
    const u64 t = u64(scriptTime);
    for (auto& p : s_script)
        if (t >= p.start && t < p.start + p.frames) scripted |= p.button;
    if (scripted) {
        st[0].err = PAD_ERR_NONE;
        st[0].button |= u16(scripted);
        if (scripted & kStickUp) st[0].stickY = 127;
        if (scripted & kStickDown) st[0].stickY = -127;
        if (scripted & kStickLeft) st[0].stickX = -127;
        if (scripted & kStickRight) st[0].stickX = 127;
    }
    static const bool padLog = envFlag("R2_PAD_LOG");
    if (padLog && s_vblank % 30 == 0) {
        for (int i = 0; i < 2; i++) {
            const auto src = s_pads ? aurora::pad::port_controller(u32(i)) : 0;
            logf("pad", "port %d: err %d buttons %04X stick %d,%d c %d,%d triggers %u,%u controller %llu", i, st[i].err,
                 st[i].button, st[i].stickX, st[i].stickY, st[i].substickX, st[i].substickY, st[i].triggerLeft,
                 st[i].triggerRight, (unsigned long long)src);
        }
    }
    const bool menu = gfx::menuOpen();
    // After the menu closes, a controller stays idle until its buttons are let go: B (which
    // closes the menu) or A must not reach the game as a press.
    static bool heldFromMenu[PAD_CHANMAX] = {};
    for (int i = 0; i < PAD_CHANMAX; i++) {
        PadState& p = padState(i);
        if (menu) heldFromMenu[i] = true;
        else if (heldFromMenu[i] && (st[i].err != PAD_ERR_NONE || (st[i].button & 0x1F7F) == 0)) heldFromMenu[i] = false;
        const bool idle = menu || heldFromMenu[i];
        if (st[i].err == PAD_ERR_NO_CONTROLLER || idle) {
            // Port 1 always has a controller: the game asks for one otherwise.
            p.connected = i == 0 || (idle && st[i].err != PAD_ERR_NO_CONTROLLER);
            p.buttons = 0x0080;
            p.stickX = p.stickY = p.substickX = p.substickY = 0x80;
            p.triggerL = p.triggerR = 0;
            continue;
        }
        p.connected = true;
        p.buttons = u16((st[i].button & 0x1F7F) | 0x0080);  // PAD_USE_ORIGIN
        p.stickX = u8(st[i].stickX + 128);
        p.stickY = u8(st[i].stickY + 128);
        p.substickX = u8(st[i].substickX + 128);
        p.substickY = u8(st[i].substickY + 128);
        p.triggerL = st[i].triggerLeft;
        p.triggerR = st[i].triggerRight;
    }
}

}  // namespace rt::input
