// Controllers: Aurora reads gamepads (SDL3, GameCube adapters) and the keyboard; the result
// feeds the SI model (hw/hw.cpp), which answers the game's own controller polling.
#include <aurora/pad.hpp>
#include <dolphin/pad.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "hw/gx_fifo.h"
#include "runtime.h"

namespace rt::input {

namespace {
bool s_ready = false;

// R2_INPUT="<vblank>:<BUTTON>[*frames],...": scripted presses for automated runs.
struct Press {
    u64 start, frames;
    u16 button;
};
std::vector<Press> s_script;
u64 s_vblank = 0;

void parseScript(const char* s) {
    static const struct {
        const char* name;
        u16 bit;
    } names[] = {{"LEFT", 0x0001}, {"RIGHT", 0x0002}, {"DOWN", 0x0004}, {"UP", 0x0008}, {"Z", 0x0010}, {"R", 0x0020},
                 {"L", 0x0040}, {"A", 0x0100}, {"B", 0x0200}, {"X", 0x0400}, {"Y", 0x0800}, {"START", 0x1000}};
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
}  // namespace

void init() {
    defaultKeyboard();
    PADInit();
    if (const char* s = getenv("R2_INPUT")) parseScript(s);
    s_ready = true;
}

void poll() {
    if (!s_ready) return;
    s_vblank++;
    PADStatus st[PAD_CHANMAX];
    PADRead(st);
    u16 scripted = 0;
    for (auto& p : s_script)
        if (s_vblank >= p.start && s_vblank < p.start + p.frames) scripted |= p.button;
    if (scripted) {
        st[0].err = PAD_ERR_NONE;
        st[0].button |= scripted;
    }
    for (int i = 0; i < PAD_CHANMAX; i++) {
        PadState& p = padState(i);
        if (st[i].err == PAD_ERR_NO_CONTROLLER) {
            // Port 1 always has a controller: the game asks for one otherwise.
            p.connected = i == 0;
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
