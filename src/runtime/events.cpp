// Guest time base, decrementer and timed device events.
//
// Device state is only touched on the guest thread. A host timer thread sleeps until the
// earliest scheduled event and then raises g_irqPending; the guest polls it at loop
// back-edges (safepoint), runs due events there and takes interrupts if enabled.
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "runtime.h"

namespace rt {

void (*g_eventHandlers[EV_COUNT])() = {};
volatile u32 g_irqLine = 0;

namespace {
LARGE_INTEGER s_qpcFreq, s_qpcStart;
u64 s_due[EV_COUNT];  // 0 = not scheduled
std::mutex s_mu;
std::condition_variable s_cv;
std::atomic<u64> s_earliest{~0ull};

u64 s_decBaseTb = 0;
u32 s_decValue = 0;
bool s_decPending = false;

void recomputeEarliest() {
    u64 e = ~0ull;
    for (u64 d : s_due)
        if (d && d < e) e = d;
    s_earliest = e;
}

void timerThread() {
    std::unique_lock lk(s_mu);
    for (;;) {
        const u64 e = s_earliest.load();
        const u64 now = tbNow();
        if (e <= now) {
            g_irqPending = 1;
            // wait until the guest consumed it (it reschedules), but do not spin
            s_cv.wait_for(lk, std::chrono::microseconds(200));
            continue;
        }
        if (e == ~0ull) {
            s_cv.wait_for(lk, std::chrono::milliseconds(50));
            continue;
        }
        const u64 waitUs = (e - now) * 1000000 / kTbClock;
        if (waitUs > 2000) s_cv.wait_for(lk, std::chrono::microseconds(waitUs - 1000));
        else {
            lk.unlock();
            while (tbNow() < e) YieldProcessor();
            lk.lock();
        }
    }
}
}  // namespace

u64 tbNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const u64 d = u64(t.QuadPart - s_qpcStart.QuadPart);
    const u64 f = u64(s_qpcFreq.QuadPart);
    return (d / f) * kTbClock + (d % f) * kTbClock / f;
}

void eventsInit() {
    QueryPerformanceFrequency(&s_qpcFreq);
    QueryPerformanceCounter(&s_qpcStart);
    timeBeginPeriod(1);
    std::thread(timerThread).detach();
}

void schedule(int id, u64 at) {
    if (!at) at = 1;
    std::lock_guard lk(s_mu);
    s_due[id] = at;
    recomputeEarliest();
    s_cv.notify_all();
}

void unschedule(int id) {
    std::lock_guard lk(s_mu);
    s_due[id] = 0;
    recomputeEarliest();
}

bool scheduled(int id) { return s_due[id] != 0; }

void wakeGuest() { g_irqPending = 1; }

void runDueEvents() {
    const u64 now = tbNow();
    for (int id = 0; id < EV_COUNT; id++) {
        bool fire = false;
        {
            std::lock_guard lk(s_mu);
            if (s_due[id] && s_due[id] <= now) {
                s_due[id] = 0;
                recomputeEarliest();
                fire = true;
            }
        }
        if (fire && g_eventHandlers[id]) g_eventHandlers[id]();
    }
}

// --- decrementer ----------------------------------------------------------------------
static void onDec() {
    s_decPending = true;
    g_irqLine |= 2;
    wakeGuest();
}

void decWrite(u32 v) {
    s_decBaseTb = tbNow();
    s_decValue = v;
    if (!g_eventHandlers[EV_DEC]) g_eventHandlers[EV_DEC] = onDec;
    if (v & 0x80000000u) onDec();
    else schedule(EV_DEC, s_decBaseTb + v + 1);
}

u32 decRead() { return s_decValue - u32(tbNow() - s_decBaseTb); }
bool decPending() { return s_decPending; }
void decAck() {
    s_decPending = false;
    g_irqLine &= ~2u;
}

}  // namespace rt
