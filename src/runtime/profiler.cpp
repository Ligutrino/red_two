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
        // no symbols (a system or driver DLL): its module and offset
        char b[MAX_PATH + 32];
        HMODULE mod = nullptr;
        char path[MAX_PATH] = "";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &mod) &&
            GetModuleFileNameA(mod, path, sizeof path)) {
            const char* base = strrchr(path, '\\');
            snprintf(b, sizeof b, "%s+%llx", base ? base + 1 : path, (unsigned long long)(rip - u64(mod)));
        } else {
            snprintf(b, sizeof b, "?%llx", (unsigned long long)rip);
        }
        s.name = b;
    }
    return byRip.emplace(rip, std::move(s)).first->second;
}

// Library leaf functions are attributed to their caller too (the return address on the stack
// at the sample), e.g. "memmove <- rt::aramDma".
bool isLeafHelper(const std::string& n) {
    return n == "memmove" || n == "memcpy" || n == "memset" || n == "memcmp" || n.rfind("XXH", 0) == 0 ||
           n.rfind("NtWait", 0) == 0 || n.rfind("ZwWait", 0) == 0 || n.rfind("WaitFor", 0) == 0 ||
           n.rfind("SleepConditionVariable", 0) == 0 || n.rfind("RtlSleepConditionVariable", 0) == 0 ||
           n.find("SRWLock") != std::string::npos || n.rfind("_NLG", 0) == 0 || n.rfind("std::_Atomic", 0) == 0 ||
           n == "strchr" || n == "RtlRaiseStatus" || n.rfind("NtAlert", 0) == 0 ||
           // system calls (device I/O, registry, files): who made them
           n.rfind("Nt", 0) == 0 || n.rfind("Zw", 0) == 0 || n == "DeviceIoControl";
}

std::unordered_map<u64, std::unordered_map<u64, u32>> s_callers;  // rip -> return address -> hits
struct Chain {
    u64 f[6];
};
std::unordered_map<u64, std::vector<Chain>> s_chains;  // rip -> caller chains (helpers only)

void report(const std::unordered_map<u64, u32>& hits, u64 total, int secs) {
    std::unordered_map<std::string, u32> byName;
    for (auto& [rip, n] : hits) {
        const std::string& name = resolve(rip).name;
        if (isLeafHelper(name)) {
            if (auto it = s_chains.find(rip); it != s_chains.end()) {
                for (const Chain& ch : it->second) {
                    std::string label = name;
                    int shown = 0;
                    for (u64 f : ch.f) {
                        if (!f) break;
                        const std::string& fn = resolve(f).name;
                        if (isLeafHelper(fn)) continue;
                        label += " <- " + fn;
                        if (++shown == 4) break;
                    }
                    byName[label]++;
                }
                continue;
            }
        }
        byName[name] += n;
    }
    s_callers.clear();
    s_chains.clear();
    // where the time goes, roughly: guest code, guest memory/FP helpers, graphics, waiting
    {
        u32 cat[6] = {};  // game, memory, graphics (bridge), wait, other, graphics (Aurora)
        for (auto& [name, n] : byName) {
            const auto has = [&name](const char* p) { return name.find(p) != std::string::npos; };
            int c = 4;
            if (name.rfind("NtWait", 0) == 0 || name.rfind("ZwWait", 0) == 0 || name.rfind("WaitFor", 0) == 0 || has("idleWait")) c = 3;
            else if (name.rfind("rt::f_", 0) == 0) c = 0;
            else if (has("aurora::") || has("dawn") || has("wgpu")) c = 5;
            else if (has("gfx::") || has("gxFifo") || has("rt::gx") || name.rfind("XXH", 0) == 0 || has("cmdLength") || has("cpRun"))
                c = 2;
            else if (name.rfind("rt::rd", 0) == 0 || name.rfind("rt::wr", 0) == 0 || name.rfind("rt::psq", 0) == 0 || has("noteAccess") ||
                     has("tlb") || has("Tlb") || has("vm"))
                c = 1;
            cat[c] += n;
        }
        const double t = double(total ? total : 1) / 100.0;
        logf("prof", "game code %.1f%%, memory/psq %.1f%%, graphics bridge %.1f%% + Aurora %.1f%%, waiting %.1f%%, other %.1f%%",
             cat[0] / t, cat[1] / t, cat[2] / t, cat[5] / t, cat[3] / t, cat[4] / t);
    }
    std::vector<std::pair<std::string, u32>> v(byName.begin(), byName.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    logf("prof", "--- %llu samples over %d s (top functions, self time) ---", (unsigned long long)total, secs);
    for (size_t i = 0; i < v.size() && i < 40; i++)
        logf("prof", "%5.1f%%  %s", 100.0 * v[i].second / double(total ? total : 1), v[i].first.c_str());
}

// Up to 6 return addresses from a stopped thread's context. A bad frame (code without unwind
// data, a stack that does not look like one) ends the walk instead of crashing.
__declspec(noinline) void unwind(const CONTEXT& start, u64* chain) {
    CONTEXT uc = start;
    __try {
        for (int d = 0; d < 6; d++) {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(uc.Rip, &imageBase, nullptr);
            if (!fe) {  // leaf: the return address is on top of the stack
                uc.Rip = *reinterpret_cast<const DWORD64*>(uc.Rsp);
                uc.Rsp += 8;
            } else {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, uc.Rip, fe, &uc, &handlerData, &establisher, nullptr);
            }
            if (!uc.Rip) break;
            chain[d] = uc.Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
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
            // Caller chain while the thread is stopped: RtlVirtualUnwind needs no allocation.
            u64 chain[6] = {};
            if (ok) unwind(ctx, chain);
            ResumeThread(target);  // resolve only after resuming: symbol lookup allocates
            if (ok) {
                hits[ctx.Rip]++;
                // the first caller outside the helper (memmove's own frames are skipped in report)
                s_callers[ctx.Rip][chain[0]]++;
                s_chains[ctx.Rip].push_back({chain[0], chain[1], chain[2], chain[3], chain[4], chain[5]});
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
