// Guest address space: one 4 GiB host reservation, guest address a at g_base + a
// (MEM1 twice, the locked cache; the VM region is translated, see vm.cpp).
//
// Save states keep the host stacks of the guest's threads (threads.cpp), whose recompiled
// frames hold host pointers: return addresses into the exe, the guest contexts, the stacks
// themselves. So the exe is linked at a fixed base (no ASLR), and guest memory and the fiber
// stacks go to fixed addresses too: a state then loads in any later session of the same build.
#include <windows.h>

#include <atomic>
#include <string>

#include <xxhash.h>

#include "runtime.h"
#include "savestate.h"

namespace rt {

u8* g_base = nullptr;
static HANDLE s_mem1 = nullptr;
static u8* s_stacks = nullptr;

constexpr u64 kFixedGuestBase = 0x0000020000000000ull;  // 2 TiB: free in practice
constexpr u64 kFixedStacks = 0x0000021000000000ull;

// R2_FAULT_DUMP="<spec>[;<spec>...]": on a guest access fault, log guest memory reached from the
// registers. A spec is r<n> followed by steps, +<hex> (add) or * (load the word there), and
// :<hex length>, e.g. "r22:200;r22+130*0*:100" (the object in r22, and what the first entry of
// its pointer array at +0x130 points to). Registers are as the context last stored them.
static void faultDump(const Ctx& c) {
    const char* spec = getenv("R2_FAULT_DUMP");
    if (!spec) return;
    auto readable = [](u32 a) { return a >= 0x80000000u && a < 0x80000000u + kMem1Size; };
    for (const char* p = spec; *p;) {
        if (*p != 'r') break;
        char* e = nullptr;
        const int reg = int(strtol(p + 1, &e, 10));
        p = e;
        u32 a = reg >= 0 && reg < 32 ? c.r[reg] : 0;
        bool ok = true;
        std::string path = "r" + std::to_string(reg);
        while (*p == '+' || *p == '*') {
            if (*p == '+') {
                a += u32(strtoul(p + 1, &e, 16));
                p = e;
            } else {
                p++;
                if (!readable(a)) {
                    ok = false;
                    break;
                }
                a = __builtin_bswap32(*reinterpret_cast<const u32*>(g_base + a));
            }
        }
        u32 len = 0x40;
        if (*p == ':') {
            len = u32(strtoul(p + 1, &e, 16));
            p = e;
        }
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
        if (!ok || !readable(a)) {
            logf("fault", "dump: %08X not readable", a);
            continue;
        }
        for (u32 off = 0; off < len && readable(a + off + 15); off += 16) {
            const u8* m = g_base + a + off;
            logf("fault", "%08X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X", a + off, m[0], m[1],
                 m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
    }
}

static DWORD s_guestThread = 0;  // the host thread the guest's fibers run on

// The SDK's BATs map 0x80000000-0x8FFFFFFF (cached) and 0xC0000000-0xCFFFFFFF (uncached) to
// physical 0-0x0FFFFFFF, but MEM1 ends at 24 MiB: on hardware an access past it does not fault
// (reads give nothing useful, Dolphin returns 0). Battle of Hoth reads there: a per-part loop
// (0x7FCF5418, block 0x7FCF58C8) indexes its 20-byte part records with r31, which only its two
// inner loops set, so the paths that jump straight to the block use the caller's r31 (a stack
// address): base + 0x800ABFB4 * 20, 14 MiB past the records (0x8230xxxx), about 30 s into the
// mission. Such pages get committed (zero-filled) when first touched and the access goes on.
static bool pastMem1(u32 guest) {
    return (guest >= 0x80000000u + kMem1Size && guest < 0x90000000u) || (guest >= 0xC0000000u + kMem1Size && guest < 0xC8000000u);
}

static bool commitPastMem1(u32 guest, bool write) {
    u8* page = g_base + (guest & ~0xFFFFu);
    if (!VirtualAlloc(page, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
        MEMORY_BASIC_INFORMATION mi{};  // another thread may have committed it meanwhile
        if (!VirtualQuery(g_base + guest, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    }
    static std::atomic<int> n{0};
    if (n++ < 8) {
        const bool guestThread = GetCurrentThreadId() == s_guestThread;
        logf("mem", "%s %08X, past the end of MEM1 (as on hardware: zeros, no fault)%s%s", write ? "write to" : "read of", guest,
             guestThread ? " in " : " by a host thread", guestThread ? describeAddr(curCtx().curFn).c_str() : "");
    }
    return true;
}

static LONG CALLBACK onAccessViolation(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_base) return EXCEPTION_CONTINUE_SEARCH;
    const u64 host = ep->ExceptionRecord->ExceptionInformation[1];
    const u64 b = u64(g_base);
    if (host >= b && host < b + 0x100000000ull) {
        const u32 guest = u32(host - b);
        const bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        if (pastMem1(guest) && commitPastMem1(guest, write)) return EXCEPTION_CONTINUE_EXECUTION;
        // Another host thread (Aurora's GX worker reading vertex arrays and textures, audio):
        // the guest's registers say nothing about it, its own call stack does.
        if (s_guestThread && GetCurrentThreadId() != s_guestThread) {
            logf("fault", "host thread %lu %s guest address %08X at %p", GetCurrentThreadId(), write ? "wrote" : "read",
                 guest, ep->ExceptionRecord->ExceptionAddress);
            printHostStack(stderr, 0);
            fatal("host %s of unmapped guest address %08X (not the guest's thread)", write ? "write" : "read", guest);
        }
        const Ctx& c = curCtx();
        faultDump(c);
        if (getenv("R2_FAULT_DUMP")) printHostStack(stderr, 0);
        char regs[600];
        int n = 0;
        for (int i = 0; i < 32; i++) n += snprintf(regs + n, sizeof regs - n, "%sr%d=%08X", i % 8 ? " " : "\n  ", i, c.r[i]);
        snprintf(regs + n, sizeof regs - n, "\n  lr=%08X ctr=%08X", c.lr, c.ctr);
        fatal("guest %s of unmapped address %08X (current fn %s)%s", write ? "write" : "read", guest,
              describeAddr(curCtx().curFn).c_str(), regs);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool mapAt(u8* base) {
    void* a = MapViewOfFileEx(s_mem1, FILE_MAP_ALL_ACCESS, 0, 0, kMem1Size, base + 0x80000000u);
    void* b = MapViewOfFileEx(s_mem1, FILE_MAP_ALL_ACCESS, 0, 0, kMem1Size, base + 0xC0000000u);
    // (0x7E000000-0x7FFFFFFF is translated by vm.cpp and never touched at g_base + a.)
    void* l = VirtualAlloc(base + kLockedCache, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* p = VirtualAlloc(base + kPortArena, kPortArenaSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (a && b && l && p) {
        g_base = base;
        return true;
    }
    if (a) UnmapViewOfFile(a);
    if (b) UnmapViewOfFile(b);
    if (l) VirtualFree(l, 0, MEM_RELEASE);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    return false;
}

bool memInit() {
    s_mem1 = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kMem1Size, nullptr);
    if (!s_mem1) return false;
    bool ok = mapAt(reinterpret_cast<u8*>(kFixedGuestBase));
    for (int attempt = 0; !ok && attempt < 32; attempt++) {
        u8* base = static_cast<u8*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
        if (!base) return false;
        VirtualFree(base, 0, MEM_RELEASE);
        ok = mapAt(base);
    }
    if (!ok) return false;
    // the guest threads' stacks (committed per fiber, threads.cpp)
    const u64 stacks = u64(kMaxFibers) * kFiberStackSize;
    s_stacks = static_cast<u8*>(VirtualAlloc(reinterpret_cast<void*>(kFixedStacks), stacks, MEM_RESERVE, PAGE_NOACCESS));
    if (!s_stacks) s_stacks = static_cast<u8*>(VirtualAlloc(nullptr, stacks, MEM_RESERVE, PAGE_NOACCESS));
    if (!s_stacks) return false;
    if (u64(g_base) != kFixedGuestBase || u64(s_stacks) != kFixedStacks)
        logf("mem", "guest memory at %p, stacks at %p (not the fixed addresses: save states of other sessions will not load)",
             g_base, s_stacks);
    s_guestThread = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, onAccessViolation);
    return true;
}

u8* fiberStackRegion() { return s_stacks; }

u8* physPtr(u32 phys, u32 len) {
    phys &= 0x3FFFFFFF;
    if (phys >= kMem1Size || phys + len > kMem1Size) return nullptr;
    return g_base + 0x80000000u + phys;
}

// The exe's identity: its code. A state holds return addresses into it, so it loads only into
// the build that saved it.
HostLayout hostLayout() {
    static u64 hash = [] {
        const u8* img = reinterpret_cast<const u8*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(img + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        u64 h = 0;
        for (u32 i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE)
                h ^= XXH3_64bits_withSeed(img + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, i);
        return h ^ u64(nt->OptionalHeader.SizeOfImage);
    }();
    return {u64(GetModuleHandleW(nullptr)), hash, u64(g_base), u64(s_stacks)};
}

void memSaveState(state::Writer& w) {
    w.section("MEM1");
    w.bytes(g_base + 0x80000000u, kMem1Size);
    w.bytes(g_base + kLockedCache, kLockedCacheSize);
    w.bytes(g_base + kPortArena, kPortArenaSize);
}

void memLoadState(state::Reader& r) {
    r.section("MEM1");
    r.bytes(g_base + 0x80000000u, kMem1Size);
    r.bytes(g_base + kLockedCache, kLockedCacheSize);
    r.bytes(g_base + kPortArena, kPortArenaSize);
}

}  // namespace rt
