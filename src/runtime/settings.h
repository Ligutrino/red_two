// Player settings: %APPDATA%\Red Two\settings.ini, edited in the in-game menu (F1 or the
// controller's Back/Select button, see gfx/menu.cpp). Automated runs (R2_BACKGROUND,
// R2_HEADLESS, R2_INPUT) neither read nor write the file; R2_SET="key=value;..." overrides
// any setting for them (and for the player's own runs).
#pragma once
#include <string>

namespace rt::settings {

enum class Resolution : int { Window = 0, X1, X2, X3, X4K, Count };  // internal (EFB) resolution
enum class Aspect : int { R4_3 = 0, R16_9, R16_10, R21_9, R32_9, Window, Count };

struct Settings {
    // display
    bool fullscreen = false;
    Resolution resolution = Resolution::Window;
    Aspect aspect = Aspect::R4_3;
    bool hudEdges = true;  // widescreen: the flight HUD at the screen edges
    bool tileMenus = true;  // widescreen: menu backgrounds continue into the sides
    int fps = 60;  // 60, 90, 120 or 144: the emulated retrace rate
    bool showFps = false;
    // game
    bool skipLogos = false;      // boot straight past the legal screen, intro and logos
    bool skipCutscenes = false;  // B (or Start) skips the opening crawl and in-game cutscenes
    // cheats
    bool invincible = false;
    bool infiniteLives = false;
    bool infiniteAmmo = false;  // secondary weapons: torpedoes, bombs, missiles
};

Settings& get();
void load();  // once at startup, before anything reads the settings
void save();  // after a change in the menu (no-op for automated runs)

const char* resolutionName(Resolution r);
const char* aspectName(Aspect a);
float resolutionScale(Resolution r);  // Aurora frame buffer scale; 0 = the window's own size
float aspectValue(Aspect a);          // width / height (4:3 = the game's own); -1 = the window's
inline constexpr int kFpsChoices[] = {60, 90, 120, 144};

}  // namespace rt::settings
