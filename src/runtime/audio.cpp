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
//
// The output can be lost behind the game's back: the user's sound stopped in a session (the game
// still mixed: a state saved then played loud in a new process). The stream follows Windows'
// default device itself (SDL migrates it), but is opened again when its device is removed
// (deviceEvent) or stops taking sound - full for a second while playing; device changes are
// logged, and every perf line says what the game played (sound: blocks, peak, gaps, drops).
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <atomic>
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
u64 s_fullSinceTb = 0;  // the queue has been past kMaxMs since then (0: it is not)
// for the perf lines (perfText): since the last one
std::atomic<u64> s_perfBlocks{0}, s_perfGaps{0}, s_perfDropped{0}, s_perfReopened{0};
std::atomic<int> s_perfPeak{0};

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

std::string deviceName(SDL_AudioDeviceID id) {
    const char* n = id ? SDL_GetAudioDeviceName(id) : nullptr;
    return n ? n : "?";
}

// opened paused: playback starts once the cushion is queued (push)
bool openStream() {
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(s_rate)};
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_stream) {
        logf("audio", "no audio device: %s", SDL_GetError());
        return false;
    }
    SDL_SetAudioStreamGain(s_stream, s_gain);
    s_playing = false;
    s_ratio = 1.f;
    s_fullSinceTb = 0;
    logf("audio", "output: %u Hz stereo, %.0f ms buffered, on %s", s_rate, s_targetMs,
         deviceName(SDL_GetAudioStreamDevice(s_stream)).c_str());
    return true;
}

void reopen(const char* why) {
    logf("audio", "output %s: opening the default device again", why);
    s_perfReopened++;
    if (s_stream) SDL_DestroyAudioStream(s_stream);
    s_stream = nullptr;
    openStream();
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
    s_rate = 32000;
    openStream();
}

// every SDL event (gx_bridge pollEvents): playback devices coming and going
void deviceEvent(const void* p) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    if (e.type != SDL_EVENT_AUDIO_DEVICE_ADDED && e.type != SDL_EVENT_AUDIO_DEVICE_REMOVED &&
        e.type != SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED)
        return;
    if (e.adevice.recording) return;
    const SDL_AudioDeviceID ours = s_stream ? SDL_GetAudioStreamDevice(s_stream) : 0;
    const char* what = e.type == SDL_EVENT_AUDIO_DEVICE_ADDED ? "added" : e.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ? "removed" : "format changed";
    logf("audio", "playback device %u %s: %s%s", unsigned(e.adevice.which), what,
         e.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ? "" : deviceName(e.adevice.which).c_str(),
         ours && e.adevice.which == ours ? " (the game's output)" : "");
    if (s_stream && e.type == SDL_EVENT_AUDIO_DEVICE_REMOVED && e.adevice.which == ours) reopen("device removed");
    // no output so far (there was no device), and now there is one
    static const bool none = envFlag("R2_NO_AUDIO");
    if (!s_stream && !none && s_rate && e.type == SDL_EVENT_AUDIO_DEVICE_ADDED) openStream();
}

std::string perfText() {
    const u64 blocks = s_perfBlocks.exchange(0), gaps = s_perfGaps.exchange(0), dropped = s_perfDropped.exchange(0),
              reopened = s_perfReopened.exchange(0);
    const int peak = s_perfPeak.exchange(0);
    char t[160];
    snprintf(t, sizeof t, "sound %llu blocks, peak %d%%, %llu gaps, %llu dropped%s", (unsigned long long)blocks,
             peak * 100 / 32768, (unsigned long long)gaps, (unsigned long long)dropped,
             reopened ? ", output reopened" : "");
    return t;
}

void setGain(float gain) {
    if (gain == s_gain) return;
    s_gain = gain;
    if (s_stream) SDL_SetAudioStreamGain(s_stream, gain);
}

void push(const u8* data, u32 frames, u32 rate) {
    static std::vector<s16> buf;
    buf.resize(size_t(frames) * 2);
    int peak = 0;
    for (u32 i = 0; i < frames * 2; i++) {
        buf[i] = s16(data[2 * i] << 8 | data[2 * i + 1]);
        peak = std::max(peak, std::abs(int(buf[i])));
    }
    s_perfBlocks++;
    if (peak > s_perfPeak.load(std::memory_order_relaxed)) s_perfPeak.store(peak, std::memory_order_relaxed);
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
    // R2_AUDIO_STALL_TEST=<seconds>: the device stops taking sound then (paused behind push's
    // back), as a lost output does; the output must be opened again a second later
    static const double stallAt = getenv("R2_AUDIO_STALL_TEST") ? atof(getenv("R2_AUDIO_STALL_TEST")) : -1.0;
    if (stallAt >= 0.0 && s_playing) {
        static const u64 t0 = tbNow();
        static bool stalled = false;
        if (!stalled && double(tbNow() - t0) / double(kTbClock) >= stallAt) {
            stalled = true;
            logf("audio", "stall test: output paused behind the game's back");
            SDL_PauseAudioStreamDevice(s_stream);
        }
    }
    const double bytesPerMs = s_rate * 4 / 1000.0;
    const double queuedMs = SDL_GetAudioStreamQueued(s_stream) / bytesPerMs;
    logQueue(queuedMs);
    // Ran dry (a dry stream still reports ~0.2 ms: the resampler's leftovers): one clean gap
    // while the cushion refills, instead of playing on with nothing in hand.
    if (s_playing && queuedMs < 2.0) {
        s_underruns++;
        s_perfGaps++;
        stopPlaying();
    }
    if (queuedMs > kMaxMs) {
        s_dropped++;
        s_perfDropped++;
        // the device takes nothing (a burst after a stall drains in a few blocks)
        const u64 now = tbNow();
        if (!s_fullSinceTb) s_fullSinceTb = now;
        else if (now - s_fullSinceTb > kTbClock) reopen("stopped taking sound for a second");
        return;
    }
    s_fullSinceTb = 0;
    SDL_PutAudioStreamData(s_stream, buf.data(), bytes);
    const double nowMs = queuedMs + bytes / bytesPerMs;
    if (!s_playing) {
        if (nowMs >= s_targetMs) {
            s_avgMs = s_targetMs;
            if (!SDL_ResumeAudioStreamDevice(s_stream)) {
                static int logs = 0;
                if (logs++ < 5) logf("audio", "cannot start the output: %s", SDL_GetError());
            }
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
