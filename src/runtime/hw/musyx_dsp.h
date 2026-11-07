#pragma once
// MusyX DSP ucode (MusyX <= 2.0.0) emulated at the command-list level; see musyx_dsp.cpp.
#include <string>

#include "../ctx.h"

namespace rt {

// A CPU->DSP mail while the ucode runs. Returns true with the DSP's answer in `reply` when a
// command list was processed (the caller posts it with a DSP interrupt).
bool musyxDspMail(u32 mail, u32& reply);
std::string musyxDspStats();  // lists and voice frames since the last call

u8* aramBase();  // hw.cpp: the 16 MiB of ARAM

namespace audio {
void init();  // host audio output (SDL3); R2_NO_AUDIO=1 for none, R2_AUDIO_DUMP=<file.wav>
// One AI DMA block as it starts playing: big-endian 16-bit stereo frames. speed > 1: the game
// is fast-forwarded and its audio clock with it; every `speed` frames are averaged into one, so
// the block takes 1/speed of its time and sounds that much faster.
void push(const u8* data, u32 frames, u32 rate, u32 speed = 1);
}  // namespace audio

}  // namespace rt
