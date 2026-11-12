// Player settings: %APPDATA%\Red Two\settings.ini, edited in the in-game menu (F1 or the
// controller's Back/Select button, see gfx/menu.cpp). Automated runs (R2_BACKGROUND,
// R2_HEADLESS, R2_INPUT) neither read nor write the file; R2_SET="key=value;..." overrides
// any setting for them (and for the player's own runs).
#pragma once
#include <string>

namespace rt::settings {

enum class Resolution : int { Window = 0, X1, X2, X3, X4K, Count };  // internal (EFB) resolution
// settings.ini keeps the numbers of the old list: 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 (was 32:9:
// now 21:9), 5 match the window
enum class Aspect : int { R4_3 = 0, R16_9, R16_10, R21_9, Window, Count };

// The widest aspect rendered (21:9); a wider window gets bars at the sides. Wider views make the
// game's cameras cull for more of the world and it keeps more in its virtual memory, which ARAM
// backs (1536 4 KiB slots): the Battle of Hoth intro at 32:9 needed 387+ slots more in its first
// second and stopped with "FATAL VM ERROR: Out of ARAM memory"; at 21:9 its gameplay uses ~1014.
inline constexpr float kMaxAspect = 64.f / 27.f;

struct Settings {
    // display
    bool fullscreen = false;
    Resolution resolution = Resolution::Window;
    Aspect aspect = Aspect::R4_3;
    bool hudEdges = true;  // widescreen: the flight HUD at the screen edges
    bool tileMenus = true;  // widescreen: menu backgrounds continue into the sides
    int fps = 60;  // 60, 90, 120 or 144: the emulated retrace rate
    bool showFps = false;
    // audio
    int volume = 100;            // master volume, 0..100 (output gain (volume/100)^2)
    bool backgroundAudio = true;  // keep playing while the window is in the background
    // game
    bool skipLogos = false;      // boot straight past the legal screen, intro and logos
    bool skipCutscenes = false;  // B (or Start) skips the opening crawl and in-game cutscenes
    // cheats
    bool invincible = false;
    bool infiniteLives = false;
    bool infiniteAmmo = false;  // secondary weapons: torpedoes, bombs, missiles
    bool perfectStats = false;  // mission results: 0:01, every enemy, 100%, nothing lost (hle_game.cpp)
    // mods
    bool customTextures = true;  // texture packs in the textures folder (Dolphin's format; mods.cpp)
    // controls
    bool xboxButtons = true;  // on-screen button prompts as on an Xbox controller (gfx/button_icons.cpp)
    // save states (savestate.cpp): the slot the hotkeys use, and the hotkeys (hotkeys.cpp), each a
    // combination held together: controller buttons by SDL name, keys by SDL key name, " + "
    // between them; empty = none
    int stateSlot = 1;
    std::string hotkeyPad[4] = {"back + rightshoulder", "back + leftshoulder", "back + dpright", "back + dpleft"};
    std::string hotkeyKeys[4] = {"F5", "F8", "F7", "F6"};
};
// hotkey actions (the index into hotkeyPad / hotkeyKeys)
enum HotkeyAction : int { HkSaveState = 0, HkLoadState, HkNextSlot, HkPrevSlot, HkCount };
const char* hotkeyDefault(int action, bool pad);

Settings& get();
void load();  // once at startup, before anything reads the settings
void save();  // after a change in the menu (no-op for automated runs)

const char* resolutionName(Resolution r);
const char* aspectName(Aspect a);
float resolutionScale(Resolution r);  // Aurora frame buffer scale; 0 = the window's own size
float aspectValue(Aspect a);          // width / height (4:3 = the game's own); -1 = the window's
inline constexpr int kFpsChoices[] = {60, 90, 120, 144};

}  // namespace rt::settings
