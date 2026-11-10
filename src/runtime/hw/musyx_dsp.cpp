// MusyX DSP emulated at the command-list level (HLE).
//
// Rogue Leader's DSP ucode is byte-identical to the one MusyX uses for versions up to 2.0.0
// (checked against the MusyX decomp's dsp_import.c). It is an AX derivative: every 5 ms the
// CPU builds a command list (salBuildCommandList) and mails 0xBABE0000|firstSize, then its
// address; the DSP runs the list and answers 0xDCD10001 (the task manager's "resume",
// MusyX's dspResumeCallback). The list mixes 160 samples (32 kHz) per studio through nine
// buses (main/aux A/aux B x L/R/S) from voice parameter blocks (_PB) in main memory, hands aux
// buffers to the CPU's effects, and writes the final stereo frame into the AI DMA buffer.
//
// Command and structure layouts follow the MusyX decomp (MIT, ../MetroidPrimePort/extern/
// musyx: hw_dspctrl.c, voice.h); the DSP-side behaviour is reimplemented here from the AX
// model: ARAM sample fetch (ADPCM/PCM8/PCM16 with loops), 4-tap polyphase resampling, a
// per-sample volume envelope and mix ramps, per-subframe parameter updates and depop values.
#include "musyx_dsp.h"

#include <cmath>
#include <cstring>

#include "../runtime.h"

namespace rt {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kN = 160;   // samples per frame (5 ms at 32 kHz)
constexpr int kSub = 32;  // samples per subframe (5 subframes)

// PB word indices (voice.h, MusyX <= 2.0.0: 0xBC bytes)
enum : int {
    PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_SELECT = 4, PB_COEF_SELECT = 5, PB_MIXER_CTRL = 6, PB_STATE = 7,
    PB_LOOP_TYPE = 8,
    PB_VL = 9, PB_DL = 10, PB_VR = 11, PB_DR = 12, PB_VAL = 13, PB_DAL = 14, PB_VAR = 15, PB_DAR = 16,
    PB_VBL = 17, PB_DBL = 18, PB_VBR = 19, PB_DBR = 20, PB_VBS = 21, PB_DBS = 22, PB_VS = 23, PB_DS = 24,
    PB_VAS = 25, PB_DAS = 26,
    PB_UPD_NUM = 34, PB_UPD_HI = 39, PB_UPD_LO = 40,
    PB_DPOP_L = 41, PB_DPOP_AL = 42, PB_DPOP_BL = 43, PB_DPOP_R = 44, PB_DPOP_AR = 45, PB_DPOP_BR = 46,
    PB_DPOP_S = 47, PB_DPOP_AS = 48, PB_DPOP_BS = 49,
    PB_VE_VOL = 50, PB_VE_DELTA = 51,
    PB_LOOP_FLAG = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58, PB_END_HI = 59, PB_END_LO = 60,
    PB_CUR_HI = 61, PB_CUR_LO = 62,
    PB_COEFS = 63, PB_GAIN = 79, PB_PS = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PS = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92, PB_STREAM_LOOPS = 93,
    PB_WORDS = 94,
};

// buses: main L/R/S, aux A L/R/S, aux B L/R/S
enum { L = 0, R = 1, S = 2 };
s32 s_main[3][kN], s_auxA[3][kN], s_auxB[3][kN];
s32 s_out[3][kN];  // final mix of the master studios
s16 s_poly[128][4];
bool s_log = false;
u64 s_lists = 0, s_voices = 0;

u8* aram() { return aramBase(); }
constexpr u32 kAramMask = 0x00FFFFFF;

u8* mem(u32 a, u32 len) { return physPtr(a & 0x01FFFFFF, len); }
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
s32 be32s(const u8* p) { return s32(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]); }
void put32(u8* p, s32 v) {
    p[0] = u8(u32(v) >> 24); p[1] = u8(u32(v) >> 16); p[2] = u8(u32(v) >> 8); p[3] = u8(v);
}
s16 clamp16(s64 v) { return s16(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

void initPolyphase() {
    // 4 taps around the interpolation point (between taps 1 and 2), 128 phases: a windowed
    // sinc normalised to unity gain. The DSP's own tables live in its coefficient ROM.
    for (int p = 0; p < 128; p++) {
        const double f = p / 128.0;
        double w[4], sum = 0;
        for (int k = 0; k < 4; k++) {
            const double x = (k - 1) - f;
            const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double win = std::fabs(x) < 2 ? std::cos(kPi * x / 4.0) : 0.0;
            w[k] = sinc * win;
            sum += w[k];
        }
        int acc = 0;
        for (int k = 0; k < 3; k++) {
            s_poly[p][k] = s16(std::lround(w[k] / sum * 32767.0));
            acc += s_poly[p][k];
        }
        s_poly[p][3] = s16(32767 - acc);
    }
}

// --- voices ---------------------------------------------------------------------------
struct Voice {
    u16 w[PB_WORDS];
    u32 cur() const { return u32(w[PB_CUR_HI]) << 16 | w[PB_CUR_LO]; }
    void setCur(u32 a) { w[PB_CUR_HI] = u16(a >> 16); w[PB_CUR_LO] = u16(a); }
    u32 end() const { return u32(w[PB_END_HI]) << 16 | w[PB_END_LO]; }
    u32 loop() const { return u32(w[PB_LOOP_HI]) << 16 | w[PB_LOOP_LO]; }
};

// One sample from ARAM through the accelerator, advancing (and looping) the address.
s16 fetch(Voice& v) {
    const u8* ar = aram();
    u32 a = v.cur();
    s32 val = 0;
    switch (v.w[PB_FORMAT]) {
    case 0x00: {  // ADPCM: nibble addresses, 8-byte frames (header byte + 14 nibbles)
        if ((a & 15) == 0) {
            v.w[PB_PS] = ar[(a >> 1) & kAramMask];
            a += 2;
        }
        const u8 byte = ar[(a >> 1) & kAramMask];
        int nib = (a & 1) ? (byte & 15) : (byte >> 4);
        if (nib >= 8) nib -= 16;
        const u16 ps = v.w[PB_PS];
        const int scale = 1 << (ps & 15), ci = (ps >> 4) & 7;
        const s32 c1 = s16(v.w[PB_COEFS + ci * 2]), c2 = s16(v.w[PB_COEFS + ci * 2 + 1]);
        const s32 yn1 = s16(v.w[PB_YN1]), yn2 = s16(v.w[PB_YN2]);
        s64 x = (s64(nib * scale) << 11) + s64(c1) * yn1 + s64(c2) * yn2;
        val = clamp16((x + 1024) >> 11);
        v.w[PB_YN2] = v.w[PB_YN1];
        v.w[PB_YN1] = u16(val);
        break;
    }
    case 0x0A: {  // PCM16: sample addresses
        const u8* p = ar + ((a * 2) & kAramMask);
        val = clamp16((s64(s16(be16(p))) * v.w[PB_GAIN]) >> 11);
        break;
    }
    case 0x19:  // PCM8: byte addresses
        val = clamp16(s64(s8(ar[a & kAramMask])) * v.w[PB_GAIN]);
        break;
    default:
        val = 0;
        break;
    }
    if (a == v.end()) {
        a = v.loop();
        if (v.w[PB_FORMAT] == 0) {
            if (v.w[PB_LOOP_FLAG]) {
                v.w[PB_PS] = v.w[PB_LOOP_PS];
                if (v.w[PB_LOOP_TYPE] == 0) {  // a sample loop restarts its decoder history
                    v.w[PB_YN1] = v.w[PB_LOOP_YN1];
                    v.w[PB_YN2] = v.w[PB_LOOP_YN2];
                }
            } else {  // one-shot: on into ARAM's zero buffer
                v.w[PB_PS] = 0;
                v.w[PB_YN1] = v.w[PB_YN2] = 0;
            }
        }
        if (v.w[PB_LOOP_FLAG]) v.w[PB_STREAM_LOOPS]++;
    } else {
        a++;
    }
    v.setCur(a);
    return s16(val);
}

// 32 resampled samples.
void resample(Voice& v, s32* out) {
    const u16 sel = v.w[PB_SRC_SELECT];
    u32 ratio = u32(v.w[PB_RATIO_HI]) << 16 | v.w[PB_RATIO_LO];
    if (sel == 2) ratio = 0x10000;
    u32 frac = v.w[PB_FRAC];
    s16 h[4];
    for (int k = 0; k < 4; k++) h[k] = s16(v.w[PB_LAST + k]);
    for (int i = 0; i < kSub; i++) {
        frac += ratio;
        while (frac >= 0x10000) {
            h[0] = h[1];
            h[1] = h[2];
            h[2] = h[3];
            h[3] = fetch(v);
            frac -= 0x10000;
        }
        if (sel == 0) {
            const s16* c = s_poly[(frac & 0xFFFF) >> 9];
            out[i] = clamp16((s64(h[0]) * c[0] + s64(h[1]) * c[1] + s64(h[2]) * c[2] + s64(h[3]) * c[3]) >> 15);
        } else if (sel == 1) {
            out[i] = h[1] + s32((s64(h[2] - h[1]) * s64(frac & 0xFFFF)) >> 16);
        } else {
            out[i] = h[3];
        }
    }
    v.w[PB_FRAC] = u16(frac);
    for (int k = 0; k < 4; k++) v.w[PB_LAST + k] = u16(h[k]);
}

struct BusMix {
    int vol, delta, dpop;
    s32* dst;
};

void processVoice(u32 pbAddr) {
    u8* p = mem(pbAddr, PB_WORDS * 2);
    if (!p) return;
    Voice v;
    for (int i = 0; i < PB_WORDS; i++) v.w[i] = be16(p + 2 * i);
    s_voices++;

    // which buses this voice feeds (mixerCtrl, MusyX <= 2.0.0; see HandleDepopVoice)
    const u16 mc = v.w[PB_MIXER_CTRL];
    const bool useS = mc & 4, useA = mc & 1, useB = mc & 0x12;
    BusMix bus[9] = {
        {PB_VL, PB_DL, PB_DPOP_L, s_main[L]},     {PB_VR, PB_DR, PB_DPOP_R, s_main[R]},
        {PB_VS, PB_DS, PB_DPOP_S, s_main[S]},     {PB_VAL, PB_DAL, PB_DPOP_AL, s_auxA[L]},
        {PB_VAR, PB_DAR, PB_DPOP_AR, s_auxA[R]},  {PB_VAS, PB_DAS, PB_DPOP_AS, s_auxA[S]},
        {PB_VBL, PB_DBL, PB_DPOP_BL, s_auxB[L]},  {PB_VBR, PB_DBR, PB_DPOP_BR, s_auxB[R]},
        {PB_VBS, PB_DBS, PB_DPOP_BS, s_auxB[S]},
    };
    const bool enabled[9] = {true, true, useS, useA, useA, useA && (mc & 0x14), useB, useB, useB && useS};

    u32 upd = u32(v.w[PB_UPD_HI]) << 16 | v.w[PB_UPD_LO];
    s32 last = 0;
    for (int sf = 0; sf < 5; sf++) {
        // parameter updates take effect at the start of their subframe
        for (u16 n = v.w[PB_UPD_NUM + sf]; n; n--) {
            const u8* e = mem(upd, 4);
            upd += 4;
            if (!e) break;
            const u16 off = be16(e), val = be16(e + 2);
            if (off < PB_WORDS) v.w[off] = val;
        }
        if (v.w[PB_STATE] != 1) continue;
        s32 smp[kSub];
        resample(v, smp);
        s32 vol = v.w[PB_VE_VOL];
        const s32 vdelta = s16(v.w[PB_VE_DELTA]);
        for (int i = 0; i < kSub; i++) {
            smp[i] = s32((s64(smp[i]) * vol) >> 15);
            vol += vdelta;
            if (vol < 0) vol = 0;
            if (vol > 0xFFFF) vol = 0xFFFF;
        }
        v.w[PB_VE_VOL] = u16(vol);
        last = smp[kSub - 1];
        const int base = sf * kSub;
        for (int b = 0; b < 9; b++) {
            s32 mv = v.w[bus[b].vol];
            const s32 md = s16(v.w[bus[b].delta]);
            if (!enabled[b] || (mv == 0 && md == 0)) continue;
            s32* d = bus[b].dst + base;
            for (int i = 0; i < kSub; i++) {
                d[i] += s32((s64(smp[i]) * mv) >> 15);
                mv += md;
                if (mv < 0) mv = 0;
                if (mv > 0xFFFF) mv = 0xFFFF;
            }
            v.w[bus[b].vol] = u16(mv);
        }
    }
    // depop: what each bus last received from this voice (faded out by the CPU on stop)
    for (int b = 0; b < 9; b++)
        v.w[bus[b].dpop] = u16(enabled[b] ? clamp16((s64(last) * v.w[bus[b].vol]) >> 15) : 0);
    for (int i = 0; i < PB_WORDS; i++) {
        p[2 * i] = u8(v.w[i] >> 8);
        p[2 * i + 1] = u8(v.w[i]);
    }
    watchHost(pbAddr, PB_WORDS * 2, "DSP PB write-back");
}

// --- buffers in main memory: L[160], R[160], S[160] of big-endian s32 -------------------
void readLRS(u32 addr, s32 (*dst)[kN], s32 vol, bool add) {
    const u8* p = mem(addr, 3 * kN * 4);
    if (!p) return;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < kN; i++) {
            const s32 x = be32s(p + (c * kN + i) * 4);
            const s32 y = vol == 0x8000 ? x : s32((s64(x) * vol) >> 15);
            dst[c][i] = add ? dst[c][i] + y : y;
        }
}
void writeLRS(u32 addr, s32 (*src)[kN]) {
    u8* p = mem(addr, 3 * kN * 4);
    if (!p) return;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < kN; i++) put32(p + (c * kN + i) * 4, src[c][i]);
    watchHost(addr, 3 * kN * 4, "DSP buffer upload");
}

void runList(u32 addr) {
    s_lists++;
    for (int guard = 0; guard < 100000; guard++) {
        const u8* p = mem(addr, 16);
        if (!p) return;
        const u16 cmd = be16(p);
        auto arg = [&](int i) { return be16(p + 2 + 2 * i); };
        auto arg32 = [&](int i) { return u32(arg(i)) << 16 | arg(i + 1); };
        if (s_log && s_lists < 4) logf("dsp", "cmd %u at %08X", cmd, addr);
        switch (cmd) {
        case 0: {  // SETUP: the studio's buses start from its depop ramps
            const u8* spb = mem(arg32(0), 0x36);
            s32* dst[9] = {s_main[L], s_main[R], s_main[S], s_auxA[L], s_auxA[R], s_auxA[S],
                           s_auxB[L], s_auxB[R], s_auxB[S]};
            for (int b = 0; b < 9; b++) {
                s32 start = spb ? be32s(spb + b * 6) : 0;
                const s32 delta = spb ? s16(be16(spb + b * 6 + 4)) : 0;
                for (int i = 0; i < kN; i++, start += delta) dst[b][i] = start;
            }
            addr += 2 * 3;
            break;
        }
        case 1: {  // DL_AND_VOL_MIX: another studio's output into main / aux A / aux B
            const u32 src = arg32(0);
            const s32 vol = arg(2), volA = arg(3), volB = arg(4);
            s32 tmp[3][kN];
            readLRS(src, tmp, 0x8000, false);
            for (int c = 0; c < 3; c++)
                for (int i = 0; i < kN; i++) {
                    s_main[c][i] += s32((s64(tmp[c][i]) * vol) >> 15);
                    s_auxA[c][i] += s32((s64(tmp[c][i]) * volA) >> 15);
                    s_auxB[c][i] += s32((s64(tmp[c][i]) * volB) >> 15);
                }
            addr += 2 * 6;
            break;
        }
        case 2: {  // PB_ADDR + PROCESS: the voice list
            u32 pb = arg32(0);
            addr += 2 * 3;
            const u8* q = mem(addr, 2);
            if (q && be16(q) == 3) addr += 2;
            for (int n = 0; pb && n < 256; n++) {
                const u8* h = mem(pb, 4);
                if (!h) break;
                const u32 next = u32(be16(h)) << 16 | be16(h + 2);
                processVoice(pb);
                pb = next;
            }
            break;
        }
        case 3:  // PROCESS without PB_ADDR: nothing to do
            addr += 2;
            break;
        case 4:  // MIX_AUXA: aux A out to the CPU's effect, its processed return into main
        case 5:  // MIX_AUXB
        case 16: {  // MIX_AUXB_LR
            s32(*aux)[kN] = cmd == 4 ? s_auxA : s_auxB;
            writeLRS(arg32(0), aux);
            readLRS(arg32(2), s_main, 0x8000, true);
            addr += 2 * 5;
            break;
        }
        case 6:  // UPLOAD_LRS: the studio's main buses to main memory
            writeLRS(arg32(0), s_main);
            addr += 2 * 3;
            break;
        case 17:  // SET_OPPOSITE_LR: start the final mix
            std::memset(s_out, 0, sizeof s_out);
            addr += 2 * 3;
            break;
        case 9:  // MIX_AUXB_NOWRITE: a master studio's uploaded output into the final mix
            readLRS(arg32(0), s_out, 0x8000, true);
            addr += 2 * 3;
            break;
        case 14: {  // OUTPUT: fold surround in with opposite phase, 16-bit stereo to the AI buffer
            if (u8* surr = mem(arg32(0), kN * 4))
                for (int i = 0; i < kN; i++) put32(surr + i * 4, s_out[S][i]);
            u8* d = mem(arg32(2), kN * 4);
            if (d) {
                for (int i = 0; i < kN; i++) {
                    const s64 s = (s64(s_out[S][i]) * 23170) >> 15;  // -3 dB
                    const s16 l = clamp16(s_out[L][i] - s), r = clamp16(s_out[R][i] + s);
                    d[i * 4 + 0] = u8(u16(l) >> 8); d[i * 4 + 1] = u8(l);
                    d[i * 4 + 2] = u8(u16(r) >> 8); d[i * 4 + 3] = u8(r);
                }
            }
            watchHost(arg32(2), kN * 4, "DSP output");
            watchHost(arg32(0), kN * 4, "DSP surround");
            addr += 2 * 5;
            break;
        }
        case 15:  // END
            return;
        case 13:  // MORE: the list continues at another address
            addr = arg32(0);
            break;
        case 18:  // compressor (MusyX >= 2.0.1)
            addr += 2 * 5;
            break;
        default: {
            static int warned = 0;
            if (warned++ < 8) logf("dsp", "MusyX: unknown command %u at %08X", cmd, addr);
            return;
        }
        }
    }
}

u32 s_pendingFirstSize = 0;
bool s_expectList = false;

}  // namespace

bool musyxDspMail(u32 mail, u32& reply) {
    static bool init = false;
    if (!init) {
        init = true;
        initPolyphase();
        s_log = envFlag("R2_DSP_LOG");
    }
    if (s_expectList) {
        s_expectList = false;
        runList(mail);
        reply = 0xDCD10001;  // done with this frame: the task manager calls MusyX's resume callback
        return true;
    }
    if ((mail >> 16) == 0xBABE) {
        s_pendingFirstSize = mail & 0xFFFF;
        s_expectList = true;
        return false;
    }
    if (s_log) logf("dsp", "MusyX: mail %08X ignored", mail);
    return false;
}

std::string musyxDspStats() {
    char b[96];
    snprintf(b, sizeof b, "%llu lists, %llu voice frames", (unsigned long long)s_lists, (unsigned long long)s_voices);
    s_lists = s_voices = 0;
    return b;
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "../savestate.h"
namespace rt {
void musyxSaveState(state::Writer& w) {
    w.section("MUSY");
    w.pod(s_pendingFirstSize);
    w.pod(s_expectList);
}
void musyxLoadState(state::Reader& r) {
    r.section("MUSY");
    r.pod(s_pendingFirstSize);
    r.pod(s_expectList);
}
}  // namespace rt
