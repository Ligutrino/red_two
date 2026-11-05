// Sampling profiler (R2_PROFILE=<seconds>): samples the instruction pointer of the guest's
// host thread (all guest threads are fibers on it) about once a millisecond and, every
// <seconds>, logs the hottest host functions - generated f_XXXXXXXX and runtime code alike -
// resolved from the PDB through DbgHelp.
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "runtime.h"

namespace rt {

namespace {

struct Sym {
    std::string name;
    u64 start = 0;
};

// Resolves an address to its function; caches whole functions by start address.
const Sym& resolve(u64 rip) {
    static std::unordered_map<u64, Sym> byRip;
    auto it = byRip.find(rip);
    if (it != byRip.end()) return it->second;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    Sym s;
    if (SymFromAddr(GetCurrentProcess(), rip, &disp, si)) {
        s.name = si->Name;
        s.start = rip - disp;
    } else {
        char b[32];
        snprintf(b, sizeof b, "?%llx", (unsigned long long)rip);
        s.name = b;
    }
    return byRip.emplace(rip, std::move(s)).first->second;
}

// Library leaf functions are attributed to their caller too (the return address on the stack
// at the sample), e.g. "memmove <- rt::aramDma".
bool isLeafHelper(const std::string& n) {
    return n == "memmove" || n == "memcpy" || n == "memset" || n == "memcmp" || n.rfind("XXH", 0) == 0;
}

std::unordered_map<u64, std::unordered_map<u64, u32>> s_callers;  // rip -> return address -> hits

void report(const std::unordered_map<u64, u32>& hits, u64 total, int secs) {
    std::unordered_map<std::string, u32> byName;
    for (auto& [rip, n] : hits) {
        const std::string& name = resolve(rip).name;
        if (isLeafHelper(name)) {
            if (auto it = s_callers.find(rip); it != s_callers.end()) {
                for (auto& [ret, m] : it->second) byName[name + " <- " + resolve(ret).name] += m;
                continue;
            }
        }
        byName[name] += n;
    }
    s_callers.clear();
    std::vector<std::pair<std::string, u32>> v(byName.begin(), byName.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    logf("prof", "--- %llu samples over %d s (top functions, self time) ---", (unsigned long long)total, secs);
    for (size_t i = 0; i < v.size() && i < 30; i++)
        logf("prof", "%5.1f%%  %s", 100.0 * v[i].second / double(total ? total : 1), v[i].first.c_str());
}

}  // namespace

void profilerStart() {
    const char* env = getenv("R2_PROFILE");
    if (!env || !*env) return;
    const int secs = atoi(env) > 0 ? atoi(env) : 5;
    HANDLE target = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::thread([target, secs] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        std::unordered_map<u64, u32> hits;
        u64 total = 0;
        ULONGLONG next = GetTickCount64() + ULONGLONG(secs) * 1000;
        for (;;) {
            Sleep(1);
            if (SuspendThread(target) == DWORD(-1)) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const bool ok = GetThreadContext(target, &ctx);
            u64 ret = 0;
            if (ok && ctx.Rsp) ret = *reinterpret_cast<const u64*>(ctx.Rsp);  // the thread is stopped
            ResumeThread(target);  // resolve only after resuming: symbol lookup allocates
            if (ok) {
                hits[ctx.Rip]++;
                s_callers[ctx.Rip][ret]++;
                total++;
            }
            if (GetTickCount64() >= next) {
                report(hits, total, secs);
                hits.clear();
                total = 0;
                next += ULONGLONG(secs) * 1000;
            }
        }
    }).detach();
    logf("prof", "sampling every 1 ms, report every %d s", secs);
}

}  // namespace rt
