// Software MMU for the VM region (0x7E000000-0x7FFFFFFF).
//
// Rogue Leader runs its main code and keeps its data in virtual memory backed by ARAM: it
// builds a PowerPC hashed page table (SDR1, segment registers), and its DSI handler pages
// 4 KiB frames in from ARAM on demand. We walk that page table on a TLB miss, keep the
// referenced/changed bits the game's pager relies on, and deliver page faults to the game's
// own handler (exception 2) exactly where the access happens.
#include "runtime.h"

namespace rt {

u8* g_vmRead[kVmLen >> 12];
u8* g_vmWrite[kVmLen >> 12];

namespace {
u32 s_sr[16];
u32 s_sdr1 = 0;
u64 s_faults = 0;
bool s_log = false;

inline u32 physRd32(u32 pa) {
    const u8* p = physPtr(pa, 4);
    if (!p) fatal("page table read outside MEM1 at %08X", pa);
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}
inline void physWr32(u32 pa, u32 v) {
    u8* p = physPtr(pa, 4);
    p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v);
}

struct Walk {
    bool found = false;
    u32 pteAddr = 0;  // physical address of the PTE
    u32 w1 = 0;       // second PTE word (RPN, R, C, WIMG, PP)
};

Walk walk(u32 ea) {
    Walk r;
    const u32 sr = s_sr[ea >> 28];
    if (sr & 0x80000000u) return r;  // direct-store segment: not used
    const u32 vsid = sr & 0xFFFFFF;
    const u32 pageIndex = (ea >> 12) & 0xFFFF;
    const u32 api = pageIndex >> 10;
    const u32 htabOrg = s_sdr1 & 0xFFFF0000u, htabMask = s_sdr1 & 0x1FF;
    for (u32 h = 0; h < 2; h++) {
        u32 hash = (vsid & 0x7FFFF) ^ pageIndex;
        if (h) hash = ~hash;
        const u32 pteg = htabOrg | ((((hash >> 10) & htabMask) | ((htabOrg >> 16) & ~htabMask & 0x1FF)) << 16) |
                         ((hash & 0x3FF) << 6);
        for (u32 i = 0; i < 8; i++) {
            const u32 pa = pteg + i * 8;
            const u32 w0 = physRd32(pa);
            if (!(w0 >> 31)) continue;
            if (((w0 >> 7) & 0xFFFFFF) != vsid || ((w0 >> 6) & 1) != h || (w0 & 0x3F) != api) continue;
            r.found = true;
            r.pteAddr = pa;
            r.w1 = physRd32(pa + 4);
            return r;
        }
    }
    return r;
}

bool ppAllows(u32 sr, u32 pp, bool write) {
    const bool key = (sr >> 30) & 1;  // Ks: supervisor (the GameCube never runs in user mode)
    if (!key) return write ? pp != 3 : true;
    switch (pp) {
    case 0: return false;
    case 2: return true;
    default: return !write;
    }
}
// The game patches the hardware DSI vector (0x80000300, see 0x800098EC) with a stub that
// sends faults on 0x7xxxxxxx to its own entry at 0x800097AC. That entry builds an OSContext-like
// frame on the current stack, calls the pager whose address it keeps at 0x80095D00 as
// pager(2, frame), restores every register from the frame and returns with rfi. We do the
// same around the recompiled pager. (The ISI path is 0x800099A4 / 0x80095D04 / 3; recompiled
// code never fetches instructions, so it is never taken.)
constexpr u32 kPagerPtr = 0x80095D00;
constexpr u32 kFrameSize = 716;

void pageFault(Ctx& c) {
    const u32 pager = rd32(kPagerPtr);
    if (!pager) {
        deliverException(c, 2, c.curFn);  // not installed yet: the OS reports it
        return;
    }
    const Ctx saved = c;
    const u32 sp = c.r[1] - kFrameSize;
    const u32 frame = sp + 8;
    wr32(sp, c.r[1]);  // back chain
    for (int i = 0; i < 32; i++) wr32(frame + 4 * i, c.r[i]);
    wr32(frame + 0x80, c.getcr());
    wr32(frame + 0x84, c.lr);
    wr32(frame + 0x88, c.ctr);
    wr32(frame + 0x8C, c.getxer());
    wr32(frame + 0x198, c.curFn);  // SRR0 (the recompiled code has no exact PC)
    wr32(frame + 0x19C, c.msr);
    c.srr0 = c.curFn;
    c.srr1 = c.msr;
    c.msr = (c.msr & ~0x8000u) | 0x2032;  // exception: interrupts off; IR DR FP RI
    c.r[1] = sp;
    c.r[3] = 2;  // OS_EXCEPTION_DSI
    c.r[4] = frame;
    c.lr = 0;
    dispatch(c, pager);
    // the entry's epilogue reloads everything from the frame; the pager does not edit it
    const u32 curFn = c.curFn;
    c = saved;
    c.curFn = curFn;
}
}  // namespace

void vmFlushAll() {
    memset(g_vmRead, 0, sizeof g_vmRead);
    memset(g_vmWrite, 0, sizeof g_vmWrite);
}

void vmSetSdr1(u32 v) {
    s_sdr1 = v;
    vmFlushAll();
    s_log = envFlag("R2_VM_LOG");
    if (s_log) logf("vm", "SDR1 = %08X", v);
}

void mtsr(Ctx&, u32 n, u32 v) {
    s_sr[n & 15] = v;
    vmFlushAll();
}
u32 mfsr(Ctx&, u32 n) { return s_sr[n & 15]; }

void tlbie(Ctx&, u32 ea) {
    if (isVm(ea)) {
        const u32 i = (ea - kVmStart) >> 12;
        g_vmRead[i] = nullptr;
        g_vmWrite[i] = nullptr;
    }
}

u8* vmMiss(u32 a, bool write) {
    const u32 idx = (a - kVmStart) >> 12;
    for (int attempt = 0; attempt < 8; attempt++) {
        const Walk w = walk(a);
        u32 dsisr;
        if (w.found) {
            const u32 pp = w.w1 & 3;
            if (!write || ppAllows(s_sr[a >> 28], pp, true)) {
                u32 w1 = w.w1 | 0x100;  // R
                if (write) w1 |= 0x80;   // C
                if (w1 != w.w1) physWr32(w.pteAddr + 4, w1);
                const u32 frame = w1 & 0xFFFFF000u;
                u8* host = physPtr(frame, 0x1000);
                if (!host) fatal("VM page %08X maps to frame %08X outside MEM1", a & ~0xFFFu, frame);
                g_vmRead[idx] = host;
                // writes go straight through once C is set and the page is writable
                g_vmWrite[idx] = ((w1 & 0x80) && ppAllows(s_sr[a >> 28], pp, true)) ? host : nullptr;
                return host;
            }
            dsisr = 0x08000000u | 0x02000000u;  // protection violation on a store
        } else {
            dsisr = 0x40000000u | (write ? 0x02000000u : 0);  // no translation
        }
        // Page fault: the game's DSI handler pages the frame in (or maps it writable).
        Ctx& c = curCtx();
        c.dar = a;
        c.dsisr = dsisr;
        s_faults++;
        if (s_log) logf("vm", "DSI %s %08X (dsisr %08X), fault #%llu", write ? "write" : "read", a, dsisr, (unsigned long long)s_faults);
        pageFault(c);
    }
    fatal("the page fault handler did not map %08X (%s)", a, write ? "write" : "read");
}

u32 rdSplit(u32 a, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | *vmr(a + u32(i));
    return v;
}
void wrSplit(u32 a, u32 v, int n) {
    for (int i = 0; i < n; i++) *vmw(a + u32(i)) = u8(v >> (8 * (n - 1 - i)));
}

}  // namespace rt
