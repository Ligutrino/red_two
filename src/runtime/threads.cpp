// Guest threads and exceptions on host fibers.
//
// The game's own OS (Dolphin SDK) schedules threads by saving and loading OSContext
// structures: OSSaveContext returns 0, and later OSLoadContext of that context makes it
// "return" 1 again; exceptions save the interrupted state into OSCurrentContext and a
// handler later OSLoadContext's it. Recompiled code keeps its state on the host stack, so:
//   * every guest context belongs to a host fiber (a fresh context gets a new fiber),
//   * OSSaveContext records the caller's jump buffer (RT_SAVECTX_CALL in generated code),
//   * an exception is delivered at a safepoint inside a host frame that records its own
//     jump buffer; loading that context jumps back there and the code continues,
//   * loading a context owned by another fiber switches to that fiber.
// Only one fiber runs at a time, so the guest sees a single CPU, as on the console.
#include <windows.h>

#include <unordered_map>
#include <vector>

#include "runtime.h"
#include "table.h"

namespace rt {

extern volatile u32 g_irqLine;

namespace {

// OSContext layout
constexpr u32 kGpr = 0x000, kCr = 0x080, kLr = 0x084, kCtr = 0x088, kXer = 0x08C, kFpscr = 0x194, kSrr0 = 0x198,
              kSrr1 = 0x19C, kState = 0x1A2, kGqr = 0x1A4;
constexpr u16 kStateExc = 2;
constexpr u32 kMsrEE = 0x8000;

struct IrqFrame {
    JmpBuf jb;
    u32 ctxAddr;
    u32 pc;
    Ctx snapshot;
};

struct HostFiber {
    void* handle = nullptr;
    Ctx ctx{};
    u32 resumeCtx = 0;  // set by whoever switches to this fiber
    int id = 0;
    std::vector<IrqFrame*> frames;
};

enum class Kind { Fresh, Running, Saved, Exc };
struct Rec {
    HostFiber* fiber = nullptr;
    Kind kind = Kind::Fresh;
    JmpBuf* jb = nullptr;      // Saved: caller's jump buffer
    IrqFrame* frame = nullptr; // Exc: delivering frame
};

std::unordered_map<u32, Rec> s_recs;
bool s_log = false;
struct IrqFrame* s_resumeFrame = nullptr;
HostFiber* s_cur = nullptr;
int s_nextId = 0;
u32 s_addrInitContextOrig = 0, s_addrSaveContext = 0;

u32 symAddr(const char* name) {
    for (const SymName* s = g_symNames; s->name; s++)
        if (!strcmp(s->name, name)) return s->addr;
    fatal("symbol %s missing from config/GSWE64/symbols.txt", name);
}

void loadAll(Ctx& c, u32 ctx) {
    for (int i = 0; i < 32; i++) c.r[i] = rd32(ctx + kGpr + 4 * i);
    c.mtcrf(0xFF, rd32(ctx + kCr));
    c.lr = rd32(ctx + kLr);
    c.ctr = rd32(ctx + kCtr);
    c.setxer(rd32(ctx + kXer));
    for (int i = 1; i < 8; i++) c.gqr[i] = rd32(ctx + kGqr + 4 * i);
    c.msr = rd32(ctx + kSrr1);
}

[[noreturn]] void resumeLocal(u32 ctx);

void WINAPI fiberMain(void* p) {
    HostFiber* f = static_cast<HostFiber*>(p);
    s_cur = f;
    Ctx& c = f->ctx;
    const u32 ctx = f->resumeCtx;
    loadAll(c, ctx);
    c.fpscr = rd32(ctx + kFpscr);
    c.fiber = f;
    s_recs[ctx] = Rec{f, Kind::Running, nullptr, nullptr};
    const u32 pc = rd32(ctx + kSrr0);
    logf("thread", "fiber %d starts context %08X at %s (sp=%08X)", f->id, ctx, describeAddr(pc).c_str(), c.r[1]);
    dispatch(c, pc);
    // The entry returned: continue at LR (OSExitThread), as blr would.
    dispatch(c, c.lr);
    fatal("thread context %08X ran past its exit (lr=%08X)", ctx, c.lr);
}

HostFiber* newFiber() {
    HostFiber* f = new HostFiber();
    f->id = ++s_nextId;
    f->handle = CreateFiberEx(256 * 1024, 16 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, fiberMain, f);
    if (!f->handle) fatal("CreateFiberEx failed");
    return f;
}

// Switch to fiber f and ask it to resume context ctx. Returns only by jumping elsewhere.
[[noreturn]] void switchTo(HostFiber* f, u32 ctx) {
    HostFiber* me = s_cur;
    f->resumeCtx = ctx;
    s_cur = f;
    SwitchToFiber(f->handle);
    // Someone switched back to us: resume the context they asked for.
    s_cur = me;
    resumeLocal(me->resumeCtx);
}

[[noreturn]] void resumeLocal(u32 ctx) {
    auto it = s_recs.find(ctx);
    if (it == s_recs.end() || it->second.fiber != s_cur)
        fatal("resume of context %08X on fiber %d, which does not own it", ctx, s_cur->id);
    Rec& r = it->second;
    switch (r.kind) {
    case Kind::Exc:
        s_resumeFrame = r.frame;
        rt_longjmp(&r.frame->jb, 1);
    case Kind::Saved:
        if (!r.jb) fatal("context %08X was saved without a resume point", ctx);
        s_cur->ctx.resumeCtxAddr = ctx;
        rt_longjmp(r.jb, 1);
    default:
        fatal("context %08X cannot be resumed (state %d)", ctx, int(r.kind));
    }
}

}  // namespace

Ctx& curCtx() { return s_cur->ctx; }

void threadsInit(Ctx*& boot) {
    HostFiber* f = new HostFiber();
    f->handle = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!f->handle) fatal("ConvertThreadToFiber failed");
    f->ctx.fiber = f;
    s_cur = f;
    boot = &f->ctx;
    s_log = envFlag("R2_IRQ_LOG");
    s_addrInitContextOrig = symAddr("OSInitContext");
    s_addrSaveContext = symAddr("OSSaveContext");
}

// --- HLE: the OS context primitives -----------------------------------------------------
void hle_OSSaveContext(Ctx& c) {
    const u32 ctx = c.r[3];
    GuestFn orig = lookupOrig(s_addrSaveContext);
    orig(c);  // writes the guest context exactly as the original does; r3 = 0
    s_recs[ctx] = Rec{s_cur, Kind::Saved, static_cast<JmpBuf*>(c.pendingJmp), nullptr};
    if (s_log) logf("ctx", "save %08X on fiber %d (jb %p, lr %08X)", ctx, s_cur->id, c.pendingJmp, c.lr);
    c.pendingJmp = nullptr;
}

void ctxResumed(Ctx& c) {
    // OSLoadContext of a context saved by OSSaveContext: registers come back from it
    // (r3 = 1, stored by OSSaveContext), FP state stays (lazy FPU switching).
    const u32 ctx = c.resumeCtxAddr;
    c.r[0] = rd32(ctx + kGpr);
    c.r[1] = rd32(ctx + kGpr + 4);
    c.r[2] = rd32(ctx + kGpr + 8);
    for (int i = 13; i < 32; i++) c.r[i] = rd32(ctx + kGpr + 4 * i);
    c.r[3] = rd32(ctx + kGpr + 12);
    c.r[4] = rd32(ctx + kGpr + 16);
    c.mtcrf(0xFF, rd32(ctx + kCr));
    c.lr = rd32(ctx + kLr);
    c.ctr = rd32(ctx + kCtr);
    c.setxer(rd32(ctx + kXer));
    for (int i = 1; i < 8; i++) c.gqr[i] = rd32(ctx + kGqr + 4 * i);
    c.msr = rd32(ctx + kSrr1);
    s_recs[ctx].kind = Kind::Running;
    if ((c.msr & kMsrEE) && g_irqLine) g_irqPending = 1;
}

void hle_OSLoadContext(Ctx& c) {
    const u32 ctx = c.r[3];
    const u16 st = u16(rd16(ctx + kState));
    if (st & kStateExc) wr16(ctx + kState, st & ~kStateExc);
    auto it = s_recs.find(ctx);
    if (s_log)
        logf("ctx", "load %08X from fiber %d: %s", ctx, s_cur->id,
             it == s_recs.end() ? "unknown" : it->second.kind == Kind::Fresh ? "fresh" : it->second.kind == Kind::Saved ? "saved" : it->second.kind == Kind::Exc ? "exception" : "running");
    if (it == s_recs.end() || it->second.kind == Kind::Fresh) {
        HostFiber* f = newFiber();
        switchTo(f, ctx);
    }
    if (it->second.fiber == s_cur) resumeLocal(ctx);
    switchTo(it->second.fiber, ctx);
}

void hle_OSInitContext(Ctx& c) {
    const u32 ctx = c.r[3];
    lookupOrig(s_addrInitContextOrig)(c);
    s_recs[ctx] = Rec{nullptr, Kind::Fresh, nullptr, nullptr};
}

// --- exceptions -----------------------------------------------------------------------
void deliverException(Ctx& c, u32 exc, u32 pc) {
    const u32 ctx = rd32(0x800000D4);  // OSCurrentContext
    if (!ctx) fatal("exception %u with no current context", exc);
    for (int i = 0; i < 32; i++) wr32(ctx + kGpr + 4 * i, c.r[i]);
    wr32(ctx + kCr, c.getcr());
    wr32(ctx + kLr, c.lr);
    wr32(ctx + kCtr, c.ctr);
    wr32(ctx + kXer, c.getxer());
    wr32(ctx + kSrr0, pc);
    wr32(ctx + kSrr1, c.msr);
    for (int i = 1; i < 8; i++) wr32(ctx + kGqr + 4 * i, c.gqr[i]);
    wr16(ctx + kState, rd16(ctx + kState) | kStateExc);

    if (s_log) logf("irq", "exception %u at %08X, context %08X, handler %08X, INTSR&MASK pending=%d", exc, pc, ctx, rd32(0x80003000 + exc * 4), int(piPending()));
    IrqFrame* fr = new IrqFrame();
    fr->ctxAddr = ctx;
    fr->pc = pc;
    fr->snapshot = c;
    s_recs[ctx] = Rec{s_cur, Kind::Exc, nullptr, fr};
    s_cur->frames.push_back(fr);
    if (rt_setjmp(&fr->jb) == 0) {
        const u32 handler = rd32(0x80003000 + exc * 4);
        if (!handler) fatal("exception %u at %08X with no handler installed", exc, pc);
        c.srr0 = pc;
        c.srr1 = c.msr;
        c.msr &= ~kMsrEE;
        c.r[3] = exc;
        c.r[4] = ctx;
        c.lr = 0;
        dispatch(c, handler);
        logf("irq", "exception %u handler %08X returned instead of loading a context", exc, handler);
    }
    // Back from the exception (OSLoadContext of ctx, or the handler returned).
    HostFiber* me = s_cur;
    if (s_resumeFrame) {
        fr = s_resumeFrame;
        s_resumeFrame = nullptr;
    }
    if (s_log) logf("irq", "return from exception to %08X on fiber %d", fr->pc, me->id);
    if (!me->frames.empty() && me->frames.back() == fr) me->frames.pop_back();
    Ctx& cc = me->ctx;
    void* pend = cc.pendingJmp;
    cc = fr->snapshot;
    cc.pendingJmp = pend;
    auto it = s_recs.find(fr->ctxAddr);
    if (it != s_recs.end() && it->second.frame == fr) it->second = Rec{me, Kind::Running, nullptr, nullptr};
    delete fr;
}

u64 g_safepoints = 0, g_exceptions = 0;  // R2_GX_LOG statistics

void safepoint(Ctx& c, u32 pc) {
    g_safepoints++;
    g_irqPending = 0;
    runDueEvents();
    if (!(c.msr & kMsrEE)) return;
    if (piPending()) {
        deliverException(c, 4, pc);
    } else if (decPending()) {
        decAck();
        deliverException(c, 8, pc);
    }
    // more to take once the handler is done (it runs with interrupts off)
    if (g_irqLine && (c.msr & kMsrEE)) g_irqPending = 1;
}

}  // namespace rt
