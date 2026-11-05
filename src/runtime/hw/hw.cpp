// GameCube hardware registers (0xCC000000) as the game's SDK code drives them.
//
// The whole SDK runs recompiled; devices are modelled at the register level, written from
// the public descriptions of the hardware (YAGCD and the SDK's own register usage).
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <mutex>
#include <set>
#include <filesystem>
#include <string>
#include <vector>

#include "../runtime.h"
#include "gx_fifo.h"
#include "musyx_dsp.h"

namespace rt {

extern void (*g_eventHandlers[EV_COUNT])();
extern volatile u32 g_irqLine;

u64 g_viLateMaxTb = 0, g_viLateSumTb = 0;  // how late retraces fire (R2_GX_LOG reports, resets)

namespace {

void siPollAll();

bool s_logIo = false;
std::set<u32> s_seen;
void noteAccess(const char* dev, u32 a, bool write, u32 v) {
    if (s_logIo) {
        logf("io", "%s %s %08X = %08X", dev, write ? "W" : "R", a, v);
        return;
    }
    if (s_seen.insert((a & ~1u) | (write ? 1u : 0u)).second)
        logf("io", "first %s of %s reg %08X (%08X)", write ? "write" : "read", dev, a, v);
}

// =======================================================================================
// PI: interrupt controller, CPU FIFO pointers
// =======================================================================================
u32 pi_intsr = 0, pi_intmr = 0;

void piUpdate() {
    if (pi_intsr & pi_intmr) {
        g_irqLine |= 1;
        g_irqPending = 1;
    } else {
        g_irqLine &= ~1u;
    }
}

u32 piRead(u32 off) {
    switch (off) {
    case 0x00: return pi_intsr | 0x00010000;  // bit 16: reset switch released
    case 0x04: return pi_intmr;
    case 0x0C: case 0x10: case 0x14: return gxPiRead(off);
    case 0x2C: return 0x246500B1;  // Flipper revision
    default: return 0;
    }
}
void piWrite(u32 off, u32 v) {
    switch (off) {
    case 0x00: pi_intsr &= ~(v & (PI_ERROR | PI_RSW)); piUpdate(); break;
    case 0x04: pi_intmr = v; piUpdate(); break;
    case 0x0C: case 0x10: case 0x14: case 0x18: gxPiWrite(off, v); break;
    case 0x24:
        logf("pi", "reset code write %08X (the game asked for a reset)", v);
        break;
    default: break;
    }
}

// =======================================================================================
// VI: video interface (timing only; the picture comes from EFB copies)
// =======================================================================================
u16 vi[0x40] = {};
u64 s_viFrameStartTb = 0;
u32 s_viFrames = 0;
constexpr u64 kViPeriod = kTbClock * 1001 / 60000;  // 59.94 Hz

void viUpdateIrq() {
    bool any = false;
    for (int n = 0; n < 4; n++) {
        const u16 hi = vi[(0x30 + 4 * n) / 2];
        if ((hi & 0x8000) && (hi & 0x1000)) any = true;
    }
    piSetIrq(PI_VI, any);
}

void onVi() {
    const u64 now = tbNow(), due = s_viFrameStartTb + kViPeriod;
    if (now > due) {
        g_viLateSumTb += now - due;
        if (now - due > g_viLateMaxTb) g_viLateMaxTb = now - due;
    }
    s_viFrameStartTb = now;
    s_viFrames++;
    for (int n = 0; n < 2; n++) {
        u16& hi = vi[(0x30 + 4 * n) / 2];
        if (hi & 0x1000) hi |= 0x8000;
    }
    viUpdateIrq();
    siPollAll();
    viFrameHook();
    schedule(EV_VI, s_viFrameStartTb + kViPeriod);
}

u16 viRead(u32 off) {
    if (off == 0x2C) {  // VCOUNT
        const u64 ph = tbNow() - s_viFrameStartTb;
        return u16(1 + (ph * 263 / kViPeriod) % 263);
    }
    if (off == 0x2E) return 1;
    if (off == 0x6E) return 0;  // VISEL: no component cable
    return vi[(off & 0x7F) / 2];
}
// Frame buffers the VI has been told to scan out (TFBL 0x1C, BFBL 0x24). The renderer
// presents only display copies into one of these: the game also clears EFB bands with
// display copies into a scratch buffer the VI never shows.
std::vector<u32> s_viFbs;
u32 s_viTop[2] = {};  // the two most recent distinct top-field frame buffers (front and back)

void viNoteFb(u32 hiOff) {
    const u32 v = (u32(vi[hiOff / 2]) << 16) | vi[hiOff / 2 + 1];
    const u32 addr = (v & 0x10000000) ? (v & 0x00FFFFFF) << 5 : v & 0x00FFFFFF;  // POFF: address >> 5
    if (!addr) return;
    if (hiOff == 0x1C && addr != s_viTop[0]) {
        s_viTop[1] = s_viTop[0];
        s_viTop[0] = addr;
    }
    if (std::find(s_viFbs.begin(), s_viFbs.end(), addr) != s_viFbs.end()) return;
    if (s_viFbs.size() >= 32) s_viFbs.erase(s_viFbs.begin());
    s_viFbs.push_back(addr);
    logf("vi", "frame buffer %08X", addr);
}


void viWrite(u32 off, u16 v) {
    vi[(off & 0x7F) / 2] = v;
    if (off >= 0x30 && off < 0x40) viUpdateIrq();
    // The 32-bit frame buffer registers arrive as two 16-bit writes (high, then low): take
    // the address once the low half is in, so no half-updated mix is recorded.
    if (off == 0x1E) viNoteFb(0x1C);
    else if (off == 0x26) viNoteFb(0x24);
}

// =======================================================================================
// DI: DVD interface
// =======================================================================================
u32 di[0x10] = {};
enum { DISR = 0, DICVR, DICMD0, DICMD1, DICMD2, DIMAR, DILEN, DICR, DIIMM, DICFG };
u32 s_diPendingErr = 0;

void diUpdateIrq() {
    const u32 sr = di[DISR];
    const bool on = ((sr & 0x04) && (sr & 0x02)) || ((sr & 0x10) && (sr & 0x08)) || ((sr & 0x40) && (sr & 0x20)) ||
                    ((di[DICVR] & 0x04) && (di[DICVR] & 0x02));
    piSetIrq(PI_DI, on);
}

struct DiOp {
    bool active = false;
    u32 cmd0 = 0, off = 0, len = 0, mar = 0;
} s_diOp;

void onDi() {
    if (!s_diOp.active) return;
    const u32 cmd = s_diOp.cmd0 >> 24;
    if (cmd == 0xA8) {
        u8* dst = physPtr(s_diOp.mar, s_diOp.len);
        if (!dst) fatal("DVD read to bad address %08X len %X", s_diOp.mar, s_diOp.len);
        const u64 off = (s_diOp.cmd0 & 0xC0) == 0x40 ? 0 : u64(s_diOp.off) << 2;
        if (!discRead(off, dst, s_diOp.len)) fatal("DVD read past the disc: %llX + %X", (unsigned long long)off, s_diOp.len);
        watchHost(s_diOp.mar, s_diOp.len, "DVD read");
        di[DIMAR] = s_diOp.mar + s_diOp.len;
    }
    s_diOp.active = false;
    di[DILEN] = 0;
    di[DICR] &= ~1u;
    di[DISR] |= 0x10;  // TCINT
    diUpdateIrq();
}

void diStart() {
    const u32 cmd0 = di[DICMD0];
    const u32 cmd = cmd0 >> 24;
    s_diOp = {true, cmd0, di[DICMD1], di[DILEN], di[DIMAR]};
    u64 latencyTb = kTbClock / 2000;  // 0.5 ms command overhead
    switch (cmd) {
    case 0xA8:
        if ((cmd0 & 0xC0) == 0x40) s_diOp.len = 0x20;  // read disc ID
        latencyTb += u64(s_diOp.len) * kTbClock / (3 * 1024 * 1024);  // ~3 MB/s
        if (envFlag("R2_DVD_LOG")) {
            const u64 o = u64(s_diOp.off) << 2;
            logf("dvd", "read %08llX len %X -> %08X  %s", (unsigned long long)o, s_diOp.len, s_diOp.mar, discFileAt(o).c_str());
        }
        break;
    case 0xAB: break;  // seek
    case 0x12: {       // inquiry
        u8* dst = physPtr(di[DIMAR], 0x20);
        if (dst) {
            memset(dst, 0, 0x20);
            dst[2] = 0x00; dst[3] = 0x02;  // revision
            dst[4] = 0x20; dst[5] = 0x01; dst[6] = 0x06; dst[7] = 0x08;  // date
            dst[8] = 0x61;
        }
        break;
    }
    case 0xE0: di[DIIMM] = s_diPendingErr; break;  // request error
    case 0xE1: case 0xE2: case 0xE4: di[DIIMM] = 0; break;  // audio streaming: not used
    case 0xE3: break;  // stop motor
    default: logf("dvd", "unknown command %08X %08X %08X", cmd0, di[DICMD1], di[DICMD2]); break;
    }
    schedule(EV_DI, tbNow() + latencyTb);
}

u32 diRead(u32 off) { return di[(off & 0x3F) / 4]; }
void diWrite(u32 off, u32 v) {
    const u32 r = (off & 0x3F) / 4;
    switch (r) {
    case DISR: {
        const u32 ack = v & 0x54;
        di[DISR] = (di[DISR] & ~0x2Au) | (v & 0x2A);
        di[DISR] &= ~ack;
        diUpdateIrq();
        break;
    }
    case DICVR: {
        di[DICVR] = (di[DICVR] & ~0x02u) | (v & 0x02);
        if (v & 0x04) di[DICVR] &= ~0x04u;
        diUpdateIrq();
        break;
    }
    case DICR:
        di[DICR] = v;
        if (v & 1) diStart();
        break;
    default: di[r] = v; break;
    }
}

// =======================================================================================
// SI: serial interface (controllers)
// =======================================================================================
u32 si[0x40] = {};  // 0x00..0xFF (32-bit regs incl. the 128-byte I/O buffer at 0x80)
constexpr u32 SIPOLL = 0x30 / 4, SICOMCSR = 0x34 / 4, SISR = 0x38 / 4;
PadState s_pad[4];

void siUpdateIrq() {
    const u32 cs = si[SICOMCSR];
    const bool on = ((cs & 0x80000000u) && (cs & 0x40000000u)) || ((cs & 0x10000000u) && (cs & 0x08000000u));
    piSetIrq(PI_SI, on);
}

void siPollResponse(int ch, u32& hi, u32& lo) {
    const PadState& p = s_pad[ch];
    hi = (u32(p.buttons) << 16) | (u32(p.stickX) << 8) | p.stickY;
    lo = (u32(p.substickX) << 24) | (u32(p.substickY) << 16) | (u32(p.triggerL) << 8) | p.triggerR;
}

void siPollAll() {
    const u32 poll = si[SIPOLL];
    for (int ch = 0; ch < 4; ch++) {
        if (!(poll & (0x80u >> ch))) continue;
        u32 hi, lo;
        if (!s_pad[ch].connected) {
            si[SISR] |= 0x08u << (8 * (3 - ch));  // NOREP
            continue;
        }
        siPollResponse(ch, hi, lo);
        si[ch * 3 + 1] = hi;
        si[ch * 3 + 2] = lo;
        si[SISR] |= 0x20u << (8 * (3 - ch));  // RDST
    }
    if (si[SISR] & 0x20202020u) {
        si[SICOMCSR] |= 0x10000000u;  // RDSTINT
        siUpdateIrq();
    }
}

void siTransfer() {
    const u32 cs = si[SICOMCSR];
    const int ch = (cs >> 1) & 3;
    u32 outLen = (cs >> 16) & 0x7F, inLen = (cs >> 8) & 0x7F;
    if (!outLen) outLen = 128;
    if (!inLen) inLen = 128;
    u8 buf[128];
    for (int i = 0; i < 32; i++) {
        const u32 w = si[0x20 + i];
        buf[i * 4] = u8(w >> 24); buf[i * 4 + 1] = u8(w >> 16); buf[i * 4 + 2] = u8(w >> 8); buf[i * 4 + 3] = u8(w);
    }
    u8 resp[128] = {};
    bool ok = s_pad[ch].connected;
    if (ok) {
        const PadState& p = s_pad[ch];
        switch (buf[0]) {
        case 0x00: case 0xFF:  // reset / identify: standard controller
            resp[0] = 0x09; resp[1] = 0x00; resp[2] = 0x00;
            break;
        case 0x40: {
            u32 hi, lo;
            siPollResponse(ch, hi, lo);
            for (int i = 0; i < 4; i++) { resp[i] = u8(hi >> (24 - 8 * i)); resp[4 + i] = u8(lo >> (24 - 8 * i)); }
            break;
        }
        case 0x41: case 0x42:  // origin / recalibrate
            resp[0] = 0x00; resp[1] = 0x80;
            resp[2] = 0x80; resp[3] = 0x80; resp[4] = 0x80; resp[5] = 0x80;
            resp[6] = p.triggerL; resp[7] = p.triggerR; resp[8] = 0; resp[9] = 0;
            break;
        default:
            logf("si", "unknown SI command %02X on port %d", buf[0], ch);
            ok = false;
            break;
        }
    }
    for (u32 i = 0; i < 32; i++)
        si[0x20 + i] = (u32(resp[i * 4]) << 24) | (u32(resp[i * 4 + 1]) << 16) | (u32(resp[i * 4 + 2]) << 8) | resp[i * 4 + 3];
    si[SICOMCSR] &= ~1u;
    si[SICOMCSR] |= 0x80000000u;  // TCINT
    if (!ok) {
        si[SICOMCSR] |= 0x20000000u;           // COMERR
        si[SISR] |= 0x08u << (8 * (3 - ch));   // NOREP
    }
    (void)inLen;
    siUpdateIrq();
}

u32 siRead(u32 off) {
    const u32 r = (off & 0xFF) / 4;
    const u32 v = si[r];
    if (r < 12 && (r % 3) == 1) si[SISR] &= ~(0x20u << (8 * (3 - r / 3)));  // reading INBUFH clears RDST
    return v;
}
void siWrite(u32 off, u32 v) {
    const u32 r = (off & 0xFF) / 4;
    switch (r) {
    case SICOMCSR: {
        u32 cs = si[SICOMCSR];
        if (v & 0x80000000u) cs &= ~0x80000000u;  // ack TCINT
        if (v & 0x10000000u) cs &= ~0x10000000u;  // ack RDSTINT
        const u32 keep = 0x80000000u | 0x10000000u | 0x20000000u;
        cs = (cs & keep) | (v & ~keep);
        si[SICOMCSR] = cs;
        if (v & 1) siTransfer();
        siUpdateIrq();
        break;
    }
    case SISR:
        if (v & 0x80000000u) {  // WR: copy out buffers (polling commands) - nothing to do
        }
        si[SISR] &= ~(v & 0x0F0F0F0Fu);  // clear error bits written as 1
        break;
    case SIPOLL:
        si[SIPOLL] = v;
        break;
    default: si[r] = v; break;
    }
}

// =======================================================================================
// EXI: memory card in slot A, IPL ROM / RTC / SRAM
// =======================================================================================
struct ExiChan {
    u32 csr = 0, mar = 0, len = 0, cr = 0, data = 0;
} exi[3];
u8 s_sram[64];
struct IplDev {
    bool haveCmd = false;
    u32 cmd = 0;
    u32 pos = 0;
} s_ipl;

void exiUpdateIrq() {
    bool on = false;
    for (auto& c : exi)
        if (((c.csr & 0x002) && (c.csr & 0x001)) || ((c.csr & 0x008) && (c.csr & 0x004)) || ((c.csr & 0x800) && (c.csr & 0x400)))
            on = true;
    piSetIrq(PI_EXI, on);
}

// OSSramEx (SRAM 0x14..0x3F): flashID[2][12] at 0x14, flashIDCheckSum[2] at 0x3A. CARD
// mounts an unlocked card only if each checksum is the inverted byte sum of its flash ID.
void sramSetFlashId(int slot, const u8* id) {
    u8 sum = 0;
    for (int i = 0; i < 12; i++) {
        s_sram[0x14 + 12 * slot + i] = id[i];
        sum = u8(sum + id[i]);
    }
    s_sram[0x3A + slot] = u8(~sum);
}

void sramInit() {
    memset(s_sram, 0, sizeof s_sram);
    const u8 zero[12] = {};
    sramSetFlashId(0, zero);
    sramSetFlashId(1, zero);
    s_sram[0x13] = 0x2C;  // flags: stereo
    s_sram[0x12] = 0;     // English
    u16 sum = 0, inv = 0;
    for (int i = 0; i < 4; i++) {
        const u16 w = u16(s_sram[0x0C + 2 * i] << 8 | s_sram[0x0D + 2 * i]);
        sum = u16(sum + w);
        inv = u16(inv + u16(~w));
    }
    s_sram[0] = u8(sum >> 8); s_sram[1] = u8(sum);
    s_sram[2] = u8(inv >> 8); s_sram[3] = u8(inv);
}

u8 iplReadByte() {
    const u32 a = (s_ipl.cmd & 0x7FFFFFFF) >> 6;
    u8 v = 0;
    if ((s_ipl.cmd & 0x7FFFFF00) == 0x20000000) {  // RTC
        const u32 secs = u32(time(nullptr) - 946684800);  // since 2000-01-01
        v = u8(secs >> (24 - 8 * (s_ipl.pos & 3)));
    } else if ((s_ipl.cmd & 0x7FFFFF00) == 0x20000100) {  // SRAM
        v = s_sram[s_ipl.pos & 63];
    } else {
        (void)a;  // IPL ROM: not available (fonts come from the game)
    }
    s_ipl.pos++;
    return v;
}
void iplWriteByte(u8 b) {
    if ((s_ipl.cmd & 0x7FFFFF00) == 0x20000100) s_sram[s_ipl.pos & 63] = b;
    s_ipl.pos++;
}

// --- memory card (EXI channel 0/1, device 0) ---------------------------------------------
// A Memory Card 59 (4 Mbit, 8 KiB sectors) behind the flash command set the SDK's CARD
// library speaks. The image is a raw dump of the card (the same layout as Dolphin's .raw).
// It reports itself unlocked, so the SDK skips the DSP-assisted unlock handshake.
struct MemCard {
    bool present = false;
    std::vector<u8> data;
    std::string path;
    u32 id = 0x00000004;  // size 4 Mbit; latency index 0 (4 bytes); sector size index 0 (8 KiB)
    u8 status = 0x41;     // ready | unlocked
    bool irqEnabled = false;
    // the transfer since the last chip select
    u32 pos = 0;
    u8 cmd = 0;
    u8 args[5] = {};
    u32 addr = 0;
    u32 dirtyLo = ~0u, dirtyHi = 0;
} s_card[2];
constexpr u32 kCardLatency = 4;  // dummy bytes between a read command and its data

u32 cardAddr(const u8* a) { return (u32(a[1] & 0x7F) << 17) | (u32(a[2]) << 9) | (u32(a[3] & 3) << 7) | (a[4] & 0x7F); }

void cardMarkDirty(MemCard& m, u32 lo, u32 hi) {
    m.dirtyLo = std::min(m.dirtyLo, lo);
    m.dirtyHi = std::max(m.dirtyHi, hi);
}

void cardFlush(MemCard& m) {
    if (m.dirtyLo >= m.dirtyHi || m.path.empty()) return;
    if (FILE* f = fopen(m.path.c_str(), "r+b")) {
        fseek(f, long(m.dirtyLo), SEEK_SET);
        fwrite(m.data.data() + m.dirtyLo, 1, m.dirtyHi - m.dirtyLo, f);
        fclose(f);
    } else {
        logf("card", "cannot write %s", m.path.c_str());
    }
    m.dirtyLo = ~0u;
    m.dirtyHi = 0;
}

void cardInit(int slot, const std::string& path) {
    MemCard& m = s_card[slot];
    m = MemCard{};
    m.path = path;
    const u32 size = (m.id & 0xFC) * 1024 * 1024 / 8;
    m.data.assign(size, 0xFF);  // erased flash
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fread(m.data.data(), 1, size, f);
        fclose(f);
        logf("card", "slot %c: %s", 'A' + slot, path.c_str());
    } else if (FILE* f2 = fopen(path.c_str(), "wb")) {
        fwrite(m.data.data(), 1, size, f2);  // a new, unformatted card
        fclose(f2);
        logf("card", "slot %c: new card %s (unformatted)", 'A' + slot, path.c_str());
    } else {
        logf("card", "slot %c: cannot create %s; no card", 'A' + slot, path.c_str());
        return;
    }
    m.present = true;
}

// A formatted card's header serial is the console's flash ID scrambled by a generator seeded
// with the format time (CARDFormat; checked by VerifyID). Recover the flash ID from it so a
// card formatted elsewhere (another console, an emulator) mounts here.
void cardSyncSram(int slot) {
    const MemCard& m = s_card[slot];
    if (!m.present || m.data.size() < 0x200) return;
    const u8* h = m.data.data();
    const u16 deviceId = u16(h[0x20] << 8 | h[0x21]);
    if (deviceId != 0) return;  // not formatted
    u64 t = 0;
    for (int i = 0; i < 8; i++) t = (t << 8) | h[12 + i];
    s64 rnd = s64(t);
    u8 flash[12];
    for (int i = 0; i < 12; i++) {
        rnd = s64(u64(rnd) * 1103515245u + 12345u) >> 16;
        flash[i] = u8(h[i] - u8(rnd));
        rnd = (s64(u64(rnd) * 1103515245u + 12345u) >> 16) & 0x7FFF;
    }
    sramSetFlashId(slot, flash);
}

void cardRaiseIrq(int ch) {
    if (!s_card[ch].irqEnabled) return;
    exi[ch].csr |= 0x002;  // EXIINT: the erase/program finished
    exiUpdateIrq();
}

void cardSelect(int ch) {
    MemCard& m = s_card[ch];
    m.pos = 0;
    m.cmd = 0;
}

// Chip select released: erase and program commands take effect now.
void cardDeselect(int ch) {
    MemCard& m = s_card[ch];
    if (!m.present || m.pos == 0) return;
    static const bool log = envFlag("R2_CARD_LOG");
    if (log)
        logf("card", "cmd %02X args %02X %02X %02X %02X addr %06X: %u bytes, status %02X", m.cmd, m.args[1], m.args[2], m.args[3],
             m.args[4], cardAddr(m.args), m.pos, m.status);
    const u32 size = u32(m.data.size());
    switch (m.cmd) {
    case 0xF1: {  // erase sector
        const u32 a = ((u32(m.args[1] & 0x7F) << 17) | (u32(m.args[2]) << 9)) & ~0x1FFFu & (size - 1);
        memset(&m.data[a], 0xFF, 0x2000);
        cardMarkDirty(m, a, a + 0x2000);
        cardFlush(m);
        cardRaiseIrq(ch);
        break;
    }
    case 0xF2:  // page program (bytes were stored as they arrived)
        cardFlush(m);
        cardRaiseIrq(ch);
        break;
    case 0xF4:  // erase card
        std::fill(m.data.begin(), m.data.end(), u8(0xFF));
        cardMarkDirty(m, 0, size);
        cardFlush(m);
        cardRaiseIrq(ch);
        break;
    default: break;
    }
    m.pos = 0;
}

u8 cardByte(int ch, u8 in) {
    MemCard& m = s_card[ch];
    const u32 p = m.pos++;
    const u32 size = u32(m.data.size());
    if (p == 0) {
        m.cmd = in;
        if (in == 0x89) m.status &= ~0x18u;  // clear status: program/erase error bits
        static bool seen[256];
        if (!seen[in] && in != 0x00 && in != 0x52 && in != 0x83 && in != 0x89 && in != 0x81 && in != 0xF1 &&
            in != 0xF2 && in != 0xF4) {
            seen[in] = true;
            logf("card", "unhandled command %02X", in);
        }
        return 0xFF;
    }
    switch (m.cmd) {
    case 0x00:  // ID (EXIGetID: 2 command bytes, then the 32-bit id)
        return p >= 2 && p < 6 ? u8(m.id >> (8 * (5 - p))) : 0;
    case 0x83:  // read status
        return p >= 2 ? m.status : 0;
    case 0x81:  // interrupt enable
        if (p == 1) m.irqEnabled = in & 1;
        return 0;
    case 0x52:  // read array: address, latency bytes, data
        if (p <= 4) {
            m.args[p] = in;
            if (p == 4) m.addr = cardAddr(m.args);
            return 0;
        }
        if (p < 5 + kCardLatency) return 0;
        return m.data[(m.addr++) & (size - 1)];
    case 0xF2:  // page program: address, then up to 128 bytes within the page
        if (p <= 4) {
            m.args[p] = in;
            if (p == 4) m.addr = cardAddr(m.args);
            return 0;
        } else {
            const u32 a = m.addr & (size - 1);
            m.data[a] = in;
            cardMarkDirty(m, a, a + 1);
            m.addr = (m.addr & ~0x7Fu) | ((m.addr + 1) & 0x7F);
            return 0;
        }
    case 0xF1:  // erase sector: 2 address bytes
    case 0xF4:  // erase card
        if (p < 5) m.args[p] = in;
        return 0;
    default:
        return 0;
    }
}

int exiDevice(int ch) {
    const u32 cs = (exi[ch].csr >> 7) & 7;
    if (!cs) return -1;
    if (ch == 0 && (cs & 2)) return 1;              // IPL/RTC/SRAM
    if (ch < 2 && (cs & 1) && s_card[ch].present) return 2;  // memory card
    return 0;                                       // absent
}

void exiTransfer(int ch) {
    ExiChan& c = exi[ch];
    const int dev = exiDevice(ch);
    const bool dma = c.cr & 2;
    const u32 rw = (c.cr >> 2) & 3;  // 0 read, 1 write, 2 read/write
    if (dev == 1) {
        if (dma) {
            u8* p = physPtr(c.mar, c.len);
            if (p) {
                for (u32 i = 0; i < c.len; i++) {
                    if (rw == 0) p[i] = iplReadByte();
                    else iplWriteByte(p[i]);
                }
            }
        } else {
            const u32 n = ((c.cr >> 4) & 3) + 1;
            if (!s_ipl.haveCmd && rw == 1) {
                s_ipl.cmd = c.data;
                s_ipl.haveCmd = true;
                s_ipl.pos = 0;
            } else if (rw == 1) {
                for (u32 i = 0; i < n; i++) iplWriteByte(u8(c.data >> (24 - 8 * i)));
            } else {
                u32 v = 0;
                for (u32 i = 0; i < n; i++) v |= u32(iplReadByte()) << (24 - 8 * i);
                c.data = v;
            }
        }
    } else if (dev == 2) {  // memory card: one byte stream per chip select
        if (dma) {
            if (u8* p = physPtr(c.mar, c.len)) {
                for (u32 i = 0; i < c.len; i++) {
                    if (rw == 0) p[i] = cardByte(ch, 0);
                    else cardByte(ch, p[i]);
                }
            }
        } else {
            const u32 n = ((c.cr >> 4) & 3) + 1;
            u32 v = 0;
            for (u32 i = 0; i < n; i++) {
                const u8 out = cardByte(ch, rw == 0 ? 0 : u8(c.data >> (24 - 8 * i)));
                v |= u32(out) << (24 - 8 * i);
            }
            if (rw != 1) c.data = v;
        }
    } else if (!dma && rw != 1) {
        c.data = dev < 0 ? 0 : 0xFFFFFFFF;
    }
    c.cr &= ~1u;
    c.csr |= 0x008;  // TCINT
    exiUpdateIrq();
}

u32 exiRead(u32 off) {
    const int ch = int(off / 0x14);
    if (ch > 2) return 0;
    ExiChan& c = exi[ch];
    switch (off % 0x14) {
    case 0x00: {
        u32 v = c.csr;
        v &= ~0x1000u;
        if (ch < 2 && s_card[ch].present) v |= 0x1000u;  // EXT: a memory card is inserted
        return v;
    }
    case 0x04: return c.mar;
    case 0x08: return c.len;
    case 0x0C: return c.cr;
    default: return c.data;
    }
}
void exiWrite(u32 off, u32 v) {
    const int ch = int(off / 0x14);
    if (ch > 2) return;
    ExiChan& c = exi[ch];
    switch (off % 0x14) {
    case 0x00: {
        const u32 oldCs = (c.csr >> 7) & 7;
        const u32 ack = v & (0x002 | 0x008 | 0x800);
        c.csr = (c.csr & (0x002 | 0x008 | 0x800)) | (v & ~(0x002u | 0x008u | 0x800u));
        c.csr &= ~ack;
        const u32 newCs = (c.csr >> 7) & 7;
        if (ch == 0 && (newCs & 2) && !(oldCs & 2)) s_ipl.haveCmd = false;  // IPL selected: new command
        if (ch < 2 && (newCs & 1) && !(oldCs & 1)) cardSelect(ch);
        if (ch < 2 && (oldCs & 1) && !(newCs & 1)) cardDeselect(ch);
        exiUpdateIrq();
        break;
    }
    case 0x04: c.mar = v & 0x03FFFFE0; break;
    case 0x08: c.len = v & 0x03FFFFE0; break;
    case 0x0C:
        c.cr = v;
        if (v & 1) exiTransfer(ch);
        break;
    default: c.data = v; break;
    }
}

// =======================================================================================
// AI + DSP + ARAM
// =======================================================================================
u32 ai[4] = {};
u16 dsp[0x20] = {};  // 16-bit registers 0x00..0x3E
std::vector<u8> s_aram(16 * 1024 * 1024);
u32 s_mailToDsp = 0;
bool s_mailToDspFull = false;
std::vector<u32> s_mailsFromDsp;  // queued DSP->CPU mails
u32 s_mailOut = 0;
bool s_mailOutValid = false;
enum class DspState { Reset, Rom, BootParams, Ucode } s_dspState = DspState::Reset;
u32 s_bootParams[16] = {};
int s_bootParamIdx = -1;
struct {
    bool on = false;
    u32 addr = 0, blocks = 0;
} s_aiDma;

constexpr u16 CSR_RES = 1 << 0, CSR_PIINT = 1 << 1, CSR_HALT = 1 << 2, CSR_AIDINT = 1 << 3, CSR_AIDMSK = 1 << 4,
              CSR_ARINT = 1 << 5, CSR_ARMSK = 1 << 6, CSR_DSPINT = 1 << 7, CSR_DSPMSK = 1 << 8, CSR_DMABUSY = 1 << 9,
              CSR_RESETINIT = 1 << 11;
u16& dspCsr() { return dsp[0x0A / 2]; }

void dspUpdateIrq() {
    const u16 c = dspCsr();
    const bool on = ((c & CSR_AIDINT) && (c & CSR_AIDMSK)) || ((c & CSR_ARINT) && (c & CSR_ARMSK)) ||
                    ((c & CSR_DSPINT) && (c & CSR_DSPMSK));
    piSetIrq(PI_DSP, on);
}

void dspPostMail(u32 m, bool irq) {
    if (!s_mailOutValid) {
        s_mailOut = m;
        s_mailOutValid = true;
    } else {
        s_mailsFromDsp.push_back(m);
    }
    if (irq) {
        dspCsr() |= CSR_DSPINT;
        dspUpdateIrq();
    }
}

void dspMailFromCpu(u32 m) {
    if (envFlag("R2_DSP_LOG")) logf("dsp", "CPU->DSP mail %08X (state %d)", m, int(s_dspState));
    switch (s_dspState) {
    case DspState::Rom:
    case DspState::BootParams:
        // ROM bootloader: pairs of (0x80F3xxxx command, value)
        if (s_bootParamIdx < 0) {
            if ((m >> 16) == 0x80F3) {
                s_bootParamIdx = int(m & 0xF) + 4 * int((m >> 12) & 3);
                s_bootParams[15] = m;
            }
        } else {
            const u32 cmd = s_bootParams[15];
            s_bootParamIdx = -1;
            if (cmd == 0x80F3A001) s_bootParams[0] = m;       // IRAM mmem address
            else if (cmd == 0x80F3C002) s_bootParams[1] = m;  // IRAM DSP address
            else if (cmd == 0x80F3A002) s_bootParams[2] = m;  // IRAM length
            else if (cmd == 0x80F3B002) s_bootParams[3] = m;  // DRAM length
            else if (cmd == 0x80F3D001) {                     // start vector: ucode runs
                s_bootParams[4] = m;
                s_dspState = DspState::Ucode;
                logf("dsp", "ucode boot: iram mmem=%08X len=%X dsp=%04X, start %04X", s_bootParams[0], s_bootParams[2],
                     s_bootParams[1], m);
                dspPostMail(0xDCD10000, true);  // task framework: DSP_INIT
            }
        }
        break;
    case DspState::Ucode: {  // the MusyX ucode (musyx_dsp.cpp)
        u32 reply = 0;
        if (musyxDspMail(m, reply)) dspPostMail(reply, true);
        break;
    }
    default:
        break;
    }
}

void dspReset() {
    s_dspState = DspState::Reset;
    s_mailsFromDsp.clear();
    s_mailOutValid = false;
    s_mailToDspFull = false;
    s_bootParamIdx = -1;
}

void onAram() {
    dspCsr() &= ~CSR_DMABUSY;
    dspCsr() |= CSR_ARINT;
    dspUpdateIrq();
}

void aramDma() {
    const u32 mm = ((u32(dsp[0x20 / 2]) << 16) | dsp[0x22 / 2]) & 0x03FFFFE0;
    const u32 ar = ((u32(dsp[0x24 / 2]) << 16) | dsp[0x26 / 2]) & 0x03FFFFE0;
    const u32 cntH = dsp[0x28 / 2];
    const u32 cnt = ((cntH & 0x7FFF) << 16 | dsp[0x2A / 2]) & 0x03FFFFE0;
    const bool toMram = cntH & 0x8000;
    u8* m = physPtr(mm, cnt);
    if (!m) fatal("ARAM DMA with bad main memory address %08X len %X", mm, cnt);
    for (u32 i = 0; i < cnt; i++) {
        u8& a = s_aram[(ar + i) & 0x00FFFFFF];
        if (toMram) m[i] = a;
        else a = m[i];
    }
    if (toMram) watchHost(mm, cnt, "ARAM DMA");
    if (envFlag("R2_ARAM_LOG")) logf("aram", "%s mram %08X aram %08X len %X", toMram ? "A->M" : "M->A", mm, ar, cnt);
    dspCsr() |= CSR_DMABUSY;
    schedule(EV_ARAM, tbNow() + 100 + u64(cnt) * kTbClock / (200 * 1024 * 1024));
}

void onAi() {
    if (!s_aiDma.on) return;
    // the block just finished: latch the next one from the registers and interrupt
    s_aiDma.addr = ((u32(dsp[0x30 / 2]) << 16) | dsp[0x32 / 2]) & 0x03FFFFE0;
    s_aiDma.blocks = dsp[0x36 / 2] & 0x7FFF;
    dspCsr() |= CSR_AIDINT;
    dspUpdateIrq();
    const u64 samples = u64(s_aiDma.blocks ? s_aiDma.blocks : 1) * 32 / 4;
    const u32 rate = (ai[0] & 0x40) ? 32000 : 48000;
    // the latched block plays now: hand it to the host
    if (const u8* p = physPtr(s_aiDma.addr, u32(samples) * 4)) audio::push(p, u32(samples), rate);
    // Next block from when this one was due, not from now: a late interrupt (the guest was
    // busy) must not slow the audio clock down. Resynchronise only after a long stall.
    static u64 due = 0;
    const u64 now = tbNow(), period = samples * kTbClock / rate;
    due = (due && now - due < 4 * period) ? due + period : now + period;
    schedule(EV_AI, due);
}

u16 dspRead(u32 off) {
    switch (off) {
    // bit 15 of the high half is the mailbox's "full" flag, not part of the mail
    case 0x00: return u16(((s_mailToDsp >> 16) & 0x7FFF) | (s_mailToDspFull ? 0x8000 : 0));
    case 0x02: return u16(s_mailToDsp);
    case 0x04: return u16(((s_mailOut >> 16) & 0x7FFF) | (s_mailOutValid ? 0x8000 : 0));
    case 0x06: {
        const u16 v = u16(s_mailOut);
        if (s_mailOutValid) {
            s_mailOutValid = false;
            if (!s_mailsFromDsp.empty()) {
                s_mailOut = s_mailsFromDsp.front();
                s_mailsFromDsp.erase(s_mailsFromDsp.begin());
                s_mailOutValid = true;
            }
        }
        return v;
    }
    case 0x16: return u16(dsp[0x16 / 2] | 1);  // ARAM mode: initialized
    case 0x3A: return u16(s_aiDma.on ? s_aiDma.blocks : 0);
    default: return dsp[(off & 0x3F) / 2];
    }
}

void dspWrite(u32 off, u16 v) {
    switch (off) {
    case 0x00:
        s_mailToDsp = (s_mailToDsp & 0xFFFF) | (u32(v & 0x7FFF) << 16) | (u32(v & 0x8000) << 16);
        return;
    case 0x02:
        s_mailToDsp = (s_mailToDsp & 0xFFFF0000u) | v;
        s_mailToDspFull = false;  // the DSP takes it at once
        dspMailFromCpu(s_mailToDsp);
        return;
    case 0x0A: {
        u16& c = dspCsr();
        const u16 old = c;
        const u16 ack = v & (CSR_AIDINT | CSR_ARINT | CSR_DSPINT);
        c = u16((c & (CSR_AIDINT | CSR_ARINT | CSR_DSPINT | CSR_DMABUSY)) |
                (v & ~(CSR_AIDINT | CSR_ARINT | CSR_DSPINT | CSR_DMABUSY | CSR_RES)));
        c &= u16(~ack);
        if (v & CSR_RES) dspReset();
        // released from halt after a reset: the ROM boots and says hello
        if ((old & CSR_HALT) && !(c & CSR_HALT) && s_dspState == DspState::Reset) {
            s_dspState = DspState::Rom;
            dspPostMail(0x8071FEED, false);
        }
        if (v & CSR_PIINT) {
            if (envFlag("R2_DSP_LOG")) logf("dsp", "CPU interrupts DSP");
        }
        dspUpdateIrq();
        return;
    }
    case 0x2A:
        dsp[0x2A / 2] = v;
        aramDma();
        return;
    case 0x36: {
        const bool was = s_aiDma.on;
        dsp[0x36 / 2] = v;
        s_aiDma.on = v & 0x8000;
        if (s_aiDma.on && !was) {
            s_aiDma.addr = ((u32(dsp[0x30 / 2]) << 16) | dsp[0x32 / 2]) & 0x03FFFFE0;
            s_aiDma.blocks = v & 0x7FFF;
            onAi();
        } else if (!s_aiDma.on) {
            unschedule(EV_AI);
        }
        return;
    }
    default:
        dsp[(off & 0x3F) / 2] = v;
        return;
    }
}

// Streaming sample counter (AISCNT) runs while PSTAT is set; the SDK measures it at init.
u64 s_aiCountBaseTb = 0;
u32 s_aiCountBase = 0;
u32 aiRate() { return (ai[0] & 0x02) ? 48000 : 32000; }
u32 aiCounter() {
    if (!(ai[0] & 1)) return s_aiCountBase;
    return s_aiCountBase + u32((tbNow() - s_aiCountBaseTb) * aiRate() / kTbClock);
}
void aiScheduleIrq() {
    if (!(ai[0] & 1) || !(ai[0] & 0x10)) return unschedule(EV_EXI2);  // AIINTVLD
    const u32 now = aiCounter(), target = ai[3];
    const u32 delta = target > now ? target - now : 1;
    schedule(EV_EXI2, tbNow() + u64(delta) * kTbClock / aiRate());
}
void onAiCount() {
    ai[0] |= 0x08;  // AIINT
    piSetIrq(PI_AI, ai[0] & 0x04);
}

u32 aiRead(u32 off) {
    if (off == 0x08) return aiCounter();
    return ai[(off & 0xF) / 4];
}
void aiWrite(u32 off, u32 v) {
    const u32 r = (off & 0xF) / 4;
    if (r == 0) {
        const u32 cur = aiCounter();
        const u32 ack = v & 0x08;  // AIINT
        ai[0] = (ai[0] & 0x08) | (v & ~0x08u);
        ai[0] &= ~ack;
        s_aiCountBase = (v & 0x20) ? 0 : cur;  // SCRESET
        s_aiCountBaseTb = tbNow();
        ai[0] &= ~0x20u;
        piSetIrq(PI_AI, (ai[0] & 0x08) && (ai[0] & 0x04));
        aiScheduleIrq();
        return;
    }
    ai[r] = v;
    if (r == 3) aiScheduleIrq();
}

// =======================================================================================
// CP / PE / MI
// =======================================================================================
u16 pe[0x10] = {};
u16 mi[0x80] = {};

u16 cpRead(u32 off) { return gxCpRead(off); }
void cpWrite(u32 off, u16 v) { gxCpWrite(off, v); }

u16 peRead(u32 off) { return pe[(off & 0x1F) / 2]; }
void peWrite(u32 off, u16 v) {
    if (off == 0x0A && envFlag("R2_GX_LOG")) logf("gx", "PE_CTRL write %04X", v);
    if (off == 0x0A) {
        u16& c = pe[0x0A / 2];
        const u16 ack = v & 0x0C;
        c = u16((c & 0x0C) | (v & 0x03));
        c &= u16(~ack);
        piSetIrq(PI_PE_TOKEN, (c & 0x04) && (c & 0x01));
        piSetIrq(PI_PE_FINISH, (c & 0x08) && (c & 0x02));
        return;
    }
    pe[(off & 0x1F) / 2] = v;
}

}  // namespace

bool viIsFramebuffer(u32 addr) { return std::find(s_viFbs.begin(), s_viFbs.end(), addr) != s_viFbs.end(); }

u8* aramBase() { return s_aram.data(); }

// Lines in the frame buffer the VI scans out: active lines per field (VTR ACV), times two
// when interlaced (DCR NIN clear).
u32 viXfbLines() {
    const u32 acv = (vi[0x00 / 2] >> 4) & 0x3FF;
    const bool nonInterlaced = vi[0x02 / 2] & 4;
    return nonInterlaced ? acv : acv * 2;
}

bool viLocateCopy(u32 dest, u32 strideBytes, u32& line, u32& lines) {
    lines = viXfbLines();
    if (!strideBytes || !lines) return false;
    for (u32 base : s_viTop) {
        if (!base || dest < base) continue;
        const u32 off = dest - base;
        if (off % strideBytes == 0 && off / strideBytes < lines) {
            line = off / strideBytes;
            return true;
        }
    }
    return false;
}

// --- events raised by the GX FIFO parser -----------------------------------------------
void peToken(u16 token, bool irq) {
    pe[0x0E / 2] = token;
    if (irq) {
        pe[0x0A / 2] |= 0x04;
        piSetIrq(PI_PE_TOKEN, pe[0x0A / 2] & 0x01);
    }
}
void peFinish() {
    pe[0x0A / 2] |= 0x08;
    piSetIrq(PI_PE_FINISH, pe[0x0A / 2] & 0x02);
}

// --- PI helpers (runtime.h) --------------------------------------------------------------
void piSetIrq(u32 bits, bool on) {
    if (on) pi_intsr |= bits;
    else pi_intsr &= ~bits;
    piUpdate();
}
bool piPending() { return (pi_intsr & pi_intmr) != 0; }

PadState& padState(int port) { return s_pad[port & 3]; }

void hwInit() {
    s_logIo = envFlag("R2_IO_LOG");
    gxInit();
    sramInit();
    // Memory card in slot A: R2_CARD_A=<path> (default saves/GSWE64_A.raw), R2_NO_CARD=1 for none.
    if (!envFlag("R2_NO_CARD")) {
        const char* env = getenv("R2_CARD_A");
        const std::string path = env && *env ? env : "saves/GSWE64_A.raw";
        std::error_code ec;
        const auto dir = std::filesystem::path(path).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
        cardInit(0, path);
        cardSyncSram(0);
    }
    di[DICVR] = 0;  // cover closed
    for (auto& p : s_pad) p = PadState{};
    s_pad[0].connected = true;
    g_eventHandlers[EV_VI] = onVi;
    g_eventHandlers[EV_DI] = onDi;
    g_eventHandlers[EV_AI] = onAi;
    g_eventHandlers[EV_ARAM] = onAram;
    g_eventHandlers[EV_SI] = siPollAll;
    g_eventHandlers[EV_EXI2] = onAiCount;  // (slot reused for the AI sample counter interrupt)
    s_viFrameStartTb = tbNow();
    schedule(EV_VI, s_viFrameStartTb + kViPeriod);
}

void siVBlankPoll() { siPollAll(); }

// --- MMIO router ------------------------------------------------------------------------
static u32 read16dev(u16 (*rd)(u32), u32 off, int size) {
    if (size == 4) return (u32(rd(off)) << 16) | rd(off + 2);
    const u16 v = rd(off & ~1u);
    return size == 2 ? v : ((off & 1) ? v & 0xFF : v >> 8);
}
static void write16dev(void (*wr)(u32, u16), u16 (*rd)(u32), u32 off, u32 v, int size) {
    if (size == 4) {
        wr(off, u16(v >> 16));
        wr(off + 2, u16(v));
    } else if (size == 2) {
        wr(off, u16(v));
    } else {
        u16 cur = rd(off & ~1u);
        cur = (off & 1) ? u16((cur & 0xFF00) | (v & 0xFF)) : u16((cur & 0x00FF) | ((v & 0xFF) << 8));
        wr(off & ~1u, cur);
    }
}
static u32 read32dev(u32 (*rd)(u32), u32 off, int size) {
    const u32 v = rd(off & ~3u);
    if (size == 4) return v;
    if (size == 2) return (off & 2) ? v & 0xFFFF : v >> 16;
    return (v >> (24 - 8 * (off & 3))) & 0xFF;
}
static void write32dev(void (*wr)(u32, u32), u32 (*rd)(u32), u32 off, u32 v, int size) {
    if (size == 4) return wr(off, v);
    u32 cur = rd(off & ~3u);
    if (size == 2) cur = (off & 2) ? (cur & 0xFFFF0000u) | (v & 0xFFFF) : (cur & 0xFFFF) | (v << 16);
    else {
        const u32 sh = 24 - 8 * (off & 3);
        cur = (cur & ~(0xFFu << sh)) | ((v & 0xFF) << sh);
    }
    wr(off & ~3u, cur);
}

static u16 miRead(u32 off) { return mi[(off & 0xFF) / 2]; }
static void miWrite(u32 off, u16 v) { mi[(off & 0xFF) / 2] = v; }

u32 ioRead(u32 a, int size) {
    if (a < 0xCC000000u) {
        noteAccess("EFB", a, false, 0);
        return 0;
    }
    const u32 off = a & 0xFFFF;
    u32 v = 0;
    const char* dev = "?";
    switch (off >> 12) {
    case 0x0: v = read16dev(cpRead, off & 0xFFF, size); dev = "CP"; break;
    case 0x1: v = read16dev(peRead, off & 0xFFF, size); dev = "PE"; break;
    case 0x2: v = read16dev(viRead, off & 0xFFF, size); dev = "VI"; break;
    case 0x3: v = read32dev(piRead, off & 0xFFF, size); dev = "PI"; break;
    case 0x4: v = read16dev(miRead, off & 0xFFF, size); dev = "MI"; break;
    case 0x5: v = read16dev(dspRead, off & 0xFFF, size); dev = "DSP"; break;
    case 0x6:
        if (off < 0x6400) { v = read32dev(diRead, off & 0x3FF, size); dev = "DI"; }
        else if (off < 0x6800) { v = read32dev(siRead, off & 0x3FF, size); dev = "SI"; }
        else if (off < 0x6C00) { v = read32dev(exiRead, off & 0x3FF, size); dev = "EXI"; }
        else { v = read32dev(aiRead, off & 0x3FF, size); dev = "AI"; }
        break;
    default: dev = "unmapped"; break;
    }
    noteAccess(dev, a, false, v);
    return v;
}

void ioWrite(u32 a, u32 v, int size) {
    if ((a & 0xFFFFFFF8u) == 0xCC008000u) [[likely]] {
        gxFifoWrite(v, size);
        return;
    }
    if (a < 0xCC000000u) {
        noteAccess("EFB", a, true, v);
        return;
    }
    const u32 off = a & 0xFFFF;
    const char* dev = "?";
    switch (off >> 12) {
    case 0x0: write16dev(cpWrite, cpRead, off & 0xFFF, v, size); dev = "CP"; break;
    case 0x1: write16dev(peWrite, peRead, off & 0xFFF, v, size); dev = "PE"; break;
    case 0x2: write16dev(viWrite, viRead, off & 0xFFF, v, size); dev = "VI"; break;
    case 0x3: write32dev(piWrite, piRead, off & 0xFFF, v, size); dev = "PI"; break;
    case 0x4: write16dev(miWrite, miRead, off & 0xFFF, v, size); dev = "MI"; break;
    case 0x5: write16dev(dspWrite, dspRead, off & 0xFFF, v, size); dev = "DSP"; break;
    case 0x6:
        if (off < 0x6400) { write32dev(diWrite, diRead, off & 0x3FF, v, size); dev = "DI"; }
        else if (off < 0x6800) { write32dev(siWrite, siRead, off & 0x3FF, v, size); dev = "SI"; }
        else if (off < 0x6C00) { write32dev(exiWrite, exiRead, off & 0x3FF, v, size); dev = "EXI"; }
        else { write32dev(aiWrite, aiRead, off & 0x3FF, v, size); dev = "AI"; }
        break;
    default: dev = "unmapped"; break;
    }
    noteAccess(dev, a, true, v);
}


}  // namespace rt
