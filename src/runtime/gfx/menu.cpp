// In-game settings menu: Dear ImGui (through Aurora), drawn over the game's frame. F1 or the
// controller's Back/Select button (the GameCube pad has none, so the game never sees it) opens
// and closes it - Back when it is let go without another button pressed meanwhile, since Back
// also starts the save state hotkeys (Back + RB); Escape or the controller's B closes it. While
// it is open the game sees idle controllers (input.cpp, which also keeps buttons held when it
// closes from the game).
//
// Tabs run along the top: LB / RB (Q / E, Tab) switch them. The d-pad or the left stick (arrow
// keys) moves through a page's rows and left / right changes the selected value; A (Enter)
// toggles or picks, X (Delete) clears a binding. The mouse works everywhere too.
//
// The look: a holo-display over a starfield - a chamfered navy panel with glowing gold edges,
// corner brackets and scanlines, Star Wars crawl gold for titles, the radar's amber for
// highlights, Bahnschrift (Windows' DIN-style face) for type.
#include "gx_bridge.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <aurora/pad.hpp>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../hotkeys.h"
#include "../hw/musyx_dsp.h"
#include "../mods.h"
#include "../runtime.h"
#include "../savestate.h"
#include "../settings.h"

namespace rt::gfx {
namespace {

// ---------------------------------------------------------------------------------------
// theme
// ---------------------------------------------------------------------------------------
constexpr ImU32 kGold = IM_COL32(255, 214, 70, 255);    // the crawl's yellow
constexpr ImU32 kAmber = IM_COL32(255, 150, 40, 255);   // the radar frame
constexpr ImU32 kHolo = IM_COL32(110, 205, 255, 255);   // hologram blue
constexpr ImU32 kText = IM_COL32(226, 233, 245, 255);
constexpr ImU32 kDim = IM_COL32(128, 146, 176, 255);
constexpr ImU32 kPanelTop = IM_COL32(10, 20, 40, 238);
constexpr ImU32 kPanelBottom = IM_COL32(2, 5, 13, 246);
constexpr ImU32 kFrame = IM_COL32(13, 28, 54, 235);

ImU32 alpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (ImU32(std::clamp(a, 0, 255)) << IM_COL32_A_SHIFT); }
ImVec4 vec(ImU32 c, float a = -1.f) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    if (a >= 0.f) v.w = a;
    return v;
}

ImFont* s_font = nullptr;     // body text
ImFont* s_fontBig = nullptr;  // titles
constexpr float kFontPx = 34.f, kBigPx = 72.f;

void applyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.f;
    st.ChildRounding = 0.f;
    st.FrameRounding = 2.f;
    st.PopupRounding = 2.f;
    st.GrabRounding = 2.f;
    st.WindowBorderSize = 0.f;
    st.ChildBorderSize = 0.f;
    st.FrameBorderSize = 1.f;
    st.PopupBorderSize = 1.f;
    st.ScrollbarSize = 10.f;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = vec(kText);
    c[ImGuiCol_TextDisabled] = vec(kDim);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.07f, 0.14f, 0.98f);
    c[ImGuiCol_Border] = vec(kGold, 0.35f);
    c[ImGuiCol_FrameBg] = ImVec4(0.05f, 0.11f, 0.21f, 0.90f);
    c[ImGuiCol_FrameBgHovered] = vec(kHolo, 0.22f);
    c[ImGuiCol_FrameBgActive] = vec(kHolo, 0.35f);
    c[ImGuiCol_Button] = ImVec4(0.06f, 0.13f, 0.25f, 0.92f);
    c[ImGuiCol_ButtonHovered] = vec(kHolo, 0.30f);
    c[ImGuiCol_ButtonActive] = vec(kAmber, 0.65f);
    c[ImGuiCol_Header] = vec(kAmber, 0.22f);
    c[ImGuiCol_HeaderHovered] = vec(kHolo, 0.28f);
    c[ImGuiCol_HeaderActive] = vec(kAmber, 0.45f);
    c[ImGuiCol_CheckMark] = vec(kGold);
    c[ImGuiCol_SliderGrab] = vec(kAmber);
    c[ImGuiCol_Separator] = vec(kGold, 0.25f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = vec(kGold, 0.25f);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(kGold, 0.45f);
    c[ImGuiCol_ScrollbarGrabActive] = vec(kAmber, 0.7f);
    c[ImGuiCol_NavCursor] = vec(kAmber);
    c[ImGuiCol_TextSelectedBg] = vec(kHolo, 0.35f);
}

// text with letter spacing (tracking) and an optional glow, at a size in pixels
void drawText(ImDrawList* dl, ImFont* font, float px, ImVec2 at, ImU32 col, const char* text, float tracking = 0.f,
              ImU32 glow = 0) {
    if (!font) font = ImGui::GetFont();
    auto pass = [&](ImVec2 p, ImU32 c) {
        for (const char* s = text; *s; s++) {
            const char buf[2] = {*s, 0};
            dl->AddText(font, px, p, c, buf);
            p.x += font->CalcTextSizeA(px, FLT_MAX, 0.f, buf).x + tracking;
        }
    };
    if (glow) {
        const float r = std::max(1.f, px * 0.05f);
        for (int i = 0; i < 8; i++) {
            const float a = float(i) * 0.785398f;
            pass(ImVec2(at.x + std::cos(a) * r, at.y + std::sin(a) * r), glow);
        }
    }
    pass(at, col);
}
float textWidth(ImFont* font, float px, const char* text, float tracking = 0.f) {
    if (!font) font = ImGui::GetFont();
    const size_t n = strlen(text);
    return font->CalcTextSizeA(px, FLT_MAX, 0.f, text).x + tracking * float(n > 0 ? n - 1 : 0);
}

// a panel with two cut corners (top-left, bottom-right), a vertical gradient and a glowing edge
void panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 top, ImU32 bottom, ImU32 edge, float thick) {
    const ImVec2 pts[6] = {ImVec2(a.x + cut, a.y), ImVec2(b.x, a.y), ImVec2(b.x, b.y - cut),
                           ImVec2(b.x - cut, b.y), ImVec2(a.x, b.y), ImVec2(a.x, a.y + cut)};
    const int start = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(pts, 6, top);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, start, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), top, bottom);
    if (edge) {
        dl->AddPolyline(pts, 6, alpha(edge, 40), ImDrawFlags_Closed, thick * 5.f);
        dl->AddPolyline(pts, 6, alpha(edge, 70), ImDrawFlags_Closed, thick * 2.5f);
        dl->AddPolyline(pts, 6, edge, ImDrawFlags_Closed, thick);
    }
}

void brackets(ImDrawList* dl, ImVec2 a, ImVec2 b, float len, ImU32 col, float thick) {
    const float o = thick * 3.f;
    const ImVec2 tr[3] = {ImVec2(b.x - len, a.y - o), ImVec2(b.x + o, a.y - o), ImVec2(b.x + o, a.y + len)};
    const ImVec2 bl[3] = {ImVec2(a.x - o, b.y - len), ImVec2(a.x - o, b.y + o), ImVec2(a.x + len, b.y + o)};
    dl->AddPolyline(tr, 3, col, ImDrawFlags_None, thick);
    dl->AddPolyline(bl, 3, col, ImDrawFlags_None, thick);
}

// slowly drifting, twinkling stars behind the panel
void starfield(ImDrawList* dl, ImVec2 size, float t) {
    struct Star {
        float x, y, r, phase, speed;
    };
    static std::vector<Star> stars;
    if (stars.empty()) {
        u32 seed = 0x5EED2001;
        auto rnd = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return float(seed >> 8) / float(1 << 24);
        };
        for (int i = 0; i < 260; i++)
            stars.push_back({rnd(), rnd(), 0.4f + rnd() * rnd() * 1.6f, rnd() * 6.28f, 0.004f + rnd() * 0.012f});
    }
    const float scale = size.y / 1080.f;
    for (const Star& s : stars) {
        const float x = std::fmod(s.x + t * s.speed, 1.f) * size.x;
        const float tw = 0.55f + 0.45f * std::sin(t * 2.2f + s.phase);
        dl->AddCircleFilled(ImVec2(x, s.y * size.y), s.r * scale * (0.8f + 0.4f * tw), alpha(kText, int(200 * tw)), 8);
    }
}

// ---------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------
bool s_open = false;
bool s_focus = false;  // just opened: focus the window
u64 s_fpsFrames = 0, s_fpsTb = 0;
float s_fps = 0.f;
// automated runs: nobody's controller drives the menu (or opens it over a scripted test)
bool automatedRun() {
    static const bool automated = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    return automated;
}
// R2_MENU_PAD_TEST=1 (automated runs): a virtual gamepad drives the menu (padSelfTest)
bool padTest() {
    static const bool on = envFlag("R2_MENU_PAD_TEST") && automatedRun();
    return on;
}

enum Section { Display, Gameplay, Audio, Controls, Cheats, Mods, SectionCount };
const char* const kSectionNames[SectionCount] = {"DISPLAY", "GAMEPLAY", "AUDIO", "CONTROLS", "CHEATS", "MODS"};
int s_section = 0;
int s_rowSel[SectionCount] = {};    // each page's selected row
int s_rowCount[SectionCount] = {};  // the rows each page had when it was last drawn

// --- navigation: controllers and keyboard ------------------------------------------------
// The menu reads controllers itself: every gamepad SDL has open (Aurora opens each one as it
// connects). ImGui's SDL backend lists gamepads once and refreshes the list only on connection
// events, which Aurora's input router keeps from it, so a controller that showed up after
// startup (Steam Input's virtual pad, a wireless pad waking up) never reached the menu.
enum Nav { NavUp, NavDown, NavLeft, NavRight, NavAccept, NavBack, NavClear, NavPrevTab, NavNextTab, NavCount };
struct NavKey {
    bool held = false;
    double next = 0.0;  // directions: when the held press repeats
};
NavKey s_navKey[NavCount];
bool s_nav[NavCount];    // this frame: pressed (or repeated, for directions)
bool s_navFresh = true;  // just opened: buttons already held are not presses
bool s_usingPad = false; // the last input came from a controller (button hints)

void readPads(bool held[NavCount]) {
    static bool last[NavCount] = {};  // while the input thread updates the controllers (a device scan)
    input::JoystickAccess joy;
    if (!joy) {
        for (int k = 0; k < NavCount; k++) held[k] |= last[k];
        return;
    }
    struct Keep {  // what this read found, for the frames a scan blocks reading
        bool* held;
        ~Keep() { std::copy(held, held + NavCount, last); }
    } keep{held};
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        auto btn = [g](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
        const int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        const int ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        constexpr int kDead = 16000;
        const bool vertical = std::abs(ly) >= std::abs(lx);  // only the stick's main direction
        held[NavUp] |= btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || (vertical && ly < -kDead);
        held[NavDown] |= btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || (vertical && ly > kDead);
        held[NavLeft] |= btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || (!vertical && lx < -kDead);
        held[NavRight] |= btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || (!vertical && lx > kDead);
        held[NavAccept] |= btn(SDL_GAMEPAD_BUTTON_SOUTH);
        held[NavBack] |= btn(SDL_GAMEPAD_BUTTON_EAST);
        held[NavClear] |= btn(SDL_GAMEPAD_BUTTON_WEST);
        held[NavPrevTab] |= btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        held[NavNextTab] |= btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    }
    SDL_free(ids);
}

// this frame's presses; `enabled` false (a rebind in progress) still tracks what is held
void pollNav(bool enabled) {
    bool held[NavCount] = {};
    if (!automatedRun() || padTest() || getenv("R2_HOTKEY_TEST")) readPads(held);
    const double now = double(SDL_GetTicksNS()) * 1e-9;
    for (int k = 0; k < NavCount; k++) {
        NavKey& st = s_navKey[k];
        const bool repeats = k <= NavRight;
        bool fire = false;
        if (held[k] && !st.held) {
            fire = !s_navFresh;
            st.next = now + 0.40;
        } else if (held[k] && repeats && now >= st.next) {
            fire = !s_navFresh;
            st.next = now + 0.09;
        }
        st.held = held[k];
        s_nav[k] = fire && enabled;
        if (s_nav[k]) s_usingPad = true;
    }
    s_navFresh = false;
    if (!enabled) return;
    // keyboard (ImGui does the key repeat)
    const bool shift = ImGui::GetIO().KeyShift;
    bool kb[NavCount] = {};
    kb[NavUp] = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    kb[NavDown] = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    kb[NavLeft] = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
    kb[NavRight] = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
    kb[NavAccept] = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Space, false);
    kb[NavClear] = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    kb[NavPrevTab] = ImGui::IsKeyPressed(ImGuiKey_Q, false) || (shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    kb[NavNextTab] = ImGui::IsKeyPressed(ImGuiKey_E, false) || (!shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    for (int k = 0; k < NavCount; k++)
        if (kb[k]) {
            s_nav[k] = true;
            s_usingPad = false;
        }
}

// A / Enter / Space still down (a rebind starts once it is released, or it would bind it)
bool acceptHeld() {
    return s_navKey[NavAccept].held || ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) ||
           ImGui::IsKeyDown(ImGuiKey_Space);
}

// --- control remapping ---------------------------------------------------------------------
struct ControlRow {
    const char* name;
    aurora::binding::ControlId control, second;  // second: the analog half of L/R
    int sign;                                    // stick directions: +1 / -1 (0: a button)
};

std::vector<ControlRow> controlRows() {
    const auto& c = aurora::pad::controls();
    return {
        {"A", c.a, 0, 0},
        {"B", c.b, 0, 0},
        {"X", c.x, 0, 0},
        {"Y", c.y, 0, 0},
        {"Z", c.z, 0, 0},
        {"START", c.start, 0, 0},
        {"L TRIGGER", c.l, c.triggerL, 0},
        {"R TRIGGER", c.r, c.triggerR, 0},
        {"D-PAD UP", c.up, 0, 0},
        {"D-PAD DOWN", c.down, 0, 0},
        {"D-PAD LEFT", c.left, 0, 0},
        {"D-PAD RIGHT", c.right, 0, 0},
        {"STICK UP", c.leftY, 0, +1},
        {"STICK DOWN", c.leftY, 0, -1},
        {"STICK LEFT", c.leftX, 0, -1},
        {"STICK RIGHT", c.leftX, 0, +1},
        {"C-STICK UP", c.rightY, 0, +1},
        {"C-STICK DOWN", c.rightY, 0, -1},
        {"C-STICK LEFT", c.rightX, 0, -1},
        {"C-STICK RIGHT", c.rightX, 0, +1},
    };
}

bool s_padDevice = false;  // controls page: false keyboard, true controller

aurora::pad::ProfileTarget profileTarget() {
    return s_padDevice ? aurora::pad::ProfileTarget::controller(aurora::pad::port_controller(0))
                       : aurora::pad::ProfileTarget::keyboard(0);
}
SDL_Gamepad* targetGamepad() {
    if (!s_padDevice) return nullptr;
    const auto src = aurora::pad::port_controller(0);
    return src ? SDL_GetGamepadFromID(aurora::input::gamepad_for_source(src)) : nullptr;
}

// which output sign a binding produces for an axis control (0: both)
int bindingSign(const aurora::binding::Binding& b) {
    using Dir = aurora::binding::PhysicalInput::GamepadAxis::Direction;
    if (const auto* ax = b.input.control.get_if<aurora::binding::PhysicalInput::GamepadAxis>()) {
        if (ax->direction == Dir::Full) return 0;
    }
    return b.scale < 0.f ? -1 : +1;
}

std::string bindingText(const ControlRow& row) {
    std::string out;
    SDL_Gamepad* pad = targetGamepad();
    for (const auto& b : aurora::pad::bindings_for_control(profileTarget(), row.control)) {
        if (row.sign != 0) {
            const int s = bindingSign(b);
            if (s != 0 && s != row.sign) continue;
        }
        if (!out.empty()) out += " / ";
        out += aurora::binding::input_label(b.input, pad);
    }
    return out.empty() ? std::string("-") : out;
}

// apply a captured input to a row
void bindRow(const ControlRow& row, const aurora::binding::PhysicalInput& in) {
    using namespace aurora::binding;
    using Dir = PhysicalInput::GamepadAxis::Direction;
    const auto target = profileTarget();
    if (row.sign == 0) {
        aurora::pad::rebind(target, row.control, in);
        if (row.second) aurora::pad::rebind(target, row.second, in, aurora::pad::ConflictPolicy::Allow);
    } else {
        // keep the other direction's bindings (splitting a full axis into its other half)
        std::vector<Binding> list;
        for (auto b : aurora::pad::bindings_for_control(target, row.control)) {
            const int s = bindingSign(b);
            if (s == row.sign) continue;
            if (s == 0) {
                auto ax = *b.input.control.get_if<PhysicalInput::GamepadAxis>();
                const int other = -row.sign;
                ax.direction = float(other) * b.scale > 0.f ? Dir::Positive : Dir::Negative;
                b.input.control = ax;
                b.scale = float(other);
            }
            list.push_back(b);
        }
        Binding nb{.input = in, .target = row.control};
        nb.scale = float(row.sign);
        list.push_back(nb);
        aurora::pad::set_binding(target, row.control, std::move(list));
    }
    aurora::pad::save_profiles();
}

void clearRow(const ControlRow& row) {
    const auto target = profileTarget();
    std::vector<aurora::binding::Binding> keep;
    if (row.sign != 0)
        for (const auto& b : aurora::pad::bindings_for_control(target, row.control))
            if (bindingSign(b) == -row.sign) keep.push_back(b);
    aurora::pad::set_binding(target, row.control, std::move(keep));
    if (row.second) aurora::pad::set_binding(target, row.second, {});
    aurora::pad::save_profiles();
}

// capture in progress: the row, and the input once it arrives (from the event loop)
int s_captureRow = -1;
int s_pendingCapture = -1;  // chosen with A / Enter: starts once that is released
bool s_captured = false, s_captureCancel = false, s_captureRearm = false;
aurora::binding::PhysicalInput s_capturedInput;
u64 s_navBackTb = 0;  // keyboard/controller navigation resumes after a capture (not on its press)

void startCapture(int row) {
    s_captureRow = row;
    s_captured = s_captureCancel = false;
    aurora::binding::capture_next([](const aurora::binding::PhysicalInput& in) {
        using namespace aurora::binding;
        if (const auto* k = in.control.get_if<PhysicalInput::Key>()) {
            if (k->scancode == SDL_SCANCODE_ESCAPE || k->scancode == SDL_SCANCODE_F1) {
                s_captureCancel = true;
                return;
            }
        }
        if (const auto* g = in.control.get_if<PhysicalInput::GamepadButton>()) {
            if (g->button == SDL_GAMEPAD_BUTTON_BACK) {
                s_captureCancel = true;
                return;
            }
        }
        if (in.control.get_if<PhysicalInput::MouseButton>()) {  // a click elsewhere: cancel
            s_captureCancel = true;
            return;
        }
        // keys for the keyboard page, buttons and sticks for the controller page
        const bool isKey = in.control.get_if<PhysicalInput::Key>() != nullptr;
        if (isKey == s_padDevice) {
            s_captureRearm = true;
            return;
        }
        s_capturedInput = in;
        s_captured = true;
    });
}

void finishCapture(const std::vector<ControlRow>& rows) {
    if (s_captureRow >= 0 && s_captureRearm) {  // the other device's input: keep listening
        s_captureRearm = false;
        startCapture(s_captureRow);
        return;
    }
    if (s_captureRow < 0 || !(s_captured || s_captureCancel)) return;
    if (s_captured && s_captureRow < int(rows.size())) bindRow(rows[size_t(s_captureRow)], s_capturedInput);
    s_captureRow = -1;
    s_captured = s_captureCancel = false;
    s_navBackTb = tbNow() + kTbClock / 4;
}

// hotkey rows (hotkeys.cpp captures a combination: held together, then let go)
int s_pendingHotkey = -1;  // chosen with A / Enter: the capture starts once that is released
bool s_hotkeyWasCapturing = false;

void cancelCapture() {
    if (s_captureRow >= 0) aurora::binding::cancel_capture();
    s_captureRow = s_pendingCapture = -1;
    s_pendingHotkey = -1;
    hotkeys::captureCancel();
}

bool rebindingNow() {
    return s_captureRow >= 0 || s_pendingCapture >= 0 || s_pendingHotkey >= 0 || hotkeys::capturing() ||
           (s_navBackTb && tbNow() < s_navBackTb);
}

// ---------------------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------------------
// A page is a list of rows: a label on the left, its value from s_valueX. One row is selected
// (the controller's and keyboard's cursor; hovering with a moving mouse selects too) and gets
// this frame's left/right, accept and clear presses.
float s_ui = 1.f;      // size of the UI: the display's height / 1080
float s_valueX = 0.f;  // where values start in a row
struct RowInput {
    int dx = 0;  // left -1 / right +1
    bool accept = false, clear = false;
} s_in;
int s_row = 0;               // rows drawn so far on this page
bool s_scrollToSel = false;  // the selection moved: bring it into view
const char* s_help = nullptr;  // the selected row's description

void bodyFont(float px) { ImGui::SetWindowFontScale(px / kFontPx); }

bool rowBegin(const char* text, const char* help) {
    const int idx = s_row++;
    int& selRow = s_rowSel[s_section];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight(), padY = ImGui::GetStyle().ItemSpacing.y * 0.5f;
    const ImVec2 a(p.x, p.y - padY), b(p.x + ImGui::GetContentRegionAvail().x, p.y + h + padY);
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f) && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b))
        selRow = idx;
    const bool sel = idx == selRow;
    if (sel) {
        dl->AddRectFilledMultiColor(a, b, alpha(kHolo, 50), alpha(kHolo, 4), alpha(kHolo, 4), alpha(kHolo, 50));
        dl->AddRectFilled(a, ImVec2(a.x + 4.f * s_ui, b.y), kAmber);
        // (copied: some pages build their descriptions on the fly)
        static std::string helpText;
        helpText = help ? help : "";
        s_help = help ? helpText.c_str() : nullptr;
        if (s_scrollToSel) {
            s_scrollToSel = false;
            if (idx == 0) ImGui::SetScrollY(0.f);  // the page's first heading too
            else ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(a, b), ImGuiScrollFlags_KeepVisibleEdgeY);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 20.f * s_ui, p.y));
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, sel ? vec(kGold) : vec(kText));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SameLine(s_valueX);
    return sel;
}

// a framed box for a value
void valueFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, bool sel, bool hot) {
    dl->AddRectFilled(a, b, hot ? IM_COL32(24, 52, 92, 240) : kFrame, 2.f);
    dl->AddRect(a, b, sel ? alpha(kAmber, 220) : alpha(kGold, hot ? 140 : 70), 2.f, 0, 1.f);
}

// ON / OFF switch
bool onOff(const char* id, bool& v, bool sel) {
    const float w = 150.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##onoff", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    if (v) {
        dl->AddRectFilledMultiColor(p, b, alpha(kAmber, 170), alpha(kAmber, 120), alpha(kAmber, 90), alpha(kAmber, 140));
        dl->AddRect(p, b, sel ? kGold : alpha(kAmber, 220), 2.f, 0, 1.f);
    } else {
        valueFrame(dl, p, b, sel, hot);
    }
    const char* text = v ? "ON" : "OFF";
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                v ? IM_COL32(255, 245, 220, 255) : kDim, text);
    if (clicked) v = !v;
    return clicked;
}

bool toggleRow(const char* text, bool& v, const char* help = nullptr) {
    const bool sel = rowBegin(text, help);
    bool changed = onOff(text, v, sel);
    if (sel && (s_in.accept || s_in.dx != 0)) {
        v = !v;
        changed = true;
    }
    return changed;
}

// a value with arrows either side: the mouse clicks one half, the controller presses left/right.
// Returns the step clicked (-1 / +1, 0 none).
int selector(const char* id, const char* text, bool canPrev, bool canNext, bool sel) {
    const float w = 380.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##selector", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    valueFrame(dl, p, ImVec2(p.x + w, p.y + h), sel, hot);
    const float ah = h * 0.2f, cy = p.y + h * 0.5f, inset = 18.f * s_ui;
    auto arrow = [&](float x, float dir, bool on) {
        const ImU32 c = on ? (sel ? kGold : kText) : alpha(kDim, 70);
        dl->AddTriangleFilled(ImVec2(x + dir * ah, cy), ImVec2(x - dir * ah, cy - ah * 1.3f), ImVec2(x - dir * ah, cy + ah * 1.3f), c);
    };
    arrow(p.x + inset, -1.f, canPrev);
    arrow(p.x + w - inset, +1.f, canNext);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                sel ? kGold : kText, text);
    if (!clicked) return 0;
    return ImGui::GetIO().MousePos.x < p.x + w * 0.5f ? -1 : +1;
}

// one of `n` choices: left/right step (stopping at the ends), A goes round
int choiceStep(int cur, int n, int clickStep, bool sel) {
    int next = std::clamp(cur + clickStep + (sel ? s_in.dx : 0), 0, n - 1);
    if (sel && s_in.accept) next = (cur + 1) % n;
    return next;
}

template <typename E>
bool enumRow(const char* text, E& value, const char* (*name)(E), const char* help = nullptr) {
    const bool sel = rowBegin(text, help);
    const int n = int(E::Count), cur = int(value);
    const int next = choiceStep(cur, n, selector(text, name(value), cur > 0, cur < n - 1, sel), sel);
    if (next == cur) return false;
    value = E(next);
    return true;
}

bool fpsRow(const char* text, int& fps, const char* help) {
    const bool sel = rowBegin(text, help);
    constexpr int n = int(std::size(settings::kFpsChoices));
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (settings::kFpsChoices[i] == fps) cur = i;
    char buf[16];
    snprintf(buf, sizeof buf, "%d FPS", fps);
    const int next = choiceStep(cur, n, selector(text, buf, cur > 0, cur < n - 1, sel), sel);
    if (settings::kFpsChoices[next] == fps) return false;
    fps = settings::kFpsChoices[next];
    return true;
}

// 0..100: a bar the mouse drags, left/right step 5
bool percentRow(const char* text, int& v, const char* help) {
    const bool sel = rowBegin(text, help);
    const float w = 380.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(text);
    ImGui::InvisibleButton("##bar", ImVec2(w, h));
    const bool active = ImGui::IsItemActive(), hot = ImGui::IsItemHovered();
    ImGui::PopID();
    const float x0 = p.x + 14.f * s_ui, x1 = p.x + w - 14.f * s_ui, cy = p.y + h * 0.5f;
    int nv = v;
    if (active) nv = int(std::lround(std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.f, 1.f) * 100.f));
    if (sel && s_in.dx > 0) nv = std::min(100, (v / 5 + 1) * 5);
    if (sel && s_in.dx < 0) nv = std::max(0, ((v + 4) / 5 - 1) * 5);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    valueFrame(dl, p, ImVec2(p.x + w, p.y + h), sel, hot || active);
    const float th = 6.f * s_ui, fx = x0 + (x1 - x0) * float(nv) / 100.f;
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), IM_COL32(30, 52, 86, 255), th * 0.5f);
    for (int i = 1; i < 10; i++) {
        const float x = x0 + (x1 - x0) * float(i) / 10.f;
        dl->AddLine(ImVec2(x, cy - th * 1.4f), ImVec2(x, cy + th * 1.4f), alpha(kDim, 60), 1.f);
    }
    if (nv > 0)
        dl->AddRectFilledMultiColor(ImVec2(x0, cy - th * 0.5f), ImVec2(fx, cy + th * 0.5f), alpha(kAmber, 200), kGold, kGold,
                                    alpha(kAmber, 200));
    dl->AddCircleFilled(ImVec2(fx, cy), th * 1.5f, sel || active ? kGold : kText, 16);
    dl->AddCircle(ImVec2(fx, cy), th * 2.4f, alpha(kAmber, sel ? 160 : 60), 20, 1.f);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, sel ? vec(kGold) : vec(kText));
    ImGui::Text("%d%%", nv);
    ImGui::PopStyleColor();
    if (nv == v) return false;
    v = nv;
    return true;
}

// a button with a caption; true when pressed (click or A)
bool buttonRow(const char* text, const char* caption, const char* help, ImU32 captionCol = kText) {
    const bool sel = rowBegin(text, help);
    const float w = 380.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(text);
    const bool clicked = ImGui::InvisibleButton("##button", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    valueFrame(dl, p, ImVec2(p.x + w, p.y + h), sel, hot);
    const ImVec2 ts = ImGui::CalcTextSize(caption);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                sel ? kGold : captionCol, caption);
    return clicked || (sel && s_in.accept);
}

// a gold heading with a rule after it
void heading(const char* text) {
    ImGui::Dummy(ImVec2(0, 4.f * s_ui));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float px = 21.f * s_ui;
    drawText(dl, s_font, px, p, kGold, text, 3.f * s_ui);
    const float w = textWidth(s_font, px, text, 3.f * s_ui);
    const float y = p.y + px * 0.55f;
    dl->AddLine(ImVec2(p.x + w + 12.f * s_ui, y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, y), alpha(kGold, 70), 1.f);
    ImGui::Dummy(ImVec2(0, px + 4.f * s_ui));
}

// ---------------------------------------------------------------------------------------
// pages
// ---------------------------------------------------------------------------------------
bool s_changed = false, s_display = false;

void pageDisplay(settings::Settings& s) {
    heading("SCREEN");
    if (toggleRow("Fullscreen", s.fullscreen, "Also F11 or Alt+Enter.")) s_changed = s_display = true;
    if (enumRow("Internal resolution", s.resolution, settings::resolutionName,
                "What the game renders at. Window size follows the window; 4K renders 2160 lines whatever the window size."))
        s_changed = s_display = true;
    if (enumRow("Aspect ratio", s.aspect, settings::aspectName,
                "Widescreen widens the 3D view and fills the screen in cutscenes. At most 21:9: a wider window "
                "gets bars at the sides."))
        s_changed = s_display = true;
    if (toggleRow("HUD at screen edges", s.hudEdges, "Widescreen: the flight HUD sits at the edges of the screen."))
        s_changed = true;
    if (toggleRow("Fill menu backgrounds", s.tileMenus,
                  "Widescreen: the menus' panel backgrounds repeat into the sides of the screen."))
        s_changed = true;
    heading("PERFORMANCE");
    if (fpsRow("Frame rate", s.fps, "Above 60 the game runs more frames per second at its normal speed."))
        s_changed = s_display = true;
    if (toggleRow("Show frame rate", s.showFps, "Frames per second in the top-left corner.")) s_changed = true;
    if (toggleRow("Ubershaders", s.ubershaders,
                  "A shader seen for the first time is drawn with a generic one while it compiles: no pop-in."))
        s_changed = s_display = true;
}

void pageGameplay(settings::Settings& s) {
    heading("LOADING");
    if (toggleRow("Fast loading", s.fastDisc,
                  "Disc reads at SSD speed instead of the GameCube drive's 3 MB/s: a mission loads 10-15 s sooner."))
        s_changed = true;
    heading("CINEMATICS");
    if (toggleRow("Skip intro logos", s.skipLogos, "Reach the title screen in seconds.")) s_changed = true;
    if (toggleRow("Skippable cutscenes", s.skipCutscenes,
                  "Press B during the opening crawl or a cutscene to skip to the gameplay."))
        s_changed = true;
}

void pageAudio(settings::Settings& s) {
    heading("VOLUME");
    if (percentRow("Master volume", s.volume,
                   "Everything the game plays. Music, effects and speech have their own levels in the game's pause "
                   "menu (Sound Settings)."))
        s_changed = true;
    heading("OUTPUT");
    if (toggleRow("Sound in background", s.backgroundAudio,
                  "Keep playing sound while another window has the focus. Off: silent until you come back."))
        s_changed = true;
}

void pageCheats(settings::Settings& s) {
    heading("REBEL ALLIANCE CLEARANCE");
    if (toggleRow("Invincibility", s.invincible, "Your ship cannot be destroyed.")) s_changed = true;
    if (toggleRow("Infinite lives", s.infiniteLives, "Losing a ship costs no life.")) s_changed = true;
    if (toggleRow("Infinite bombs", s.infiniteAmmo, "Torpedoes, bombs and missiles never run out.")) s_changed = true;
    if (toggleRow("Perfect mission stats", s.perfectStats,
                  "The mission results say 0:01, every enemy destroyed, 100% accuracy, no friendlies or lives lost and 100% "
                  "targeting computer efficiency; the medals follow."))
        s_changed = true;
}

// --- mods: texture packs, codes, importing from Dolphin -------------------------------------
mods::DolphinScan s_dolphin;       // what Dolphin has for this game (scanned when the page shows)
bool s_dolphinScanned = false;
bool s_confirmSave = false;        // the save import asks for a second press
std::string s_modsStatus;          // the last import's outcome

std::string gigabytes(uint64_t b) {
    char t[32];
    // (binary units, as Explorer shows them)
    snprintf(t, sizeof t, b >= (1ull << 30) ? "%.1f GB" : "%.0f MB", b >= (1ull << 30) ? double(b) / double(1ull << 30) : double(b) / double(1ull << 20));
    return t;
}

void openFolder(const std::string& dir) {
    std::string url = "file:///";
    for (char c : dir) {
        if (c == '\\') url += '/';
        else if (c == ' ') url += "%20";
        else url += c;
    }
    SDL_OpenURL(url.c_str());
}

// a texture pack copy finished (the menu may be closed by then): load what it brought
void modsPoll() {
    mods::CopyProgress& copy = mods::packCopy();
    if (!copy.done.exchange(false)) return;
    s_modsStatus = copy.message;
    if (!copy.failed) mods::reloadTextures();
    s_dolphinScanned = false;  // (now up to date)
}

void pageMods(settings::Settings& s) {
    if (!s_dolphinScanned) {
        s_dolphin = mods::scanDolphin();
        s_dolphinScanned = true;
    }
    modsPoll();
    const mods::CopyProgress& copy = mods::packCopy();

    heading("TEXTURE PACKS");
    {
        const auto& packs = mods::loadedPacks();
        std::string help;
        if (!s.customTextures) help = "Replacement textures in Dolphin's format, from Red Two's textures folder.";
        else if (packs.empty()) help = "No texture pack for Rogue Leader in Red Two's textures folder yet.";
        else help = std::to_string(mods::loadedTextureCount()) + " replacement textures loaded (Dolphin's format).";
        if (toggleRow("Custom textures", s.customTextures, help.c_str())) {
            s_changed = true;
            mods::setTexturesEnabled(s.customTextures);
        }
    }
    if (buttonRow("Textures folder", "OPEN", "Opens Red Two's textures folder: packs go in a GSWE64 folder inside, as in Dolphin.")) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(std::u8string(mods::texturesDir().begin(), mods::texturesDir().end())), ec);
        openFolder(mods::texturesDir());
    }

    heading("CODES");
    const auto list = mods::codesSnapshot();
    if (list.empty())
        ImGui::TextDisabled("None yet: import Dolphin's below, or add Gecko / Action Replay codes to codes.ini.");
    for (size_t i = 0; i < list.size(); i++) {
        const mods::Code& c = list[i];
        ImGui::PushID(int(i));
        static const char* kinds[] = {"Gecko code", "Action Replay code", "Patch"};
        std::string help = std::string(kinds[int(c.kind)]) + (c.creator.empty() ? "" : " by " + c.creator) +
                           (c.notes.empty() ? "." : ": " + c.notes);
        if (c.supported) {
            bool v = c.enabled;
            if (toggleRow(c.name.c_str(), v, help.c_str())) mods::setCodeEnabled(i, v);
        } else {
            buttonRow(c.name.c_str(), "NOT SUPPORTED", c.why.c_str(), kDim);
        }
        ImGui::PopID();
    }

    heading("IMPORT FROM DOLPHIN");
    if (!s_dolphin.found) {
        ImGui::TextDisabled("Dolphin was not found on this PC.");
        return;
    }
    {  // save
        const bool have = !s_dolphin.saves.empty();
        std::string help = "No Rogue Leader save in Dolphin's memory cards.";
        if (have) {
            const mods::DolphinSave& sv = s_dolphin.saves.front();
            help = "Saved " + sv.when + " in " + sv.where + ". Replaces Red Two's save (old card kept in saves\\backup), restarts.";
        }
        const int row = s_row;
        const bool pressed = buttonRow("Save data", !have ? "NONE FOUND" : s_confirmSave ? "PRESS AGAIN: REPLACE" : "IMPORT",
                                       help.c_str(), have ? (s_confirmSave ? kAmber : kText) : kDim);
        if (s_rowSel[s_section] != row) s_confirmSave = false;
        if (pressed && have) {
            if (!s_confirmSave) {
                s_confirmSave = true;
            } else {
                s_confirmSave = false;
                std::string msg;
                if (mods::importSave(s_dolphin.saves.front(), msg)) {
                    restartGame();  // (returns only if it could not start the new instance)
                    msg += " Restart Red Two to play with it.";
                }
                s_modsStatus = msg;
            }
        }
    }
    {  // texture packs
        const bool have = !s_dolphin.packs.empty();
        size_t files = 0;
        uint64_t bytes = 0;
        for (const auto& p : s_dolphin.packs) files += p.files, bytes += p.bytes;
        bool imported = have;
        for (const auto& p : s_dolphin.packs) imported &= p.imported;
        std::string caption = !have ? "NONE FOUND" : imported ? "UP TO DATE" : "IMPORT";
        if (copy.running) {
            const uint64_t t = copy.bytesTotal, d = copy.bytesDone;
            caption = "COPYING " + std::to_string(t ? int(d * 100 / t) : 0) + "%";
        }
        std::string help = "No Rogue Leader texture pack in Dolphin's Load\\Textures folder.";
        if (have)
            help = std::to_string(files) + " files (" + gigabytes(bytes) + ")" + (imported ? ", already copied from " : ", copied from ") +
                   s_dolphin.packs.front().path;
        if (buttonRow("Texture pack", caption.c_str(), help.c_str(), have ? (copy.running ? kAmber : kText) : kDim) && have &&
            !copy.running) {
            s_modsStatus.clear();
            mods::startPackImport(s_dolphin.packs);
        }
    }
    {  // codes
        const bool have = !s_dolphin.codes.empty();
        size_t on = 0;
        for (const auto& c : s_dolphin.codes) on += c.enabled;
        const size_t n = s_dolphin.codes.size();
        const std::string help = have ? std::to_string(n) + (n == 1 ? " code" : " codes") + " for this game in Dolphin (" +
                                            std::to_string(on) + " enabled there), kept on or off as they are there."
                                      : "Dolphin has no codes for Rogue Leader.";
        if (buttonRow("Codes", have ? "IMPORT" : "NONE FOUND", help.c_str(), have ? kText : kDim) && have) {
            std::string msg;
            mods::importCodes(s_dolphin, msg);
            s_modsStatus = msg;
        }
    }
    if (!s_modsStatus.empty()) {
        ImGui::Dummy(ImVec2(0, 4.f * s_ui));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kHolo));
        ImGui::TextWrapped("%s", s_modsStatus.c_str());
        ImGui::PopStyleColor();
    }
}

void pageControls() {
    const auto rows = controlRows();
    finishCapture(rows);
    if (s_pendingCapture >= 0 && !acceptHeld()) {
        startCapture(s_pendingCapture);
        s_pendingCapture = -1;
    }
    if (s_pendingHotkey >= 0 && !acceptHeld()) {
        hotkeys::captureStart(s_pendingHotkey, s_padDevice);
        s_pendingHotkey = -1;
    } else if (hotkeys::capturing() &&
               (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        hotkeys::captureCancel();  // a click elsewhere
    }
    hotkeys::captureTick();
    if (s_hotkeyWasCapturing && !hotkeys::capturing()) s_navBackTb = tbNow() + kTbClock / 4;  // (its release is no menu press)
    s_hotkeyWasCapturing = hotkeys::capturing();
    heading("ON-SCREEN BUTTONS");
    {
        settings::Settings& st = settings::get();
        const bool sel = rowBegin("Button prompts", "The buttons the game shows in its prompts: as on an Xbox controller "
                                                    "(the button each GameCube button is bound to), or the original GameCube ones.");
        const int cur = st.xboxButtons ? 0 : 1;
        const int next = choiceStep(cur, 2, selector("prompts", cur == 0 ? "XBOX" : "GAMECUBE", cur > 0, cur < 1, sel), sel);
        if (next != cur) {
            st.xboxButtons = next == 0;
            s_changed = true;
            buttonIconsUpdate();
        }
    }
    heading("SAVE STATES");
    {
        settings::Settings& st = settings::get();
        const bool sel = rowBegin("State slot", "The slot the save state hotkeys use (the next / previous slot hotkeys "
                                                "change it too). States are kept in your Red Two folder, states.");
        const std::string info = state::slotInfo(st.stateSlot);
        char buf[64];
        snprintf(buf, sizeof buf, "SLOT %d   %s", st.stateSlot, info.empty() ? "EMPTY" : info.c_str());
        const int cur = st.stateSlot - 1;
        const int next = choiceStep(cur, state::kSlots, selector("slot", buf, cur > 0, cur < state::kSlots - 1, sel), sel);
        if (next != cur) {
            st.stateSlot = next + 1;
            s_changed = true;
        }
    }
    if (buttonRow("Save state now", "SAVE", "Saves the game as it is right now into the slot."))
        state::request(state::Op::Save, settings::get().stateSlot);
    if (buttonRow("Load state now", "LOAD", "Puts the game back the way it was saved in the slot."))
        state::request(state::Op::Load, settings::get().stateSlot);
    {  // both devices' hotkeys at a glance (the HOTKEYS rows below show and change one device's)
        const std::string padSave = hotkeys::label(settings::HkSaveState, true),
                          padLoad = hotkeys::label(settings::HkLoadState, true),
                          keySave = hotkeys::label(settings::HkSaveState, false),
                          keyLoad = hotkeys::label(settings::HkLoadState, false);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kHolo));
        ImGui::Text("   Controller: %s saves, %s loads        Keyboard: %s saves, %s loads", padSave.c_str(), padLoad.c_str(),
                    keySave.c_str(), keyLoad.c_str());
        ImGui::PopStyleColor();
    }
    heading("INPUT DEVICE");
    {
        const bool sel = rowBegin("Remap", "Which device's bindings this page shows and changes.");
        const int step = selector("device", s_padDevice ? "CONTROLLER" : "KEYBOARD", s_padDevice, !s_padDevice, sel);
        if (step || (sel && (s_in.dx || s_in.accept)) ) {
            const bool want = step ? step > 0 : (s_in.dx ? s_in.dx > 0 : !s_padDevice);
            s_padDevice = want;
        }
    }
    const bool noPad = s_padDevice && !aurora::pad::port_controller(0);
    if (buttonRow("Reset to defaults", "RESET", "Puts back the original bindings and hotkeys of the device shown.") &&
        !noPad) {
        aurora::pad::reset_to_defaults(profileTarget());
        aurora::pad::save_profiles();
        hotkeys::resetDefaults(s_padDevice);
    }
    if (noPad) {
        ImGui::TextDisabled("No controller connected.");
        return;
    }
    if (s_padDevice) {
        SDL_Gamepad* pad = targetGamepad();
        const char* name = pad ? SDL_GetGamepadName(pad) : nullptr;
        ImGui::TextDisabled("%s", name ? name : "Controller");
    }
    heading("HOTKEYS");
    static const char* const kHotkeyNames[settings::HkCount] = {"Save state", "Load state", "Next slot", "Previous slot"};
    for (int a = 0; a < settings::HkCount; a++) {
        ImGui::PushID(100 + a);
        const bool capturing = hotkeys::captureAction() == a || s_pendingHotkey == a;
        const std::string text = capturing ? std::string("HOLD, THEN LET GO...")
                                           : hotkeys::label(a, s_padDevice);
        const bool idle = !rebindingNow();
        const bool pressed = buttonRow(kHotkeyNames[a], text.c_str(),
                                       s_padDevice ? "Press A (or click), then hold the buttons together - Back + RB - and let "
                                                     "go. X or a right-click clears it."
                                                   : "Press Enter (or click), then hold the keys together and let go. Delete "
                                                     "or a right-click clears it. Esc cancels.",
                                       capturing ? kGold : kText);
        const bool sel = s_rowSel[s_section] == s_row - 1;
        if (pressed && idle) s_pendingHotkey = a;  // listens once the click / A / Enter is released
        if (idle && (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (sel && s_in.clear))) hotkeys::clear(a, s_padDevice);
        ImGui::PopID();
    }
    heading("GAMECUBE CONTROLLER");
    for (int i = 0; i < int(rows.size()); i++) {
        const ControlRow& row = rows[size_t(i)];
        ImGui::PushID(i);
        const bool capturing = s_captureRow == i || s_pendingCapture == i;
        const std::string text = capturing ? std::string("PRESS A KEY OR BUTTON...") : bindingText(row);
        const bool idle = s_captureRow < 0 && s_pendingCapture < 0 && s_pendingHotkey < 0 && !hotkeys::capturing();
        const bool pressed = buttonRow(row.name, text.c_str(),
                                       "Press A / Enter (or click), then the new key or button. X / Delete or a right-click "
                                       "clears it. Back / Esc cancels.",
                                       capturing ? kGold : kText);
        const bool sel = s_rowSel[s_section] == s_row - 1;
        if (pressed && idle) s_pendingCapture = i;  // listens once the click / A / Enter is released
        if (idle && (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (sel && s_in.clear))) clearRow(row);
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------------------
// the window
// ---------------------------------------------------------------------------------------
void switchSection(int i) {
    if (i == s_section) return;
    s_section = i;
    s_scrollToSel = true;
    s_pendingCapture = -1;
    s_pendingHotkey = -1;
    hotkeys::captureCancel();
}

// the controller's Back: down now, and whether another button went down meanwhile (menuEvent)
bool s_backDown = false, s_backChord = false;
// a short message at the top of the screen (save states)
std::string s_toast;
u64 s_toastTb = 0;

// a small key cap: the button that does something here
void keyCap(ImDrawList* dl, ImVec2 c, const char* text, bool lit) {
    const float px = 17.f * s_ui;
    const float tw = textWidth(s_font, px, text, 1.f * s_ui);
    const ImVec2 half(std::max(tw * 0.5f + 10.f * s_ui, 20.f * s_ui), 15.f * s_ui);
    const ImVec2 a(c.x - half.x, c.y - half.y), b(c.x + half.x, c.y + half.y);
    dl->AddRectFilled(a, b, lit ? alpha(kAmber, 200) : IM_COL32(10, 22, 44, 220), 6.f * s_ui);
    dl->AddRect(a, b, lit ? kGold : alpha(kGold, 140), 6.f * s_ui, 0, 1.5f * s_ui);
    drawText(dl, s_font, px, ImVec2(c.x - tw * 0.5f, c.y - px * 0.55f), lit ? IM_COL32(20, 12, 0, 255) : kGold, text, 1.f * s_ui);
}

// the tabs: across the top, LB / RB (Q / E) caps at the ends
void tabBar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float h = 54.f * s_ui, capW = 64.f * s_ui, gap = 10.f * s_ui;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tabsX = p.x + capW + gap, tabsW = avail - 2.f * (capW + gap);
    const float tw = (tabsW - gap * float(SectionCount - 1)) / float(SectionCount);
    for (int i = 0; i < SectionCount; i++) {
        const ImVec2 a(tabsX + float(i) * (tw + gap), p.y), b(a.x + tw, p.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, h)) && s_captureRow < 0) switchSection(i);
        const bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = s_section == i;
        panel(dl, a, b, 12.f * s_ui, on ? IM_COL32(74, 48, 10, 235) : IM_COL32(10, 22, 44, hot ? 230 : 170),
              on ? IM_COL32(30, 18, 4, 235) : IM_COL32(4, 10, 22, 200),
              on ? kAmber : (hot ? alpha(kHolo, 200) : alpha(kGold, 60)), 1.f * s_ui);
        if (on) dl->AddRectFilled(ImVec2(a.x + 14.f * s_ui, b.y - 4.f * s_ui), ImVec2(b.x - 14.f * s_ui, b.y), kAmber);
        const float px = 22.f * s_ui, tr = 4.f * s_ui;
        const float w = textWidth(s_font, px, kSectionNames[i], tr);
        drawText(dl, s_font, px, ImVec2(a.x + (tw - w) * 0.5f, a.y + h * 0.5f - px * 0.55f),
                 on ? kGold : (hot ? kText : kDim), kSectionNames[i], tr, on ? alpha(kAmber, 40) : 0);
    }
    const float cy = p.y + h * 0.5f;
    keyCap(dl, ImVec2(p.x + capW * 0.5f, cy), s_usingPad ? "LB" : "Q",
           s_navKey[NavPrevTab].held || ImGui::IsKeyDown(ImGuiKey_Q));
    keyCap(dl, ImVec2(p.x + avail - capW * 0.5f, cy), s_usingPad ? "RB" : "E",
           s_navKey[NavNextTab].held || ImGui::IsKeyDown(ImGuiKey_E));
    const float ruleY = p.y + h + 12.f * s_ui;
    dl->AddLine(ImVec2(p.x, ruleY), ImVec2(p.x + avail, ruleY), alpha(kGold, 60), 1.f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, ruleY + 14.f * s_ui));
}

// returns false when the menu closed
bool drawMenu(settings::Settings& s, float t) {
    // input first: B closes, LB / RB switch tabs, up / down move the selection
    const bool rebinding = rebindingNow();
    pollNav(!rebinding);
    if (s_nav[NavBack]) return false;
    if (s_nav[NavPrevTab]) switchSection((s_section + SectionCount - 1) % SectionCount);
    if (s_nav[NavNextTab]) switchSection((s_section + 1) % SectionCount);
    {
        const int count = s_rowCount[s_section];
        int& sel = s_rowSel[s_section];
        if (count > 0 && (s_nav[NavUp] || s_nav[NavDown])) {
            sel = (sel + (s_nav[NavUp] ? count - 1 : 1)) % count;
            s_scrollToSel = true;
        }
    }
    s_in = {};
    s_in.dx = (s_nav[NavRight] ? 1 : 0) - (s_nav[NavLeft] ? 1 : 0);
    s_in.accept = s_nav[NavAccept];
    s_in.clear = s_nav[NavClear];

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 disp = io.DisplaySize;
    s_ui = std::clamp(disp.y / 1080.f, 0.6f, 3.f);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    // the game behind: dimmed, stars over it
    bg->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 3, 10, 205));
    starfield(bg, disp, t);

    const float w = std::min(disp.x * 0.86f, disp.y * 1.45f), h = disp.y * 0.86f;
    const ImVec2 a((disp.x - w) * 0.5f, (disp.y - h) * 0.5f), b(a.x + w, a.y + h);
    panel(bg, a, b, 34.f * s_ui, kPanelTop, kPanelBottom, alpha(kGold, 200), 1.5f * s_ui);
    brackets(bg, a, b, 46.f * s_ui, kAmber, 2.f * s_ui);
    for (float y = a.y + 2.f; y < b.y; y += 4.f * s_ui)
        bg->AddLine(ImVec2(a.x + 2.f, y), ImVec2(b.x - 2.f, y), IM_COL32(120, 190, 255, 9), 1.f);
    // a slow scan line
    const float scanY = a.y + std::fmod(t * 0.12f, 1.f) * h;
    bg->AddRectFilledMultiColor(ImVec2(a.x, scanY - 18.f * s_ui), ImVec2(b.x, scanY), IM_COL32(110, 205, 255, 0),
                                IM_COL32(110, 205, 255, 0), IM_COL32(110, 205, 255, 22), IM_COL32(110, 205, 255, 22));

    // header
    const float pad = 44.f * s_ui;
    const float titlePx = 50.f * s_ui;
    const float titleY = a.y + pad * 0.55f;
    drawText(bg, s_fontBig, titlePx, ImVec2(a.x + pad, titleY), kGold, "RED TWO", 10.f * s_ui, alpha(kAmber, 50));
    drawText(bg, s_font, 18.f * s_ui, ImVec2(a.x + pad + 2.f, titleY + titlePx * 1.02f), kHolo,
             "ROGUE SQUADRON II  //  FLIGHT SYSTEMS CONFIGURATION", 3.f * s_ui);
    const float blink = 0.5f + 0.5f * std::sin(t * 3.f);
    const ImVec2 lamp(b.x - pad - 8.f * s_ui, titleY + titlePx * 0.45f);
    bg->AddCircleFilled(lamp, 6.f * s_ui, alpha(kAmber, int(120 + 135 * blink)));
    bg->AddCircle(lamp, 11.f * s_ui, alpha(kAmber, 90), 24, 1.f);
    const char* status = "SYSTEMS ONLINE";
    const float statusPx = 17.f * s_ui;
    drawText(bg, s_font, statusPx, ImVec2(lamp.x - 22.f * s_ui - textWidth(s_font, statusPx, status, 2.f * s_ui), lamp.y - 9.f * s_ui),
             kDim, status, 2.f * s_ui);
    const float headerBottom = titleY + titlePx * 1.02f + 30.f * s_ui;

    // footer: the selected row's description, then the buttons
    const float footY = b.y - pad * 0.8f;
    const float helpY = footY - 40.f * s_ui;
    bg->AddLine(ImVec2(a.x + pad, helpY - 12.f * s_ui), ImVec2(b.x - pad, helpY - 12.f * s_ui), alpha(kGold, 50), 1.f);
    std::string hint = s_usingPad ? "LB / RB  TABS      D-PAD / STICK  MOVE, CHANGE      A  SELECT"
                                  : "Q / E  TABS      ARROWS  MOVE, CHANGE      ENTER  SELECT";
    if (s_section == Controls) hint += s_usingPad ? "      X  CLEAR" : "      DEL  CLEAR";
    hint += s_usingPad ? "      B / BACK  CLOSE" : "      ESC / F1  CLOSE";
    drawText(bg, s_font, 16.f * s_ui, ImVec2(a.x + pad, footY), kDim, hint.c_str(), 2.f * s_ui);

    // the interactive part: an invisible window over the panel body
    const float top = headerBottom;
    ImGui::SetNextWindowPos(ImVec2(a.x + pad, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w - 2.f * pad, helpY - 24.f * s_ui - top), ImGuiCond_Always);
    if (s_focus) ImGui::SetNextWindowFocus();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoScrollWithMouse;
    s_help = nullptr;
    if (ImGui::Begin("##redtwo_menu", nullptr, flags)) {
        // (a child's font scale multiplies its parent's: this window keeps 1, the page sets it)
        if (s_font) ImGui::PushFont(s_font);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f * s_ui, 7.f * s_ui));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f * s_ui, 10.f * s_ui));

        tabBar();

        ImGui::BeginChild("##page", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
        bodyFont(23.f * s_ui);
        s_valueX = std::min(380.f * s_ui, ImGui::GetContentRegionAvail().x * 0.42f);
        s_changed = s_display = false;
        s_row = 0;
        switch (s_section) {
        case Display: pageDisplay(s); break;
        case Gameplay: pageGameplay(s); break;
        case Audio: pageAudio(s); break;
        case Controls: pageControls(); break;
        case Cheats: pageCheats(s); break;
        case Mods: pageMods(s); break;
        }
        ImGui::Dummy(ImVec2(0, 8.f * s_ui));
        s_rowCount[s_section] = s_row;
        if (s_row > 0) s_rowSel[s_section] = std::clamp(s_rowSel[s_section], 0, s_row - 1);
        ImGui::EndChild();

        ImGui::PopStyleVar(2);
        if (s_font) ImGui::PopFont();
        if (s_changed) settings::save();
        if (s_display) applyDisplaySettings();
    }
    ImGui::End();
    if (s_help) drawText(bg, s_font, 19.f * s_ui, ImVec2(a.x + pad, helpY), alpha(kHolo, 220), s_help, 0.5f * s_ui);
    s_focus = false;
    if (s_captureRow < 0 && s_navBackTb && tbNow() >= s_navBackTb) s_navBackTb = 0;
    return true;
}

// R2_MENU_BIND_TEST=1 (with R2_MENU=1 R2_MENU_SECTION=3, automated runs only: their bindings
// live in build/test_cache): rebinds A and STICK UP on the keyboard through synthetic key
// presses, logs the results, then resets the keyboard profile.
void pushKey(SDL_Scancode sc) {
    for (int down = 1; down >= 0; down--) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.timestamp = SDL_GetTicksNS();
        e.key.windowID = aurora_get_window() ? SDL_GetWindowID(aurora_get_window()) : 0;
        e.key.scancode = sc;
        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.down = down != 0;
        SDL_PushEvent(&e);
    }
}
void bindSelfTest() {
    static const bool on = envFlag("R2_MENU_BIND_TEST") && getenv("R2_INPUT");
    if (!on) return;
    static int k = 0;
    const auto rows = controlRows();
    auto show = [&](const char* when) {
        logf("menu", "bind test %s: A = %s, STICK UP = %s, STICK DOWN = %s", when, bindingText(rows[0]).c_str(),
             bindingText(rows[12]).c_str(), bindingText(rows[13]).c_str());
    };
    switch (++k) {
    case 30: s_padDevice = false; show("before"); startCapture(0); break;
    case 40: pushKey(SDL_SCANCODE_K); break;
    case 60: show("after A=K"); startCapture(12); break;
    case 70: pushKey(SDL_SCANCODE_I); break;
    case 90: show("after STICK UP=I"); startCapture(1); break;
    case 100: pushKey(SDL_SCANCODE_ESCAPE); break;
    case 120: show("after Esc (B unchanged)"); logf("menu", "bind test: B = %s, menu open %d", bindingText(rows[1]).c_str(), s_open); break;
    case 130:
        aurora::pad::reset_to_defaults(profileTarget());
        aurora::pad::save_profiles();
        show("after reset");
        break;
    default: break;
    }
}

// R2_MENU_PAD_TEST=1 (with R2_MENU=1, automated runs): an SDL virtual gamepad - the same path
// as a real controller - switches tabs, moves, changes the volume, toggles, then closes the
// menu with B; the log shows where each step got to.
void padSelfTest() {
    if (!padTest()) return;
    static SDL_Joystick* joy = nullptr;
    static int k = 0;
    if (!joy) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Red Two test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joy = id ? SDL_OpenJoystick(id) : nullptr;
        logf("menu", "pad test: virtual gamepad %u %s", unsigned(id), joy ? "attached" : SDL_GetError());
        if (!joy) return;
    }
    const settings::Settings& s = settings::get();
    auto show = [&](const char* step) {
        logf("menu", "pad test %-22s: open %d tab %s row %d volume %d background %d", step, s_open, kSectionNames[s_section],
             s_rowSel[s_section], s.volume, s.backgroundAudio);
    };
    auto press = [&](int at, SDL_GamepadButton b) {
        if (k == at) SDL_SetJoystickVirtualButton(joy, b, true);
        if (k == at + 4) SDL_SetJoystickVirtualButton(joy, b, false);
    };
    auto stick = [&](int at, SDL_GamepadAxis a, Sint16 v) {
        if (k == at) SDL_SetJoystickVirtualAxis(joy, a, v);
        if (k == at + 4) SDL_SetJoystickVirtualAxis(joy, a, 0);
    };
    k++;
    press(40, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // Display -> Gameplay
    press(60, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // -> Audio
    press(80, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);    // -> Gameplay
    press(100, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);  // -> Audio
    stick(120, SDL_GAMEPAD_AXIS_LEFTY, 32767);      // stick down: row 1
    press(140, SDL_GAMEPAD_BUTTON_DPAD_UP);         // row 0, master volume
    press(160, SDL_GAMEPAD_BUTTON_DPAD_LEFT);       // 95
    stick(180, SDL_GAMEPAD_AXIS_LEFTX, -32768);     // 90
    press(200, SDL_GAMEPAD_BUTTON_DPAD_DOWN);       // row 1
    press(220, SDL_GAMEPAD_BUTTON_SOUTH);           // A: sound in background off
    press(260, SDL_GAMEPAD_BUTTON_EAST);            // B: close
    switch (k) {
    case 30: show("start"); break;
    case 55: show("RB"); break;
    case 75: show("RB"); break;
    case 95: show("LB"); break;
    case 115: show("RB"); break;
    case 135: show("stick down"); break;
    case 155: show("d-pad up"); break;
    case 175: show("d-pad left"); break;
    case 195: show("stick left"); break;
    case 215: show("d-pad down"); break;
    case 235: show("A"); break;
    case 275: show("B"); break;
    default: break;
    }
}

void setOpen(bool open) {
    if (!open) cancelCapture();
    if (open) s_dolphinScanned = false;  // Dolphin's files may have changed meanwhile
    s_confirmSave = false;
    s_open = open;
    s_focus = open;
    s_navFresh = true;
    s_scrollToSel = open;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// interface
// ---------------------------------------------------------------------------------------
bool menuOpen() { return s_open; }

void setHudAnchoring(bool on) {
    static bool last = false;
    if (on != last) AuroraSetHudAnchoring(last = on);
}

// Perspective aspects the game's cameras were widened to (hle_CameraSetup): Aurora shows those
// projections as they are. The latest 8; Aurora hears of each new one once.
void noteWidenedAspect(float aspect) {
    static float list[8] = {};
    static u32 count = 0, next = 0;
    for (u32 i = 0; i < count; i++)
        if (std::abs(list[i] - aspect) < 0.0005f) return;
    list[next] = aspect;
    next = (next + 1) % 8;
    count = std::min<u32>(count + 1, 8);
    AuroraSetWidenedAspects(list, count);
    logf("wide", "widened camera aspect %.4f", aspect);
}

void menuImGuiInit(const void*) {
    ImGuiIO& io = ImGui::GetIO();
    // the menu does its own navigation (above); ImGui's would move a second cursor
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    const char* path = "C:\\Windows\\Fonts\\bahnschrift.ttf";
    if (FILE* f = fopen(path, "rb")) {
        fclose(f);
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        s_font = io.Fonts->AddFontFromFileTTF(path, kFontPx, &cfg);
        s_fontBig = io.Fonts->AddFontFromFileTTF(path, kBigPx, &cfg);
    }
    if (!s_font) s_font = s_fontBig = io.Fonts->AddFontDefault();
    applyStyle();
}

void menuEvent(const void* p) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    if (rebindingNow()) return;  // rebinding takes these
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.key == SDLK_F1) {
            logf("menu", "F1: %s", s_open ? "close" : "open");
            s_usingPad = false;
            if (!s_open) s_padDevice = false;  // Controls shows the keyboard's
            setOpen(!s_open);
        } else if (e.key.key == SDLK_ESCAPE && s_open) {
            logf("menu", "Esc: close");
            setOpen(false);
        }
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        // Back toggles the menu when it is let go with nothing else pressed meanwhile: Back + RB
        // and the other hotkeys hold it too
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            s_backDown = true;
            s_backChord = false;
        } else if (s_backDown) {
            s_backChord = true;
        }
    } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && s_backDown && e.gaxis.value > 16000 &&
               (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        s_backChord = true;
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
        const bool toggle = s_backDown && !s_backChord && (!automatedRun() || getenv("R2_HOTKEY_TEST"));
        s_backDown = false;
        if (toggle) {
            logf("menu", "controller Back: %s", s_open ? "close" : "open");
            s_usingPad = true;
            if (!s_open) s_padDevice = aurora::pad::port_controller(0) != 0;  // Controls shows the controller's
            setOpen(!s_open);
        }
    }
}

void toast(const std::string& text) {
    s_toast = text;
    s_toastTb = tbNow();
}

// The aspect Aurora renders at: the setting's, or the window's (-1) up to settings::kMaxAspect -
// a wider window gets a fixed 21:9 frame with bars at the sides.
static float s_appliedAspect = 0.f;

static float displayAspect() {
    const float a = settings::aspectValue(settings::get().aspect);
    if (a > 0.f) return std::min(a, settings::kMaxAspect);
    int w = 0, h = 0;
    if (SDL_Window* win = aurora_get_window()) SDL_GetWindowSizeInPixels(win, &w, &h);
    return w > 0 && h > 0 && float(w) / float(h) > settings::kMaxAspect ? settings::kMaxAspect : -1.f;
}

static void applyDisplayAspect() {
    const float a = displayAspect();
    if (a == s_appliedAspect) return;  // (AuroraSetDisplayAspect drains the GX worker)
    s_appliedAspect = a;
    AuroraSetDisplayAspect(a);
}

void applyDisplaySettings() {
    const settings::Settings& s = settings::get();
    VISetFrameBufferScale(settings::resolutionScale(s.resolution));
    applyDisplayAspect();
    // ubershaders (Aurora patch 0021): R2_UBER=0/1/2 (off, hybrid, every draw: testing) overrides
    static const char* uberEnv = getenv("R2_UBER");
    AuroraSetUberShaders(uberEnv ? atoi(uberEnv) : s.ubershaders ? 1 : 0);
    // widescreen: letterboxed cinematics fill the screen (4:3 keeps the original bars)
    AuroraSetLetterboxFill(s.aspect != settings::Aspect::R4_3);
    if (SDL_Window* w = aurora_get_window()) {
        const bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
        if (full != s.fullscreen) SDL_SetWindowFullscreen(w, s.fullscreen);
    }
    viSetRate(s.fps);
}

// Before the game starts, every pipeline in the cache is built (gx_bridge.cpp precompileShaders);
// frames of its own show how far it got.
void drawShaderProgress(u32 done, u32 total) {
    if (!GImGui || !GImGui->WithinFrameScope) return;  // (no UI frame before the first XFB exists)
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float ui = std::clamp(disp.y / 1080.f, 0.6f, 3.f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 255));
    const float cx = disp.x * 0.5f, cy = disp.y * 0.5f;
    const char* title = "PREPARING SHADERS";
    const float tpx = 38.f * ui, track = 5.f * ui;
    drawText(dl, s_fontBig, tpx, ImVec2(cx - textWidth(s_fontBig, tpx, title, track) * 0.5f, cy - 80.f * ui), kGold, title,
             track);
    const float w = std::min(disp.x * 0.5f, 720.f * ui), h = 8.f * ui;
    const ImVec2 a(cx - w * 0.5f, cy - h * 0.5f), b(cx + w * 0.5f, cy + h * 0.5f);
    dl->AddRectFilled(a, b, IM_COL32(30, 52, 86, 255), h * 0.5f);
    const float f = total ? std::clamp(float(done) / float(total), 0.f, 1.f) : 1.f;
    if (f > 0.f) dl->AddRectFilled(a, ImVec2(a.x + w * f, b.y), kAmber, h * 0.5f);
    char line[64];
    snprintf(line, sizeof line, "%u / %u", done, total);
    const float px = 24.f * ui;
    drawText(dl, s_font, px, ImVec2(cx - textWidth(s_font, px, line) * 0.5f, b.y + 18.f * ui), kText, line);
    const char* note = "Every shader the game has used on this PC is compiled before it starts.";
    const char* note2 = "This takes long only after a graphics driver update.";
    const float npx = 19.f * ui;
    drawText(dl, s_font, npx, ImVec2(cx - textWidth(s_font, npx, note) * 0.5f, b.y + 70.f * ui), kDim, note);
    drawText(dl, s_font, npx, ImVec2(cx - textWidth(s_font, npx, note2) * 0.5f, b.y + 96.f * ui), kDim, note2);
}

void menuDraw() {
    // widescreen: the game's cameras take the aspect the frame is rendered at (so they also cull
    // for it); 4:3 restores the original
    applyDisplayAspect();  // match window: a resize may cross the 21:9 limit
    {
        u32 w = 0, h = 0;
        AuroraGetRenderSize(&w, &h);
        if (s_appliedAspect > 0.f) setGameAspect(s_appliedAspect);
        else if (w && h) setGameAspect(std::min(double(w) / double(h), double(settings::kMaxAspect)));
    }
    settings::Settings& s = settings::get();
    modsPoll();
    // button prompts follow the bindings (a controller connected, a rebind): checked twice a second
    {
        static u64 lastIconsTb = 0;
        const u64 now = tbNow();
        if (now - lastIconsTb >= kTbClock / 2) {
            // (reads controller names from SDL: not while the input thread updates the controllers)
            if (input::JoystickAccess joy; joy) {
                lastIconsTb = now;
                buttonIconsUpdate();
            }
        }
    }
    // volume: (volume / 100)^2 roughly follows loudness; silent in the background if asked
    {
        SDL_Window* win = aurora_get_window();
        const bool focused = !win || (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const float v = float(std::clamp(s.volume, 0, 100)) / 100.f;
        audio::setGain(!s.backgroundAudio && !focused ? 0.f : v * v);
    }
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) return;
    static bool testOpen = envFlag("R2_MENU");  // screenshots of the menu in automated runs
    if (testOpen) {
        testOpen = false;
        setOpen(true);
        if (const char* sec = getenv("R2_MENU_SECTION")) s_section = std::clamp(atoi(sec), 0, int(SectionCount) - 1);
        if (const char* row = getenv("R2_MENU_ROW")) s_rowSel[s_section] = std::max(0, atoi(row));
        s_usingPad = envFlag("R2_MENU_PAD");
        s_padDevice = envFlag("R2_MENU_PAD");  // (the Controls page shows the controller)
    }

    // presented frames per second (half-second window)
    s_fpsFrames++;
    const u64 now = tbNow();
    if (now - s_fpsTb >= kTbClock / 2) {
        s_fps = s_fpsTb ? float(double(s_fpsFrames) * double(kTbClock) / double(now - s_fpsTb)) : 0.f;
        s_fpsFrames = 0;
        s_fpsTb = now;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float ui = std::clamp(io.DisplaySize.y / 1080.f, 0.6f, 3.f);
    const float t = float(double(SDL_GetTicks()) / 1000.0);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (s.showFps && !s_open) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.0f FPS", s_fps);
        const float px = 20.f * ui;
        const ImVec2 at(14.f * ui, 12.f * ui);
        const float tw = textWidth(s_font, px, buf, 2.f * ui);
        panel(fg, ImVec2(at.x - 8.f * ui, at.y - 5.f * ui), ImVec2(at.x + tw + 10.f * ui, at.y + px + 5.f * ui), 6.f * ui,
              IM_COL32(6, 14, 30, 170), IM_COL32(2, 6, 14, 190), alpha(kGold, 140), 1.f);
        drawText(fg, s_font, px, at, kGold, buf, 2.f * ui);
    }
    // save state messages: 2.5 s at the top, fading out
    if (!s_toast.empty()) {
        const double age = double(now - s_toastTb) / double(kTbClock);
        if (age < 2.5) {
            const float a = age < 2.0 ? 1.f : float((2.5 - age) / 0.5);
            const float px = 24.f * ui;
            const float tw = textWidth(s_font, px, s_toast.c_str(), 3.f * ui);
            const ImVec2 c(io.DisplaySize.x * 0.5f, 64.f * ui);
            const ImVec2 pa(c.x - tw * 0.5f - 22.f * ui, c.y - px * 0.5f - 12.f * ui),
                pb(c.x + tw * 0.5f + 22.f * ui, c.y + px * 0.5f + 12.f * ui);
            panel(fg, pa, pb, 10.f * ui, alpha(IM_COL32(8, 18, 38, 255), int(225 * a)), alpha(IM_COL32(2, 6, 14, 255), int(235 * a)),
                  alpha(kGold, int(200 * a)), 1.5f * ui);
            drawText(fg, s_font, px, ImVec2(c.x - tw * 0.5f, c.y - px * 0.55f), alpha(kGold, int(255 * a)), s_toast.c_str(),
                     3.f * ui);
        } else {
            s_toast.clear();
        }
    }
    if (cinematicSkip()) {
        const char* text = "SKIPPING";
        const float px = 34.f * ui;
        const float tw = textWidth(s_font, px, text, 8.f * ui);
        const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        const float pulse = 0.6f + 0.4f * std::sin(t * 6.f);
        drawText(fg, s_font, px, ImVec2(c.x - tw * 0.5f, c.y - px * 0.5f), alpha(kGold, int(255 * pulse)), text, 8.f * ui,
                 alpha(kAmber, 40));
        const float bw = tw * 1.2f, y = c.y + px * 0.9f;
        fg->AddRectFilled(ImVec2(c.x - bw * 0.5f, y), ImVec2(c.x + bw * 0.5f, y + 2.f * ui), alpha(kGold, 60));
        const float x = c.x - bw * 0.5f + std::fmod(t * 0.8f, 1.f) * bw * 0.8f;
        fg->AddRectFilled(ImVec2(x, y), ImVec2(x + bw * 0.2f, y + 2.f * ui), kAmber);
    }
    if (s_open && !drawMenu(s, t)) {
        logf("menu", "controller B: close");
        setOpen(false);
    }
    if (s_open) bindSelfTest();
    padSelfTest();
}

}  // namespace rt::gfx
