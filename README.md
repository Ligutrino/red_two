# Red Two

A static recompilation of **Star Wars: Rogue Squadron II – Rogue Leader** (GameCube) for PC.

The game's own PowerPC code is translated to C++ ahead of time and runs on a native runtime
that stands in for the console's hardware. **No game data is included**: you need your own
disc image of the USA release (`GSWE64`, revision 0).

> Status: early development. The recompiled game boots, runs its menus and cutscenes, and the
> first mission is playable at full speed with graphics (Aurora) and controls (keyboard or
> gamepad). Saving works (memory card in slot A, `saves/GSWE64_A.raw`). Sound plays (music and effects
> through an emulation of the MusyX audio DSP). Only the first mission has been tried.

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

### Playing

- Gamepads work through SDL (with rumble), also while the window is in the background. On an
  Xbox-style pad the face buttons follow the GameCube positions: A = A (fire), X (left) = B,
  B (right) = X, Y = Y; LT/RT = L/R, RB = Z, right stick = C-stick, Menu = Start. Keyboard: WASD or arrows steer, Space = A (fire),
  Left Shift = B, E = X, Q = Y, R = Z, 1/2 = L/R, Enter = Start, IJKL = C-stick, TFGH = D-pad.
- The game's button prompts show the Xbox controller's buttons (the one each GameCube button is
  bound to, e.g. "PRESS (Y)" and SELECT CRAFT's "(B) > pilot"); F1 > Controls > Button prompts
  switches back to the GameCube ones. The C-stick icon stays.
- F11 or Alt+Enter toggles fullscreen; `--fullscreen` starts that way.
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
  `Load\Textures`; `.dds` and `.png`, mip files and wildcards work). Gecko / Action Replay codes and
  patches go in `%APPDATA%\Red Two\codes.ini` (Dolphin's GameSettings format) and can be switched
  in the menu. The game is recompiled ahead of time, so codes that change its code (Gecko C2
  "insert ASM", writes into code) cannot work; the menu marks them as not supported.
- **Importing from Dolphin**: MODS -> IMPORT FROM DOLPHIN, or
  `red_two.exe --import-dolphin[=saves,textures,codes] --console`. Red Two finds Dolphin's user
  folder (AppData, Documents, a portable install, the registry, a custom Load folder in
  `Dolphin.ini`) and brings over this game's newest save (from GCI folders or `.raw` cards; the
  previous card is kept in `saves\backup`, and the game restarts), its texture packs (copied) and
  its codes (with their on/off states). `R2_DOLPHIN_DIR=<User folder>` points it elsewhere.
- `R2_NO_AUDIO=1` runs without sound. The log goes to `red_two.log` (or `--console` for a
  console window).

## How it works

See [CLAUDE.md](CLAUDE.md) for the architecture and what we learned about the game
(it ships a second executable inside `main.dol` and runs it from its own virtual memory).

## Legal

Red Two is released under the [MIT License](LICENSE). It covers this repository's own code
only: the code the build generates from your disc stays yours alone and must not be shared.

This project contains no code or assets from the game. Star Wars and Rogue Leader are
trademarks of Lucasfilm Ltd.; GameCube is a trademark of Nintendo. This project is not
affiliated with or endorsed by them.
