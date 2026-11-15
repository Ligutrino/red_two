# Red Two

A static recompilation of **Star Wars: Rogue Squadron II – Rogue Leader** (GameCube) for PC.

The game's own PowerPC code is translated to C++ ahead of time and runs on a native runtime
that stands in for the console's hardware. **No game code or data is included, and there are no
prebuilt downloads**: you build it from your own disc image of the USA release (`GSWE64`,
revision 0), and the build recompiles the game from it.

> Status: playable on Windows and Linux (also packaged as an AppImage for the Steam Deck):
> menus, cutscenes and missions at full speed with graphics (Aurora), sound (an emulation of the
> MusyX audio DSP), controls, saving, save states, widescreen, mods, cheats and RetroAchievements.

## Building (Windows)

Requirements: Visual Studio 2022 Build Tools (C++), CMake 3.25+, Ninja, clang-cl (LLVM).

```powershell
# 1. Build: recompiles the game straight from your disc image (RVZ, WIA, ISO or GCM)
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Disc "<your Rogue Leader.rvz>"

# 2. Run
build\windows\red_two.exe --disc "<your Rogue Leader.rvz>"
```

Without `--disc` the game looks for `orig\GSWE64.iso`, `orig\GSWE64.rvz`, `GSWE64.rvz` and
`GSWE64.iso`. `tools\rvz.py` (Python + libzstd) can still extract a disc or convert it to ISO.

The code generated from your disc ends up in `build/` and is yours alone: never share it.

`tools\make_release_win.ps1` packs a build into `build\RedTwo-windows-x64.zip` (the program, its
symbols, DLLs, a readme) for your own use on another PC.

## Building (Linux) and the Steam Deck AppImage

Requirements: clang, lld, CMake 3.25+, Ninja, git, and SDL3's build dependencies (X11, Wayland,
xkbcommon, ALSA / PulseAudio / PipeWire, udev and D-Bus development packages). Ubuntu 24.04 (also
under WSL2) works; the result needs glibc 2.38 or later (SteamOS 3.6+).

```bash
tools/build_linux.sh --disc "<your Rogue Leader.rvz>"     # -> build/linux/red_two
# an AppImage: the AppImage type 2 runtime (github.com/AppImage/type2-runtime, runtime-x86_64),
# mksquashfs and patchelf
tools/make_appimage.sh --runtime runtime-x86_64 --icon <256x256 png>   # -> build/linux/RedTwo-x86_64.AppImage
```

On the Steam Deck: copy the AppImage and your disc image (`.rvz` / `.iso`, any name) into the
same folder (or the disc into `~/Emulation/roms/gc`), make the AppImage executable, and add it
to Steam as a non-Steam game. The first start on a Deck is fullscreen with the picture as wide
as the screen. Settings, saves, states and logs live in `~/.local/share/Red Two/`.

### Playing

- Gamepads work through SDL (with rumble), also while the window is in the background. On an
  Xbox-style pad the face buttons follow the GameCube positions: A = A (fire), X (left) = B,
  B (right) = X, Y = Y; LT/RT = L/R, RB = Z, right stick = C-stick, Menu = Start. Keyboard: WASD or arrows steer, Space = A (fire),
  Left Shift = B, E = X, Q = Y, R = Z, 1/2 = L/R, Enter = Start, IJKL = C-stick, TFGH = D-pad.
- The game's button prompts show the Xbox controller's buttons (the one each GameCube button is
  bound to, e.g. "PRESS (Y)" and SELECT CRAFT's "(B) > pilot"); F1 > Controls > Button prompts
  switches back to the GameCube ones. The C-stick icon stays.
- F11 or Alt+Enter toggles fullscreen; `--fullscreen` starts that way.
- **Cheats** (F1 > Cheats): invincibility, infinite lives, infinite bombs, perfect mission
  stats (the results screen says 0:01, every enemy destroyed, 100% accuracy, no friendlies or
  lives lost and 100% targeting computer efficiency, and the medal is awarded for those), and
  all craft (every craft a mission allows waits in its hangar). **Passcodes**: the game's own
  unlocks without typing codes - all missions, all tech upgrades, ace mode, art gallery, music
  hall, documentary, audio commentary, credits, black and white - kept in the game's save as
  if the codes had been entered.
- **RetroAchievements** (F1 > Achievements): log in with your retroachievements.org account
  (only the login token is stored) or take Dolphin's login; softcore only, so save states and
  cheats stay usable. Unlocks pop up in the top right corner.
- **Reset game** and **Quit to desktop** are at the top of the menu's first page.
- **Save states**: Back + RB saves, Back + LB loads, Back + D-pad right / left picks the slot
  (8 slots; keyboard F5 / F8, F7 / F6). F1 > Controls has the slot, save / load buttons and the
  hotkeys, each any combination of buttons (or keys) held together: pick the row, press A, hold
  the buttons, let go. On a controller Back alone (released with nothing else pressed) still
  opens the menu. States live in `%APPDATA%\Red Two\states` and also load after a restart, but
  only into the same build of Red Two (a rebuild makes them "OLD"). The memory card is not part
  of a state: loading one does not undo the game's own saves.
- Saves go to `saves/GSWE64_A.raw` (a Memory Card 59 in slot A; the game formats a new one).
  `R2_CARD_A=<path>` uses another card image.
- **Mods** (F1, MODS tab): texture packs in Dolphin's format go in
  `%APPDATA%\Red Two\textures\` (a `GSWE64` folder inside, exactly as in Dolphin's
  `Load\Textures`; `.dds` and `.png`, mip files and wildcards work), or stay wherever you unpacked
  them: MODS -> Texture pack folder opens a folder browser (d-pad / stick, A opens, X goes up, B
  back) - pick the pack's folder (the one holding `GSWE64`, or `GSWE64` itself), e.g. in Downloads
  or on an SD card. Gecko / Action Replay codes and
  patches go in `%APPDATA%\Red Two\codes.ini` (Dolphin's GameSettings format) and can be switched
  in the menu. The game is recompiled ahead of time, so codes that change its code (Gecko C2
  "insert ASM", writes into code) cannot work; the menu marks them as not supported.
- **Importing from Dolphin**: MODS -> IMPORT FROM DOLPHIN, or
  `red_two.exe --import-dolphin[=saves,textures,codes] --console`. Red Two finds Dolphin's user
  folder (AppData, Documents, a portable install, the registry, a custom Load folder in
  `Dolphin.ini`) and brings over this game's newest save (from GCI folders or `.raw` cards; the
  previous card is kept in `saves\backup`, and the game restarts), its texture packs (copied) and
  its codes (with their on/off states). `R2_DOLPHIN_DIR=<User folder>` points it elsewhere.
- **Shaders** (F1, DISPLAY -> PERFORMANCE): "Prepare shaders at start" (on) compiles every shader
  the game has used on this device before it starts; off, the game starts at once and objects may
  pop in while their shaders compile. "Ubershaders" (off) draws not-yet-compiled objects with a
  generic shader instead of skipping them.
- `R2_NO_AUDIO=1` runs without sound. Every session writes a log to `%APPDATA%\Red Two\logs`
  (Linux: `~/.local/share/Red Two/logs`); `--console` shows it in a console window on Windows.

## How it works

`tools/rvz.py` and `src/common/disc_image.cpp` read the disc; the recompiler (`src/recompiler`)
finds the game's functions in `main.dol` without symbols and writes them out as C++ - including the
second executable the game hides inside `main.dol` and runs from its own demand-paged virtual
memory. The runtime (`src/runtime`) stands in for the console: the CPU state and MMU, threads on
host fibers, the GameCube's hardware at the register level (video, disc, audio DSP, ARAM, memory
card, controllers), and a bridge that hands the GPU command stream to
[Aurora](https://github.com/encounter/aurora) for rendering. Names we gave the game's functions are
in `config/GSWE64/symbols.txt`.

## Legal

Red Two is released under the [MIT License](LICENSE). It covers this repository's own code
only: the code the build generates from your disc stays yours alone and must not be shared.

This project contains no code or assets from the game. Star Wars and Rogue Leader are
trademarks of Lucasfilm Ltd.; GameCube is a trademark of Nintendo. This project is not
affiliated with or endorsed by them.
