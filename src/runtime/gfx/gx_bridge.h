// The game's GX command stream rendered with Aurora (gx_bridge.cpp).
#pragma once
#include <string>

#include "../ctx.h"

namespace rt::gfx {
bool init(int argc, char** argv);  // window + renderer; registers the GX command sink
void vblank();                     // once per emulated retrace (keeps the window responsive)
u64 frames();                      // frames presented
std::string stats();               // Aurora's per-frame statistics

// menu.cpp: the settings menu (F1 or the controller's Back/Select button)
void menuEvent(const void* sdlEvent);  // every SDL event (opens/closes the menu)
void menuDraw();                       // inside a frame, before it ends (ImGui)
// menu.cpp: the startup screen while shaders compile and textures prefetch (inside a frame)
void drawShaderProgress(u32 done, u32 total, u64 texDone, u64 texTotal, u64 texBytes);
void applyDisplaySettings();           // resolution, aspect, fullscreen, frame rate -> Aurora/VI
int uberShaderMode();                  // AuroraSetUberShaders' mode: 0 off, 1 hybrid (R2_UBER overrides)
void menuImGuiInit(const void* windowSize);  // Aurora's ImGui init callback: fonts and style
}  // namespace rt::gfx
