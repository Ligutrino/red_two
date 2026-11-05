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

Requirements: Visual Studio 2022 Build Tools (C++), CMake 3.25+, Ninja, clang-cl (LLVM),
Python 3.10+ with a local `libzstd` (for reading RVZ images).

```powershell
# 1. Unpack your disc (RVZ or ISO) into orig/
python tools\rvz.py extract "<your Rogue Leader.rvz>" orig\GSWE64
python tools\rvz.py iso     "<your Rogue Leader.rvz>" orig\GSWE64.iso

# 2. Build (recompiles orig\GSWE64\sys\main.dol, then compiles the runtime)
powershell -ExecutionPolicy Bypass -File tools\build.ps1

# 3. Run
build\windows\red_two.exe --disc orig\GSWE64.iso
```

The code generated from your disc ends up in `build/` and is yours alone: never share it.

### Playing

- Gamepads work through SDL (with rumble). Keyboard: WASD or arrows steer, Space = A (fire),
  Left Shift = B, E = X, Q = Y, R = Z, 1/2 = L/R, Enter = Start, IJKL = C-stick, TFGH = D-pad.
- F11 or Alt+Enter toggles fullscreen; `--fullscreen` starts that way.
- Saves go to `saves/GSWE64_A.raw` (a Memory Card 59 in slot A; the game formats a new one).
  `R2_CARD_A=<path>` uses another card image, for example a Dolphin `.raw` card.
- `R2_NO_AUDIO=1` runs without sound.

## How it works

See [CLAUDE.md](CLAUDE.md) for the architecture and what we learned about the game
(it ships a second executable inside `main.dol` and runs it from its own virtual memory).

## Legal

Red Two is released under the [MIT License](LICENSE). It covers this repository's own code
only: the code the build generates from your disc stays yours alone and must not be shared.

This project contains no code or assets from the game. Star Wars and Rogue Leader are
trademarks of Lucasfilm Ltd.; GameCube is a trademark of Nintendo. This project is not
affiliated with or endorsed by them.
