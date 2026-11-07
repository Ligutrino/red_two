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
void applyDisplaySettings();           // resolution, aspect, fullscreen, frame rate -> Aurora/VI
}  // namespace rt::gfx
