// Red Two: boots the recompiled game.
//
//   red_two.exe [--disc <GSWE64.iso>] [--trace]
//
// Environment: R2_IO_LOG=1 (every hardware register access), R2_DVD_LOG=1, R2_DSP_LOG=1,
// R2_ARAM_LOG=1, R2_TRACE=1 (record guest function entries; ring dumped on fatal errors).
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "hw/gx_fifo.h"
#include "runtime.h"
#include "table.h"

namespace rt {

// --- logging ----------------------------------------------------------------------------
static std::mutex s_logMu;
static u64 s_t0Tb = 0;

void logf(const char* tag, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lk(s_logMu);
    const double t = double(tbNow() - s_t0Tb) / double(kTbClock);
    fprintf(stderr, "[%9.3f %-6s] %s\n", t, tag, buf);
    fflush(stderr);
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\nFATAL: %s\n", buf);
    if (g_trace) {
        fprintf(stderr, "recent guest functions (oldest first):\n");
        printTraceRing(stderr, 40);
    }
    fflush(stderr);
    ExitProcess(1);
}

bool envFlag(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

const char* symName(u32 addr) {
    for (const SymName* s = g_symNames; s->name; s++)
        if (s->addr == addr) return s->name;
    return nullptr;
}

std::string describeAddr(u32 addr) {
    char buf[96];
    if (const char* n = symName(addr)) snprintf(buf, sizeof buf, "%08X (%s)", addr, n);
    else snprintf(buf, sizeof buf, "%08X", addr);
    return buf;
}

// --- disc -----------------------------------------------------------------------------
static FILE* s_disc = nullptr;
static u64 s_discSize = 0;

bool discOpen(const std::string& path) {
    s_disc = fopen(path.c_str(), "rb");
    if (!s_disc) return false;
    _fseeki64(s_disc, 0, SEEK_END);
    s_discSize = u64(_ftelli64(s_disc));
    return s_discSize > 0x440;
}

bool discRead(u64 off, void* dst, u32 len) {
    if (off + len > s_discSize) return false;
    _fseeki64(s_disc, s64(off), SEEK_SET);
    return fread(dst, 1, len, s_disc) == len;
}

static u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

struct DiscFile {
    std::string path;
    u32 off, size;
};
static std::vector<DiscFile> s_files;

static void loadFst() {
    u8 hdr[0x440];
    if (!discRead(0, hdr, sizeof hdr)) return;
    const u32 fstOff = be32(hdr + 0x424), fstSize = be32(hdr + 0x428);
    std::vector<u8> fst(fstSize);
    if (!discRead(fstOff, fst.data(), fstSize)) return;
    const u32 n = be32(&fst[8]);
    const char* names = reinterpret_cast<const char*>(&fst[n * 12]);
    std::vector<std::pair<std::string, u32>> dirs{{"", n}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().second) dirs.pop_back();
        const u8* e = &fst[i * 12];
        const std::string name = names + (be32(e) & 0xFFFFFF);
        const std::string path = dirs.back().first + name;
        if (e[0]) dirs.push_back({path + "/", be32(e + 8)});
        else s_files.push_back({path, be32(e + 4), be32(e + 8)});
    }
}

std::string discFileAt(u64 off) {
    for (auto& f : s_files)
        if (off >= f.off && off < u64(f.off) + f.size) {
            char b[32];
            snprintf(b, sizeof b, "+0x%llX", (unsigned long long)(off - f.off));
            return f.path + b;
        }
    return "";
}

// --- guest thread dump (debugging; races with the guest, reads memory without faulting) ---
static u32 peek32(u32 a) {
    u8* p;
    if (isVm(a)) {
        p = g_vmRead[(a - kVmStart) >> 12];
        if (!p) return 0;
        p += a & 0xFFF;
    } else if (isMem1(a)) {
        p = hp(a);
    } else {
        return 0;
    }
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}

void dumpThreads() {
    // OSThread: context 0x000, state u16 0x2C8, priority 0x2D0, queue 0x2DC, linkActive 0x2FC
    static const char* states[] = {"?", "ready", "running", "?", "waiting", "?", "?", "?", "moribund"};
    u32 t = peek32(0x800000DC);
    for (int n = 0; t && n < 32; n++) {
        const u32 state = peek32(t + 0x2C8) >> 16;
        std::string chain;
        u32 sp = peek32(t + 4);
        for (int d = 0; d < 10 && sp; d++) {
            sp = peek32(sp);
            const u32 lr = sp ? peek32(sp + 4) : 0;
            if (!lr) break;
            char b[16];
            snprintf(b, sizeof b, " %08X", lr);
            chain += b;
        }
        logf("thread", "%08X %-8s prio %2u waits on %08X ctx lr %08X srr0 %08X | stack:%s", t,
             state < 9 ? states[state] : "?", peek32(t + 0x2D0), peek32(t + 0x2DC), peek32(t + 0x84), peek32(t + 0x198),
             chain.c_str());
        t = peek32(t + 0x2FC);
    }
}

// --- frame hook -------------------------------------------------------------------------
static u32 s_frames = 0;
void viFrameHook() {
    s_frames++;
    if (envFlag("R2_GX_LOG") && s_frames % 60 == 0) {
        const GxStats& g = gxStats();
        logf("gx", "frame %u: fifo %llu bytes, %llu draws, %llu verts, %llu BP, %llu XF, %llu CP, %llu DL", s_frames,
             (unsigned long long)g.bytes, (unsigned long long)g.draws, (unsigned long long)g.vertices,
             (unsigned long long)g.bp, (unsigned long long)g.xf, (unsigned long long)g.cp, (unsigned long long)g.dl);
    }
}

// --- boot -------------------------------------------------------------------------------
// Load main.dol and set up low memory the way the IPL and apploader leave it.
static u32 bootDisc() {
    u8 hdr[0x440];
    if (!discRead(0, hdr, sizeof hdr)) fatal("cannot read the disc header");
    if (memcmp(hdr, "GSWE64", 6) != 0) fatal("this is not Star Wars: Rogue Leader (USA): game ID %.6s", (const char*)hdr);
    if (hdr[7] != 0) fatal("unsupported disc revision %u (rev 0 is supported)", hdr[7]);
    const u32 dolOff = be32(hdr + 0x420), fstOff = be32(hdr + 0x424), fstSize = be32(hdr + 0x428),
              fstMax = be32(hdr + 0x42C);

    u8 dh[0x100];
    discRead(dolOff, dh, sizeof dh);
    for (int i = 0; i < 18; i++) {
        const u32 off = be32(dh + 4 * i), addr = be32(dh + 0x48 + 4 * i), size = be32(dh + 0x90 + 4 * i);
        if (!size) continue;
        if (!discRead(dolOff + off, hp(addr), size)) fatal("cannot read DOL section %d", i);
    }
    const u32 bss = be32(dh + 0xD8), bssSize = be32(dh + 0xDC), entry = be32(dh + 0xE0);
    // bss is cleared by __init_data, but the sdata sections inside it were just loaded
    (void)bss;
    (void)bssSize;

    // FST at the top of MEM1, as the apploader puts it
    const u32 fstAddr = (0x81800000u - fstMax) & ~0x1Fu;
    if (!discRead(fstOff, hp(fstAddr), fstSize)) fatal("cannot read the FST");
    const u32 bi2Addr = (fstAddr - 0x2000) & ~0x1Fu;
    discRead(0x440, hp(bi2Addr), 0x2000);

    memcpy(hp(0x80000000), hdr, 0x20);  // disc ID
    wr32(0x80000020, 0x0D15EA5E);       // boot magic
    wr32(0x80000024, 1);
    wr32(0x80000028, kMem1Size);        // physical memory size
    wr32(0x8000002C, 3);                // console type: retail
    wr32(0x80000030, 0);                // arena low: from the linker symbol
    wr32(0x80000034, bi2Addr);          // arena high: below bi2/FST
    wr32(0x80000038, fstAddr);
    wr32(0x8000003C, fstMax);
    wr32(0x800000CC, 0);                // video mode: NTSC
    wr32(0x800000D0, 0x01000000);       // ARAM size
    wr32(0x800000EC, 0x81800000);       // debug monitor top (none)
    wr32(0x800000F0, kMem1Size);        // simulated memory size
    wr32(0x800000F4, bi2Addr);
    wr32(0x800000F8, u32(kBusClock));
    wr32(0x800000FC, u32(kCpuClock));
    logf("boot", "DOL entry %08X, FST at %08X, arena high %08X", entry, fstAddr, bi2Addr);
    return entry;
}

}  // namespace rt

int main(int argc, char** argv) {
    using namespace rt;
    std::string disc = "orig/GSWE64.iso";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--trace")) g_trace = true;
    }
    if (envFlag("R2_TRACE")) g_trace = true;
    debugInit();
    if (const char* w = getenv("R2_WATCHDOG")) {
        const int secs = atoi(w) > 0 ? atoi(w) : 2;
        g_trace = true;
        std::thread([secs] {
            for (;;) {
                Sleep(DWORD(secs * 1000));
                logf("watch", "%s", traceRing(12).c_str());
                dumpThreads();
            }
        }).detach();
    }
    eventsInit();
    s_t0Tb = tbNow();
    if (!memInit()) fatal("could not reserve the guest address space");
    tableInit();
    if (!discOpen(disc)) fatal("cannot open the disc image %s", disc.c_str());
    loadFst();
    Ctx* boot = nullptr;
    threadsInit(boot);
    Ctx& c = *boot;
    const u32 entry = bootDisc();
    hwInit();
    c.msr = 0x2032;  // FP, IR, DR, RI; interrupts off until the OS enables them
    logf("boot", "%u recompiled functions; jumping to %s", g_funcCount, describeAddr(entry).c_str());
    dispatch(c, entry);
    logf("boot", "the game returned from its entry point");
    return 0;
}
