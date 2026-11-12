#include "settings.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime.h"

namespace rt::settings {
namespace {

Settings s_settings;
std::string s_path;  // empty: do not persist (automated runs)
const char* const kHotkeyNames[HkCount] = {"save_state", "load_state", "next_slot", "previous_slot"};

bool parseBool(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

void set(const std::string& key, const std::string& value) {
    Settings& s = s_settings;
    const int n = atoi(value.c_str());
    if (key == "fullscreen") s.fullscreen = parseBool(value);
    else if (key == "resolution") { if (n >= 0 && n < int(Resolution::Count)) s.resolution = Resolution(n); }
    else if (key == "aspect") {
        if (n >= 0 && n <= 3) s.aspect = Aspect(n);
        else if (n == 4) s.aspect = Aspect::R21_9;  // 32:9 (retired: kMaxAspect)
        else if (n == 5) s.aspect = Aspect::Window;
    }
    else if (key == "fps") { for (int f : kFpsChoices) if (f == n) s.fps = n; }
    else if (key == "show_fps") s.showFps = parseBool(value);
    else if (key == "hud_edges") s.hudEdges = parseBool(value);
    else if (key == "tile_menus") s.tileMenus = parseBool(value);
    else if (key == "volume") s.volume = n < 0 ? 0 : n > 100 ? 100 : n;
    else if (key == "background_audio") s.backgroundAudio = parseBool(value);
    else if (key == "skip_logos") s.skipLogos = parseBool(value);
    else if (key == "skip_cutscenes") s.skipCutscenes = parseBool(value);
    else if (key == "invincible") s.invincible = parseBool(value);
    else if (key == "infinite_lives") s.infiniteLives = parseBool(value);
    else if (key == "infinite_ammo") s.infiniteAmmo = parseBool(value);
    else if (key == "perfect_stats") s.perfectStats = parseBool(value);
    else if (key == "custom_textures") s.customTextures = parseBool(value);
    else if (key == "xbox_buttons") s.xboxButtons = parseBool(value);
    else if (key == "state_slot") s.stateSlot = n < 1 ? 1 : n > 8 ? 8 : n;
    else {
        for (int a = 0; a < HkCount; a++) {
            const std::string base = std::string("hotkey_") + kHotkeyNames[a];
            if (key == base + "_pad") return void(s.hotkeyPad[a] = value);
            if (key == base + "_keys") return void(s.hotkeyKeys[a] = value);
        }
        logf("config", "unknown setting '%s'", key.c_str());
    }
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

void parseLine(const std::string& line) {
    const std::string l = trim(line);
    if (l.empty() || l[0] == '#' || l[0] == ';' || l[0] == '[') return;
    const size_t eq = l.find('=');
    if (eq == std::string::npos) return;
    set(trim(l.substr(0, eq)), trim(l.substr(eq + 1)));
}

}  // namespace

Settings& get() { return s_settings; }

void load() {
    const bool automated = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    if (!automated) {
        if (char* dir = SDL_GetPrefPath(nullptr, "Red Two")) {
            s_path = std::string(dir) + "settings.ini";
            SDL_free(dir);
        }
    }
    if (!s_path.empty()) {
        if (FILE* f = fopen(s_path.c_str(), "r")) {
            char buf[512];
            while (fgets(buf, sizeof buf, f)) parseLine(buf);
            fclose(f);
            logf("config", "settings from %s", s_path.c_str());
        }
    }
    // R2_SET="fps=120;aspect=1": overrides, e.g. for test runs
    if (const char* o = getenv("R2_SET")) {
        std::string all = o;
        size_t start = 0;
        while (start <= all.size()) {
            const size_t end = all.find(';', start);
            parseLine(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
}

void save() {
    if (s_path.empty()) return;
    const Settings& s = s_settings;
    FILE* f = fopen(s_path.c_str(), "w");
    if (!f) {
        logf("config", "cannot write %s", s_path.c_str());
        return;
    }
    fprintf(f, "# Red Two settings (also editable in game: F1 or the controller's Back/Select button)\n");
    fprintf(f, "fullscreen=%d\n", s.fullscreen);
    fprintf(f, "# 0 window size, 1 480p, 2 960p, 3 1440p, 4 2160p (4K)\n");
    fprintf(f, "resolution=%d\n", int(s.resolution));
    fprintf(f, "# 0 4:3, 1 16:9, 2 16:10, 3 21:9, 5 match the window (at most 21:9)\n");
    fprintf(f, "aspect=%d\n", s.aspect == Aspect::Window ? 5 : int(s.aspect));
    fprintf(f, "fps=%d\n", s.fps);
    fprintf(f, "show_fps=%d\n", s.showFps);
    fprintf(f, "hud_edges=%d\n", s.hudEdges);
    fprintf(f, "tile_menus=%d\n", s.tileMenus);
    fprintf(f, "# master volume 0..100\n");
    fprintf(f, "volume=%d\n", s.volume);
    fprintf(f, "background_audio=%d\n", s.backgroundAudio);
    fprintf(f, "skip_logos=%d\n", s.skipLogos);
    fprintf(f, "skip_cutscenes=%d\n", s.skipCutscenes);
    fprintf(f, "invincible=%d\n", s.invincible);
    fprintf(f, "infinite_lives=%d\n", s.infiniteLives);
    fprintf(f, "infinite_ammo=%d\n", s.infiniteAmmo);
    fprintf(f, "perfect_stats=%d\n", s.perfectStats);
    fprintf(f, "# texture packs in the textures folder next to this file (Dolphin's format)\n");
    fprintf(f, "custom_textures=%d\n", s.customTextures);
    fprintf(f, "# on-screen button prompts: 1 as on an Xbox controller, 0 GameCube\n");
    fprintf(f, "xbox_buttons=%d\n", s.xboxButtons);
    fprintf(f, "# save states: the slot (1-8) and the hotkeys, buttons or keys held together (SDL names, \" + \" between)\n");
    fprintf(f, "state_slot=%d\n", s.stateSlot);
    for (int a = 0; a < HkCount; a++) {
        fprintf(f, "hotkey_%s_pad=%s\n", kHotkeyNames[a], s.hotkeyPad[a].c_str());
        fprintf(f, "hotkey_%s_keys=%s\n", kHotkeyNames[a], s.hotkeyKeys[a].c_str());
    }
    fclose(f);
}

const char* hotkeyDefault(int action, bool pad) {
    static const Settings d;
    if (action < 0 || action >= HkCount) return "";
    return pad ? d.hotkeyPad[action].c_str() : d.hotkeyKeys[action].c_str();
}

const char* resolutionName(Resolution r) {
    switch (r) {
    case Resolution::Window: return "Window size";
    case Resolution::X1: return "480p (native)";
    case Resolution::X2: return "960p (2x)";
    case Resolution::X3: return "1440p (3x)";
    case Resolution::X4K: return "2160p (4K)";
    default: return "?";
    }
}

const char* aspectName(Aspect a) {
    switch (a) {
    case Aspect::R4_3: return "4:3 (original)";
    case Aspect::R16_9: return "16:9 widescreen";
    case Aspect::R16_10: return "16:10";
    case Aspect::R21_9: return "21:9 ultrawide";
    case Aspect::Window: return "Match window";
    default: return "?";
    }
}

float resolutionScale(Resolution r) {
    switch (r) {
    case Resolution::X1: return 1.f;
    case Resolution::X2: return 2.f;
    case Resolution::X3: return 3.f;
    case Resolution::X4K: return 4.5f;  // 480 * 4.5 = 2160 lines
    default: return 0.f;
    }
}

float aspectValue(Aspect a) {
    switch (a) {
    case Aspect::R16_9: return 16.f / 9.f;
    case Aspect::R16_10: return 16.f / 10.f;
    case Aspect::R21_9: return 64.f / 27.f;  // 2560x1080 / 3440x1440 class panels
    case Aspect::Window: return -1.f;
    default: return 4.f / 3.f;  // the game's own aspect, pillarboxed in a wider window
    }
}

}  // namespace rt::settings
