// In-game settings menu: Dear ImGui (through Aurora), drawn over the game's frame. F1 or the
// controller's Back/Select button (the GameCube pad has none, so the game never sees it) opens
// and closes it; Escape closes it. While it is open the game sees idle controllers
// (input.cpp), and the menu is driven by mouse, keyboard or controller (d-pad + A, B backs out).
#include "gx_bridge.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

#include "../runtime.h"
#include "../settings.h"

namespace rt::gfx {
namespace {

bool s_open = false;
bool s_focus = false;  // just opened: focus the window and its first item
u64 s_fpsFrames = 0, s_fpsTb = 0;
float s_fps = 0.f;

void setOpen(bool open) {
    s_open = open;
    s_focus = open;
    if (ImGuiContext* ctx = ImGui::GetCurrentContext()) {
        ImGuiIO& io = ctx->IO;
        constexpr ImGuiConfigFlags nav = ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
        if (open) io.ConfigFlags |= nav;
        else io.ConfigFlags &= ~nav;
    }
}

template <typename E>
bool enumCombo(const char* label, E& value, const char* (*name)(E)) {
    bool changed = false;
    if (ImGui::BeginCombo(label, name(value))) {
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
    return changed;
}

bool fpsCombo(int& fps) {
    bool changed = false;
    char cur[16];
    snprintf(cur, sizeof cur, "%d fps", fps);
    if (ImGui::BeginCombo("Frame rate", cur)) {
        for (int f : settings::kFpsChoices) {
            char item[16];
            snprintf(item, sizeof item, "%d fps", f);
            const bool selected = fps == f;
            if (ImGui::Selectable(item, selected) && !selected) {
                fps = f;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void hint(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) || (ImGui::IsItemFocused() && ImGui::GetIO().NavActive))
        ImGui::SetItemTooltip("%s", text);
}

}  // namespace

bool menuOpen() { return s_open; }

void menuEvent(const void* p) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    // automated runs: someone's controller must not open the menu over a scripted test
    static const bool automated = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
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
    const float scale = std::clamp(io.DisplaySize.y / 720.f, 1.f, 4.f);
    if (s.showFps) {
        ImGui::SetNextWindowPos(ImVec2(8.f * scale, 8.f * scale), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.5f);
        constexpr ImGuiWindowFlags overlay = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##fps", nullptr, overlay)) {
            ImGui::SetWindowFontScale(scale);
            ImGui::Text("%.0f fps", s_fps);
        }
        ImGui::End();
    }
    if (cinematicSkip()) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.f);
        constexpr ImGuiWindowFlags overlay = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##skip", nullptr, overlay)) {
            ImGui::SetWindowFontScale(scale * 1.5f);
            ImGui::TextUnformatted("Skipping...");
        }
        ImGui::End();
    }
    if (!s_open) return;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (s_focus) ImGui::SetNextWindowFocus();
    bool open = true;
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Red Two settings", &open, flags)) {
        ImGui::SetWindowFontScale(scale);
        ImGui::PushItemWidth(220.f * scale);
        bool changed = false, display = false;

        ImGui::SeparatorText("Display");
        if (s_focus) ImGui::SetKeyboardFocusHere();
        if (ImGui::Checkbox("Fullscreen", &s.fullscreen)) changed = display = true;
        hint("Also F11 or Alt+Enter.");
        if (enumCombo("Internal resolution", s.resolution, settings::resolutionName)) changed = display = true;
        hint("The resolution the game is rendered at. Window size follows the window; 4K renders "
             "2160 lines whatever the window size.");
        if (enumCombo("Aspect ratio", s.aspect, settings::aspectName)) changed = display = true;
        hint("Widescreen widens the 3D view; the HUD and menus keep their shape.");
        if (fpsCombo(s.fps)) changed = display = true;
        hint("Above 60 the game runs more frames per second at its normal speed.");
        if (ImGui::Checkbox("Show frame rate", &s.showFps)) changed = true;

        ImGui::SeparatorText("Game");
        if (ImGui::Checkbox("Skip intro logos", &s.skipLogos)) changed = true;
        hint("Start at the title screen (takes effect at the next start).");
        if (ImGui::Checkbox("Skippable cutscenes", &s.skipCutscenes)) changed = true;
        hint("Press B during the opening crawl or a cutscene to skip to the gameplay.");

        ImGui::SeparatorText("Cheats");
        if (ImGui::Checkbox("Invincibility", &s.invincible)) changed = true;
        if (ImGui::Checkbox("Infinite lives", &s.infiniteLives)) changed = true;

        ImGui::Separator();
        if (ImGui::Button("Close")) open = false;
        ImGui::SameLine();
        ImGui::TextDisabled("F1 / Back / Esc");
        ImGui::PopItemWidth();
        if (changed) settings::save();
        if (display) applyDisplaySettings();
    }
    ImGui::End();
    s_focus = false;
    if (!open) setOpen(false);
}

}  // namespace rt::gfx
