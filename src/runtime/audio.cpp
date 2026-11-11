// Host audio output: the AI DMA blocks the game plays (16-bit stereo, 32 or 48 kHz) go to an
// SDL3 audio stream. R2_NO_AUDIO=1 opens no device (automated runs); R2_AUDIO_DUMP=<file>
// also writes everything to a WAV file; R2_AUDIO_LOG=1 logs the queue once a second.
//
// Buffering: the game hands over a 5 ms block whenever the guest's clock says one is due (onAi,
// at the guest thread's safepoints), the device plays on its own clock. Started with nothing in
// hand, the queue sat at about one device period (5-20 ms, now and then 0.2 ms), so a block a
// few ms late left the device empty: crackling. Playback now starts (and starts again after the
// device ran dry: a load, a skipped cinematic, a state load) only once kTargetMs is queued, and
// the two clocks' drift is taken up by the stream's playback rate (at most 0.5% off, inaudible)
// rather than by dropping blocks, which clicked. R2_AUDIO_MS=<ms> sets the cushion.
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

double s_targetMs = 50.0;     // the cushion playback keeps
constexpr double kMaxMs = 150.0;  // past this blocks are dropped (only after a burst)
constexpr double kMaxRatioOff = 0.005;
bool s_playing = false;
double s_avgMs = 0.0;  // queue depth, smoothed: the device takes it in chunks of its period
float s_ratio = 1.f;
u64 s_underruns = 0, s_dropped = 0;

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

void setRatio(float r) {
    if (std::fabs(r - s_ratio) < 0.0002f && r != 1.f) return;
    if (r == s_ratio) return;
    s_ratio = r;
    SDL_SetAudioStreamFrequencyRatio(s_stream, r);
}

void stopPlaying() {
    if (!s_playing) return;
    SDL_PauseAudioStreamDevice(s_stream);
    s_playing = false;
    setRatio(1.f);
}

// R2_AUDIO_LOG=1: once a second
void logQueue(double queuedMs) {
    static const bool log = envFlag("R2_AUDIO_LOG");
    if (!log) return;
    static u64 lastTb = 0, pushes = 0, lastUnder = 0, lastDrop = 0;
    static double minMs = 1e9, maxMs = 0;
    pushes++;
    minMs = std::min(minMs, queuedMs);
    maxMs = std::max(maxMs, queuedMs);
    const u64 now = tbNow();
    if (now - lastTb < kTbClock) return;
    logf("audio", "%llu blocks/s, queue %.1f-%.1f ms (avg %.1f), ratio %.4f, underruns %llu, dropped %llu",
         (unsigned long long)pushes, minMs, maxMs, s_avgMs, s_ratio, (unsigned long long)(s_underruns - lastUnder),
         (unsigned long long)(s_dropped - lastDrop));
    lastTb = now;
    pushes = 0;
    lastUnder = s_underruns;
    lastDrop = s_dropped;
    minMs = 1e9;
    maxMs = 0;
}
}  // namespace

void init() {
    if (const char* d = getenv("R2_AUDIO_DUMP")) {
        s_dump = fopen(d, "wb");
        if (s_dump) logf("audio", "writing %s", d);
    }
    if (const char* ms = getenv("R2_AUDIO_MS")) s_targetMs = std::clamp(atof(ms), 10.0, 120.0);
    if (envFlag("R2_NO_AUDIO")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        logf("audio", "no audio: %s", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    // opened paused: playback starts once the cushion is queued (push)
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_stream) {
        logf("audio", "no audio device: %s", SDL_GetError());
        return;
    }
    s_rate = 32000;
    SDL_SetAudioStreamGain(s_stream, s_gain);
    logf("audio", "output: 32 kHz stereo, %.0f ms buffered", s_targetMs);
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
    const double bytesPerMs = s_rate * 4 / 1000.0;
    const double queuedMs = SDL_GetAudioStreamQueued(s_stream) / bytesPerMs;
    logQueue(queuedMs);
    // Ran dry (a dry stream still reports ~0.2 ms: the resampler's leftovers): one clean gap
    // while the cushion refills, instead of playing on with nothing in hand.
    if (s_playing && queuedMs < 2.0) {
        s_underruns++;
        stopPlaying();
    }
    if (queuedMs > kMaxMs) {
        s_dropped++;
        return;
    }
    SDL_PutAudioStreamData(s_stream, buf.data(), bytes);
    const double nowMs = queuedMs + bytes / bytesPerMs;
    if (!s_playing) {
        if (nowMs >= s_targetMs) {
            s_avgMs = s_targetMs;
            SDL_ResumeAudioStreamDevice(s_stream);
            s_playing = true;
        }
        return;
    }
    // drift: play a little faster while above the cushion, a little slower below it
    s_avgMs += (nowMs - s_avgMs) * 0.01;
    const double off = std::clamp((s_avgMs - s_targetMs) / s_targetMs, -1.0, 1.0) * kMaxRatioOff;
    setRatio(float(1.0 + off));
}

// a state was loaded: what is queued belongs to the other timeline
void flush() {
    if (!s_stream) return;
    SDL_ClearAudioStream(s_stream);
    stopPlaying();
}

}  // namespace rt::audio
