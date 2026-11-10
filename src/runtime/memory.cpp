// Guest address space: one 4 GiB host reservation, guest address a at g_base + a
// (MEM1 twice, the locked cache; the VM region is translated, see vm.cpp).
//
// Save states keep the host stacks of the guest's threads (threads.cpp), whose recompiled
// frames hold host pointers: return addresses into the exe, the guest contexts, the stacks
// themselves. So the exe is linked at a fixed base (no ASLR), and guest memory and the fiber
// stacks go to fixed addresses too: a state then loads in any later session of the same build.
#include <windows.h>

#include <xxhash.h>

#include "runtime.h"
#include "savestate.h"

namespace rt {

u8* g_base = nullptr;
static HANDLE s_mem1 = nullptr;
static u8* s_stacks = nullptr;

constexpr u64 kFixedGuestBase = 0x0000020000000000ull;  // 2 TiB: free in practice
constexpr u64 kFixedStacks = 0x0000021000000000ull;

static LONG CALLBACK onAccessViolation(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_base) return EXCEPTION_CONTINUE_SEARCH;
    const u64 host = ep->ExceptionRecord->ExceptionInformation[1];
    const u64 b = u64(g_base);
    if (host >= b && host < b + 0x100000000ull) {
        const u32 guest = u32(host - b);
        const bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        const Ctx& c = curCtx();
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
    if (a && b && l) {
        g_base = base;
        return true;
    }
    if (a) UnmapViewOfFile(a);
    if (b) UnmapViewOfFile(b);
    if (l) VirtualFree(l, 0, MEM_RELEASE);
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
}

void memLoadState(state::Reader& r) {
    r.section("MEM1");
    r.bytes(g_base + 0x80000000u, kMem1Size);
    r.bytes(g_base + kLockedCache, kLockedCacheSize);
}

}  // namespace rt
