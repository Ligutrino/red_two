// The game's GX command stream rendered with Aurora (gx_bridge.cpp).
#pragma once
#include <string>

#include "../ctx.h"

namespace rt::gfx {
bool init(int argc, char** argv);  // window + renderer; registers the GX command sink
void vblank();                     // once per emulated retrace (keeps the window responsive)
u64 frames();                      // frames presented
std::string stats();               // Aurora's per-frame statistics
}  // namespace rt::gfx
