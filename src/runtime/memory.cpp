// Guest address space: one 4 GiB host reservation, guest address a at g_base + a
// (MEM1 twice, the locked cache; the VM region is translated, see vm.cpp).
#include <windows.h>

#include "runtime.h"

namespace rt {

u8* g_base = nullptr;
static HANDLE s_mem1 = nullptr;

static LONG CALLBACK onAccessViolation(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_base) return EXCEPTION_CONTINUE_SEARCH;
    const u64 host = ep->ExceptionRecord->ExceptionInformation[1];
    const u64 b = u64(g_base);
    if (host >= b && host < b + 0x100000000ull) {
        const u32 guest = u32(host - b);
        const bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        fatal("guest %s of unmapped address %08X (current fn %s)", write ? "write" : "read", guest,
              describeAddr(curCtx().curFn).c_str());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool memInit() {
    s_mem1 = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kMem1Size, nullptr);
    if (!s_mem1) return false;
    for (int attempt = 0; attempt < 32; attempt++) {
        u8* base = static_cast<u8*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
        if (!base) return false;
        VirtualFree(base, 0, MEM_RELEASE);
        void* a = MapViewOfFileEx(s_mem1, FILE_MAP_ALL_ACCESS, 0, 0, kMem1Size, base + 0x80000000u);
        void* b = MapViewOfFileEx(s_mem1, FILE_MAP_ALL_ACCESS, 0, 0, kMem1Size, base + 0xC0000000u);
        // (0x7E000000-0x7FFFFFFF is translated by vm.cpp and never touched at g_base + a.)
        void* l = VirtualAlloc(base + kLockedCache, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (a && b && l) {
            g_base = base;
            AddVectoredExceptionHandler(1, onAccessViolation);
            return true;
        }
        if (a) UnmapViewOfFile(a);
        if (b) UnmapViewOfFile(b);
        if (l) VirtualFree(l, 0, MEM_RELEASE);
    }
    return false;
}

u8* physPtr(u32 phys, u32 len) {
    phys &= 0x3FFFFFFF;
    if (phys >= kMem1Size || phys + len > kMem1Size) return nullptr;
    return g_base + 0x80000000u + phys;
}

}  // namespace rt
