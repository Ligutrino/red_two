// In-game settings menu: Dear ImGui (through Aurora), drawn over the game's frame. F1 or the
// controller's Back/Select button (the GameCube pad has none, so the game never sees it) opens
// and closes it; Escape closes it. While it is open the game sees idle controllers
// (input.cpp), and the menu is driven by mouse, keyboard or controller (d-pad + A, LB/RB switch
// sections).
//
// The look: a holo-display over a starfield - a chamfered navy panel with glowing gold edges,
// corner brackets and scanlines, Star Wars crawl gold for titles, the radar's amber for
// highlights, Bahnschrift (Windows' DIN-style face) for type.
#include "gx_bridge.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
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
#include <cstring>
#include <string>
#include <vector>

#include "../runtime.h"
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
bool s_focus = false;  // just opened: focus the window and its first item
int s_section = 0;
u64 s_fpsFrames = 0, s_fpsTb = 0;
float s_fps = 0.f;

enum Section { Display, Gameplay, Controls, Cheats, SectionCount };
const char* const kSectionNames[SectionCount] = {"DISPLAY", "GAMEPLAY", "CONTROLS", "CHEATS"};

void setNav(bool on) {
    if (ImGuiContext* ctx = ImGui::GetCurrentContext()) {
        constexpr ImGuiConfigFlags nav = ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
        if (on) ctx->IO.ConfigFlags |= nav;
        else ctx->IO.ConfigFlags &= ~nav;
    }
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
bool s_captured = false, s_captureCancel = false, s_captureRearm = false;
aurora::binding::PhysicalInput s_capturedInput;
u64 s_navBackTb = 0;  // keyboard/controller navigation resumes after a capture (not on its press)

void startCapture(int row) {
    s_captureRow = row;
    s_captured = s_captureCancel = false;
    setNav(false);
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

// ---------------------------------------------------------------------------------------
// widgets
// ---------------------------------------------------------------------------------------
float s_ui = 1.f;      // size of the UI: the display's height / 1080
float s_valueX = 0.f;  // where values start in a row

void bodyFont(float px) { ImGui::SetWindowFontScale(px / kFontPx); }

// a row: label on the left; the caller draws the value after it
void label(const char* text, const char* help = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    if (help && ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", help);
    ImGui::SameLine(s_valueX);
}

void tip(const char* help) {
    if (help && (ImGui::IsItemHovered() || ImGui::IsItemFocused())) ImGui::SetItemTooltip("%s", help);
}

// ON / OFF switch
bool toggle(const char* id, bool& v, const char* help = nullptr) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, v ? vec(kAmber, 0.55f) : ImVec4(0.06f, 0.13f, 0.25f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Text, v ? vec(IM_COL32(255, 245, 220, 255)) : vec(kDim));
    const bool pressed = ImGui::Button(v ? "ON" : "OFF", ImVec2(150.f * s_ui, 0.f));
    ImGui::PopStyleColor(2);
    tip(help);
    ImGui::PopID();
    if (pressed) v = !v;
    return pressed;
}

template <typename E>
bool enumCombo(const char* id, E& value, const char* (*name)(E), const char* help = nullptr) {
    bool changed = false;
    ImGui::SetNextItemWidth(360.f * s_ui);
    if (ImGui::BeginCombo(id, name(value))) {
        for (int i = 0; i < int(E::Count); i++) {
            const bool selected = int(value) == i;
            if (ImGui::Selectable(name(E(i)), selected) && !selected) {
                value = E(i);
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    tip(help);
    return changed;
}

bool fpsCombo(int& fps, const char* help) {
    bool changed = false;
    char cur[16];
    snprintf(cur, sizeof cur, "%d FPS", fps);
    ImGui::SetNextItemWidth(360.f * s_ui);
    if (ImGui::BeginCombo("##fps", cur)) {
        for (int f : settings::kFpsChoices) {
            char item[16];
            snprintf(item, sizeof item, "%d FPS", f);
            const bool selected = fps == f;
            if (ImGui::Selectable(item, selected) && !selected) {
                fps = f;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    tip(help);
    return changed;
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
    label("Fullscreen");
    if (toggle("full", s.fullscreen, "Also F11 or Alt+Enter.")) s_changed = s_display = true;
    label("Internal resolution");
    if (enumCombo("##res", s.resolution, settings::resolutionName,
                  "What the game renders at. Window size follows the window; 4K renders 2160 lines "
                  "whatever the window size."))
        s_changed = s_display = true;
    label("Aspect ratio");
    if (enumCombo("##aspect", s.aspect, settings::aspectName,
                  "Widescreen widens the 3D view and fills the screen in cutscenes."))
        s_changed = s_display = true;
    label("HUD at screen edges");
    if (toggle("hudedges", s.hudEdges, "Widescreen: the flight HUD sits at the edges of the screen.")) s_changed = true;
    heading("PERFORMANCE");
    label("Frame rate");
    if (fpsCombo(s.fps, "Above 60 the game runs more frames per second at its normal speed.")) s_changed = s_display = true;
    label("Show frame rate");
    if (toggle("showfps", s.showFps)) s_changed = true;
}

void pageGameplay(settings::Settings& s) {
    heading("CINEMATICS");
    label("Skip intro logos");
    if (toggle("logos", s.skipLogos, "Reach the title screen in seconds.")) s_changed = true;
    label("Skippable cutscenes");
    if (toggle("cutscenes", s.skipCutscenes, "Press B during the opening crawl or a cutscene to skip to the gameplay."))
        s_changed = true;
}

void pageCheats(settings::Settings& s) {
    heading("REBEL ALLIANCE CLEARANCE");
    label("Invincibility");
    if (toggle("inv", s.invincible, "Your ship cannot be destroyed.")) s_changed = true;
    label("Infinite lives");
    if (toggle("lives", s.infiniteLives, "Losing a ship costs no life.")) s_changed = true;
}

void pageControls() {
    const auto rows = controlRows();
    finishCapture(rows);
    auto deviceButton = [](const char* text, bool pad) {
        const bool on = s_padDevice == pad;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? vec(kAmber, 0.55f) : ImVec4(0.06f, 0.13f, 0.25f, 0.92f));
        if (ImGui::Button(text, ImVec2(220.f * s_ui, 0.f))) s_padDevice = pad;
        ImGui::PopStyleColor();
    };
    deviceButton("KEYBOARD", false);
    ImGui::SameLine();
    deviceButton("CONTROLLER", true);
    ImGui::SameLine();
    if (ImGui::Button("RESET TO DEFAULTS") && (!s_padDevice || aurora::pad::port_controller(0))) {
        aurora::pad::reset_to_defaults(profileTarget());
        aurora::pad::save_profiles();
    }
    if (s_padDevice) {
        SDL_Gamepad* pad = targetGamepad();
        const char* name = pad ? SDL_GetGamepadName(pad) : nullptr;
        if (!aurora::pad::port_controller(0)) {
            ImGui::TextDisabled("No controller connected.");
            return;
        }
        ImGui::TextDisabled("%s", name ? name : "Controller");
    }
    ImGui::TextDisabled("Select a control, then press its new key or button (Esc cancels). Right-click or X clears.");
    heading("GAMECUBE CONTROLLER");
    for (int i = 0; i < int(rows.size()); i++) {
        const ControlRow& row = rows[size_t(i)];
        ImGui::PushID(i);
        label(row.name);
        const bool capturing = s_captureRow == i;
        const std::string text = capturing ? std::string("PRESS A KEY OR BUTTON...") : bindingText(row);
        ImGui::PushStyleColor(ImGuiCol_Text, capturing ? vec(kGold) : vec(kText));
        ImGui::PushStyleColor(ImGuiCol_Button, capturing ? vec(kAmber, 0.35f) : ImVec4(0.06f, 0.13f, 0.25f, 0.92f));
        if (ImGui::Button(text.c_str(), ImVec2(440.f * s_ui, 0.f)) && s_captureRow < 0) startCapture(i);
        ImGui::PopStyleColor(2);
        if (s_captureRow < 0 && (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                                 (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))))
            clearRow(row);
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------------------
// the window
// ---------------------------------------------------------------------------------------
void drawMenu(settings::Settings& s, float t) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 disp = io.DisplaySize;
    s_ui = std::clamp(disp.y / 1080.f, 0.6f, 3.f);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    // the game behind: dimmed, stars over it
    bg->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 3, 10, 205));
    starfield(bg, disp, t);

    const float w = std::min(disp.x * 0.86f, disp.y * 1.45f), h = disp.y * 0.84f;
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
    const float titlePx = 58.f * s_ui;
    const float titleY = a.y + pad * 0.6f;
    drawText(bg, s_fontBig, titlePx, ImVec2(a.x + pad, titleY), kGold, "RED TWO", 10.f * s_ui, alpha(kAmber, 50));
    drawText(bg, s_font, 19.f * s_ui, ImVec2(a.x + pad + 2.f, titleY + titlePx * 1.02f), kHolo,
             "ROGUE SQUADRON II  //  FLIGHT SYSTEMS CONFIGURATION", 3.f * s_ui);
    const float blink = 0.5f + 0.5f * std::sin(t * 3.f);
    const ImVec2 lamp(b.x - pad - 8.f * s_ui, titleY + titlePx * 0.45f);
    bg->AddCircleFilled(lamp, 6.f * s_ui, alpha(kAmber, int(120 + 135 * blink)));
    bg->AddCircle(lamp, 11.f * s_ui, alpha(kAmber, 90), 24, 1.f);
    const char* status = "SYSTEMS ONLINE";
    const float statusPx = 17.f * s_ui;
    drawText(bg, s_font, statusPx, ImVec2(lamp.x - 22.f * s_ui - textWidth(s_font, statusPx, status, 2.f * s_ui), lamp.y - 9.f * s_ui),
             kDim, status, 2.f * s_ui);
    const float headerBottom = titleY + titlePx * 1.02f + 34.f * s_ui;
    bg->AddLine(ImVec2(a.x + pad, headerBottom), ImVec2(b.x - pad, headerBottom), alpha(kGold, 90), 1.f);

    // footer
    const char* hint = "F1 / BACK  CLOSE      LB / RB  SECTION      A  SELECT      B  BACK";
    drawText(bg, s_font, 16.f * s_ui, ImVec2(a.x + pad, b.y - pad * 0.75f), kDim, hint, 2.f * s_ui);

    // the interactive part: an invisible window over the panel body
    const float top = headerBottom + 18.f * s_ui;
    ImGui::SetNextWindowPos(ImVec2(a.x + pad, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w - 2.f * pad, b.y - pad * 1.2f - top), ImGuiCond_Always);
    if (s_focus) ImGui::SetNextWindowFocus();
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##redtwo_menu", nullptr, flags)) {
        if (s_font) ImGui::PushFont(s_font);
        bodyFont(24.f * s_ui);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f * s_ui, 7.f * s_ui));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f * s_ui, 10.f * s_ui));

        // section switching: LB / RB (or Q / E)
        if (s_captureRow < 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
                s_section = (s_section + SectionCount - 1) % SectionCount;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))
                s_section = (s_section + 1) % SectionCount;
        }

        // left: sections
        const float navW = 250.f * s_ui;
        ImGui::BeginChild("##nav", ImVec2(navW, 0.f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        bodyFont(24.f * s_ui);
        for (int i = 0; i < SectionCount; i++) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 sz(navW - 12.f * s_ui, 54.f * s_ui);
            const bool on = s_section == i;
            if (s_focus && on) ImGui::SetKeyboardFocusHere();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##sec", sz)) s_section = i;
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImGui::PopID();
            panel(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), 12.f * s_ui, on ? IM_COL32(60, 40, 10, 220) : IM_COL32(10, 22, 44, hot ? 230 : 170),
                  on ? IM_COL32(30, 18, 4, 220) : IM_COL32(4, 10, 22, 200),
                  on ? kAmber : (hot ? alpha(kHolo, 200) : alpha(kGold, 60)), 1.f * s_ui);
            if (on) dl->AddRectFilled(p, ImVec2(p.x + 5.f * s_ui, p.y + sz.y), kAmber);
            drawText(dl, s_font, 22.f * s_ui, ImVec2(p.x + 22.f * s_ui, p.y + sz.y * 0.5f - 12.f * s_ui),
                     on ? kGold : (hot ? kText : kDim), kSectionNames[i], 4.f * s_ui);
            ImGui::Dummy(ImVec2(0, 4.f * s_ui));
        }
        ImGui::EndChild();
        ImGui::SameLine(0.f, 28.f * s_ui);

        // right: the section's settings
        ImGui::BeginChild("##page", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
        bodyFont(24.f * s_ui);
        s_valueX = std::min(360.f * s_ui, ImGui::GetContentRegionAvail().x * 0.42f);
        s_changed = s_display = false;
        switch (s_section) {
        case Display: pageDisplay(s); break;
        case Gameplay: pageGameplay(s); break;
        case Controls: pageControls(); break;
        case Cheats: pageCheats(s); break;
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(2);
        if (s_font) ImGui::PopFont();
        if (s_changed) settings::save();
        if (s_display) applyDisplaySettings();
    }
    ImGui::End();
    s_focus = false;
    // keyboard/controller navigation comes back a moment after a capture
    if (s_captureRow < 0 && s_navBackTb && tbNow() >= s_navBackTb) {
        s_navBackTb = 0;
        setNav(true);
    }
}

// R2_MENU_BIND_TEST=1 (with R2_MENU=1 R2_MENU_SECTION=2, automated runs only: their bindings
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

void setOpen(bool open) {
    if (!open && s_captureRow >= 0) {
        aurora::binding::cancel_capture();
        s_captureRow = -1;
    }
    s_open = open;
    s_focus = open;
    setNav(open);
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

void menuImGuiInit(const void*) {
    ImGuiIO& io = ImGui::GetIO();
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
    // automated runs: someone's controller must not open the menu over a scripted test
    static const bool automated = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    if (s_captureRow >= 0 || (s_navBackTb && tbNow() < s_navBackTb)) return;  // rebinding takes these keys
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.key == SDLK_F1) {
            logf("menu", "F1: %s", s_open ? "close" : "open");
            setOpen(!s_open);
        } else if (e.key.key == SDLK_ESCAPE && s_open) {
            logf("menu", "Esc: close");
            setOpen(false);
        }
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK && !automated) {
        logf("menu", "controller Back: %s", s_open ? "close" : "open");
        setOpen(!s_open);
    }
}

void applyDisplaySettings() {
    const settings::Settings& s = settings::get();
    VISetFrameBufferScale(settings::resolutionScale(s.resolution));
    AuroraSetDisplayAspect(settings::aspectValue(s.aspect));
    // widescreen: letterboxed cinematics fill the screen (4:3 keeps the original bars)
    AuroraSetLetterboxFill(s.aspect != settings::Aspect::R4_3);
    if (SDL_Window* w = aurora_get_window()) {
        const bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
        if (full != s.fullscreen) SDL_SetWindowFullscreen(w, s.fullscreen);
    }
    viSetRate(s.fps);
}

void menuDraw() {
    // widescreen: the game's cameras take the aspect the frame is rendered at (so they also cull
    // for it); 4:3 restores the original
    {
        u32 w = 0, h = 0;
        AuroraGetRenderSize(&w, &h);
        const float a = settings::aspectValue(settings::get().aspect);
        if (a > 0.f) setGameAspect(a);
        else if (w && h) setGameAspect(double(w) / double(h));
    }
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) return;
    settings::Settings& s = settings::get();
    static bool testOpen = envFlag("R2_MENU");  // screenshots of the menu in automated runs
    if (testOpen) {
        testOpen = false;
        setOpen(true);
        if (const char* sec = getenv("R2_MENU_SECTION")) s_section = std::clamp(atoi(sec), 0, int(SectionCount) - 1);
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
    if (s_open) drawMenu(s, t);
    if (s_open) bindSelfTest();
}

}  // namespace rt::gfx
