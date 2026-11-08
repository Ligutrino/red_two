// Host audio output: the AI DMA blocks the game plays (16-bit stereo, 32 or 48 kHz) go to an
// SDL3 audio stream. R2_NO_AUDIO=1 opens no device (automated runs); R2_AUDIO_DUMP=<file>
// also writes everything to a WAV file.
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <cstdio>
#include <vector>

#include "hw/musyx_dsp.h"
#include "runtime.h"

namespace rt::audio {

namespace {
SDL_AudioStream* s_stream = nullptr;
u32 s_rate = 0;
float s_gain = 1.f;
FILE* s_dump = nullptr;
u32 s_dumpBytes = 0, s_dumpRate = 0;
u64 s_dropped = 0;

void writeWavHeader() {
    auto u32le = [](u8* p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); };
    u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    u32le(h + 4, 36 + s_dumpBytes);
    u32le(h + 24, s_dumpRate);
    u32le(h + 28, s_dumpRate * 4);
    u32le(h + 40, s_dumpBytes);
    fseek(s_dump, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, s_dump);
    fseek(s_dump, 0, SEEK_END);
}
}  // namespace

void init() {
    if (const char* d = getenv("R2_AUDIO_DUMP")) {
        s_dump = fopen(d, "wb");
        if (s_dump) logf("audio", "writing %s", d);
    }
    if (envFlag("R2_NO_AUDIO")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        logf("audio", "no audio: %s", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_stream) {
        logf("audio", "no audio device: %s", SDL_GetError());
        return;
    }
    s_rate = 32000;
    SDL_SetAudioStreamGain(s_stream, s_gain);
    SDL_ResumeAudioStreamDevice(s_stream);
    logf("audio", "output: 32 kHz stereo");
}

void setGain(float gain) {
    if (gain == s_gain) return;
    s_gain = gain;
    if (s_stream) SDL_SetAudioStreamGain(s_stream, gain);
}

void push(const u8* data, u32 frames, u32 rate) {
    static std::vector<s16> buf;
    buf.resize(size_t(frames) * 2);
    for (u32 i = 0; i < frames * 2; i++) buf[i] = s16(data[2 * i] << 8 | data[2 * i + 1]);
    const int bytes = int(frames * 4);
    if (s_dump) {
        if (!s_dumpRate) s_dumpRate = rate;
        fwrite(buf.data(), 1, size_t(bytes), s_dump);
        s_dumpBytes += u32(bytes);
        writeWavHeader();
    }
    if (!s_stream) return;
    if (rate != s_rate) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(rate)};
        SDL_SetAudioStreamFormat(s_stream, &spec, nullptr);
        s_rate = rate;
    }
    // The guest's clock and the device's drift apart: keep at most ~120 ms queued.
    if (SDL_GetAudioStreamQueued(s_stream) > int(s_rate * 4 * 12 / 100)) {
        s_dropped++;
        return;
    }
    SDL_PutAudioStreamData(s_stream, buf.data(), bytes);
}

}  // namespace rt::audio
