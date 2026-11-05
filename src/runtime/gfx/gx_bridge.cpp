// Renders the game's GX command stream with Aurora.
//
// Aurora decodes raw BP/CP/XF registers and draws itself, but it takes memory references
// (vertex arrays, textures, palettes, EFB copy destinations) as host pointers through its own
// GX_AURORA sub-commands, as a source port's GX calls would send them. The game's stream has
// the hardware form instead: physical addresses in CP 0xA0-0xAF, BP TX_SETIMAGE3, the TLUT
// load registers and the copy registers. This bridge forwards the stream and translates those:
//   * vertex arrays: sized from the indices the draws actually use (Aurora uploads whole arrays)
//   * textures: one LOAD_TEXOBJ per changed texture map before a draw, from the raw registers
//     (Aurora caches textures by content hash, so memory the game rewrites shows up)
//   * palettes: TMEM TLUT loads are remembered and bound to Aurora TLUT slots at draw time
//   * EFB copies: source rect, destination size/format/address; display copies end the frame
// Display lists are inlined by the CP model (hw/gx_fifo.cpp), so CALL_DL never arrives here.
#include "gx_bridge.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <xxhash.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <initializer_list>
#include <unordered_map>

#include "../../../extern/aurora/lib/gx/fifo.hpp"

// user32 SetWindowPos without pulling <windows.h> into this file (its macros clash with GX names).
extern "C" __declspec(dllimport) int __stdcall SetWindowPos(void*, void*, int, int, int, int, unsigned);
#include "../hw/gx_fifo.h"
#include "../runtime.h"

namespace rt::gfx {
namespace fifo = aurora::gx::fifo;

namespace {

bool s_enabled = false;
bool s_inFrame = false;
u64 s_lastFrameTb = 0;
u64 s_frames = 0;

// --- raw state shadowed from the stream ---
u32 s_bpMask = 0xFFFFFF;
u32 s_bp[256] = {};
u32 s_vcdLo = 0, s_vcdHi = 0, s_vatA[8] = {}, s_vatB[8] = {}, s_vatC[8] = {};

struct Array {
    u32 base = 0;       // physical
    u32 stride = 0;
    u32 sentSize = 0;   // size Aurora has; 0 = not sent since the base changed
};
Array s_arrays[16];

struct TexMap {
    bool dirty = true;
    u64 frame = ~0ull;  // frame the map was last sent
};
TexMap s_tex[8];

struct TlutLoad {
    u32 addr = 0, entries = 0;
};
std::unordered_map<u32, TlutLoad> s_tlutLoads;  // TMEM offset -> last load
struct TlutSlot {
    u32 tmem = ~0u, fmt = 0, addr = 0, entries = 0, id = 0;
};
TlutSlot s_tlutSlots[16];
u32 s_tlutNext = 0;
std::unordered_map<u64, u32> s_tlutIds;  // content key -> Aurora TLUT object id
u32 s_nextTlutId = 1;

inline u32 be16(const u8* p) { return (u32(p[0]) << 8) | p[1]; }
inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

void aur(u16 sub) {
    fifo::write_u8(GX_AURORA);
    fifo::write_u16(sub);
}

// ---------------------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------------------
u64 s_lastPollTb = 0;

void pollEvents() {
    s_lastPollTb = tbNow();
    const AuroraEvent* e = aurora_update();
    for (; e && e->type != AURORA_NONE; ++e) {
        if (e->type == AURORA_EXIT) {
            logf("gfx", "window closed");
            quit(0);
        }
    }
}

void testQuad();
bool s_testClear = false;  // R2_GFX_TEST: clear each frame to magenta (is presentation working?)
bool s_xfb = true;         // R2_NO_XFB: present the EFB instead of the emulated XFB

void ensureFrame() {
    if (s_inFrame) return;
    pollEvents();
    s_inFrame = aurora_begin_frame();
    if (s_inFrame && s_testClear) {
        aur(GX_AURORA_CLEAR);
        fifo::write_u8(0x0F);  // whole target, color, alpha, depth
        for (int i = 0; i < 4; i++) fifo::write_u32(0);
        fifo::write_u8(255); fifo::write_u8(0); fifo::write_u8(255); fifo::write_u8(255);
        fifo::write_u32(0xFFFFFF);
        testQuad();
    }
}

// A green quad drawn purely with raw hardware registers, as the game's own GX code would.
void testQuad() {
    auto bp = [](u32 v) { fifo::write_u8(0x61); fifo::write_u32(v); };
    auto cp = [](u8 r, u32 v) { fifo::write_u8(0x08); fifo::write_u8(r); fifo::write_u32(v); };
    auto xf = [](u16 a, std::initializer_list<u32> vals) {
        fifo::write_u8(0x10);
        fifo::write_u32((u32(vals.size() - 1) << 16) | a);
        for (u32 v : vals) fifo::write_u32(v);
    };
    auto f = [](float x) { u32 u; memcpy(&u, &x, 4); return u; };
    cp(0x50, 0x2200);      // POS, CLR0 direct
    cp(0x60, 0);
    cp(0x70, 0x40016009);  // POS XYZ f32, CLR0 RGBA8
    xf(0x1008, {1});       // one color
    xf(0x1009, {1});
    xf(0x100E, {1});       // color0: vertex color, no lighting
    xf(0x1010, {1});
    xf(0x103F, {0});
    bp(0x00000010);        // genmode: 1 color, 0 texgens, 1 stage, no culling
    bp(0xC008FFFA);        // TEV color = RASC
    bp(0xC108FFD0);        // TEV alpha = RASA
    bp(0x2800003F);        // stage 0: no texture, color0
    bp(0x41000018);        // color+alpha update, no blending
    bp(0x40000000);        // no Z
    bp(0xF33F0000);        // alpha test always
    xf(0x0000, {f(1), 0, 0, 0, 0, f(1), 0, 0, 0, 0, f(1), 0});
    cp(0x30, 0);
    xf(0x1018, {0});
    xf(0x101A, {f(320), f(-240), f(16777215), f(660), f(580), f(16777215)});
    xf(0x1020, {f(2.f / 640), f(-1), f(-2.f / 480), f(1), 0, f(-0.5f), 1});
    bp(0x20154154);
    bp(0x213D3333);
    bp(0x590D5B56);  // scissor offset 340 in Aurora's (negated) convention
    fifo::write_u8(0x80);  // quads, format 0
    fifo::write_u16(4);
    const float vtx[4][2] = {{100, 100}, {540, 100}, {540, 380}, {100, 380}};
    for (auto& v : vtx) {
        fifo::write_u32(f(v[0]));
        fifo::write_u32(f(v[1]));
        fifo::write_u32(0);
        fifo::write_u32(0x00FF00FF);
    }
    fifo::finish_draw();
}

void endFrame() {
    if (!s_inFrame) return;
    aurora_end_frame();
    s_inFrame = false;
    s_frames++;
    s_lastFrameTb = tbNow();
}

// ---------------------------------------------------------------------------------------
// vertex arrays
// ---------------------------------------------------------------------------------------
void sendArray(u32 n, u32 need) {
    Array& a = s_arrays[n];
    if (!a.base) return;
    if (a.sentSize && need <= a.sentSize) return;
    u32 size = a.sentSize ? a.sentSize : 256;
    while (size < need) size *= 2;
    const u32 maxSize = kMem1Size - (a.base & 0x01FFFFFF);
    if (size > maxSize) size = maxSize;
    u8* host = physPtr(a.base, size);
    if (!host) return;
    aur(u16(GX_AURORA_LOAD_ARRAYBASE | n));
    fifo::write_u64(reinterpret_cast<u64>(host));
    fifo::write_u32(size);
    fifo::write_u8(0);  // big-endian data
    a.sentSize = size;
}

u32 compSize(u32 fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : fmt == 4 ? 4 : 0; }
u32 colSize(u32 fmt) {
    static const u32 k[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return k[fmt & 7];
}

// Walk the vertices of a draw: grow each indexed array to cover the largest index used.
void prepareArrays(const u8* p) {
    const u32 fmt = p[0] & 7;
    const u32 count = be16(p + 1);
    const u32 lo = s_vcdLo, hi = s_vcdHi, A = s_vatA[fmt], B = s_vatB[fmt], C = s_vatC[fmt];
    struct Ref {
        u32 array, offset, bytes, count;  // count: indices per vertex for this attribute
    } refs[12];
    u32 nrefs = 0, off = 0;
    for (int i = 0; i < 9; i++)
        if (lo & (1u << i)) off += 1;
    auto attr = [&](u32 type, u32 array, u32 directBytes, u32 idxPerVtx) {
        if (type == 1) off += directBytes;
        else if (type >= 2) {
            const u32 b = type == 2 ? 1 : 2;
            refs[nrefs++] = {array, off, b, idxPerVtx};
            off += b * idxPerVtx;
        }
    };
    attr((lo >> 9) & 3, 0, compSize((A >> 1) & 7) * ((A & 1) ? 3 : 2), 1);
    const bool nbt = (A >> 9) & 1;
    attr((lo >> 11) & 3, 1, compSize((A >> 10) & 7) * (nbt ? 9 : 3), (nbt && (A >> 31)) ? 3 : 1);
    attr((lo >> 13) & 3, 2, colSize((A >> 14) & 7), 1);
    attr((lo >> 15) & 3, 3, colSize((A >> 18) & 7), 1);
    const u32 texCnt[8] = {(A >> 21) & 1, B & 1, (B >> 9) & 1, (B >> 18) & 1, (B >> 27) & 1, (C >> 5) & 1, (C >> 14) & 1, (C >> 23) & 1};
    const u32 texFmt[8] = {(A >> 22) & 7, (B >> 1) & 7, (B >> 10) & 7, (B >> 19) & 7, (B >> 28) & 7, (C >> 6) & 7, (C >> 15) & 7, (C >> 24) & 7};
    for (u32 t = 0; t < 8; t++) attr((hi >> (2 * t)) & 3, 4 + t, compSize(texFmt[t]) * (texCnt[t] ? 2 : 1), 1);
    if (!nrefs) return;
    const u32 vsize = off;
    u32 maxIdx[12] = {};
    const u8* v = p + 3;
    for (u32 i = 0; i < count; i++, v += vsize)
        for (u32 r = 0; r < nrefs; r++)
            for (u32 k = 0; k < refs[r].count; k++) {
                const u8* q = v + refs[r].offset + k * refs[r].bytes;
                const u32 idx = refs[r].bytes == 1 ? q[0] : be16(q);
                if (idx > maxIdx[r]) maxIdx[r] = idx;
            }
    for (u32 r = 0; r < nrefs; r++) {
        const Array& a = s_arrays[refs[r].array];
        const u32 stride = a.stride ? a.stride : 16;
        sendArray(refs[r].array, (maxIdx[r] + 1) * stride + 16);
    }
}

// ---------------------------------------------------------------------------------------
// textures and palettes
// ---------------------------------------------------------------------------------------
u32 texRegBase(u32 map) { return map < 4 ? 0x80 + map : 0xA0 + (map - 4); }
bool isPalette(u32 fmt) { return fmt == 8 || fmt == 9 || fmt == 10; }

u64 hashBytes(const u8* p, u32 n) {
    u64 h = 1469598103934665603ull;  // FNV-1a (palettes are at most 512 bytes)
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Bind the palette the texture names (TX_SETTLUT: TMEM offset + format) to an Aurora slot.
u32 bindTlut(u32 tlutReg) {
    const u32 tmem = tlutReg & 0x3FF, fmt = (tlutReg >> 10) & 3;
    auto it = s_tlutLoads.find(tmem);
    if (it == s_tlutLoads.end()) return 0;
    const TlutLoad& ld = it->second;
    const u32 bytes = ld.entries * 2;
    const u8* src = physPtr(ld.addr, bytes);
    if (!src) return 0;
    const u64 key = hashBytes(src, bytes) ^ (u64(fmt) << 60) ^ (u64(ld.entries) << 40);
    u32& id = s_tlutIds[key];
    if (!id) id = s_nextTlutId++;
    u32 slot = 0;
    for (; slot < 16; slot++)
        if (s_tlutSlots[slot].tmem == tmem && s_tlutSlots[slot].fmt == fmt) break;
    if (slot == 16) slot = (s_tlutNext++) % 16;
    TlutSlot& s = s_tlutSlots[slot];
    if (s.tmem != tmem || s.fmt != fmt || s.id != id || s.addr != ld.addr || s.entries != ld.entries) {
        s = {tmem, fmt, ld.addr, ld.entries, id};
        aur(GX_AURORA_LOAD_TLUT);
        fifo::write_u8(u8(slot));
        fifo::write_u64(reinterpret_cast<u64>(src));
        fifo::write_u32(fmt);
        fifo::write_u16(u16(ld.entries));
        fifo::write_u32(id);
        fifo::write_u32(0);
    }
    return slot;
}

// Texture identity for Aurora's cache: one object id per (address, size, format, mips), and a
// version that changes when the texture's bytes do (hashed at most once per frame).
struct TexInfo {
    u32 id = 0, version = 0;
    u64 hash = 0, frame = ~0ull;
};
std::unordered_map<u64, TexInfo> s_texInfo;
u32 s_nextTexId = 1;
std::unordered_map<u32, u32> s_copyDests;  // EFB copy destinations (contents live on the GPU)

u32 textureBytes(u32 fmt, u32 w, u32 h, u32 levels) {
    u32 bw = 4, bh = 4, bytes = 32;
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;  // I4 C4 CMPR
    case 1: case 2: case 9: bw = 8; bh = 4; break;   // I8 IA4 C8
    case 6: bytes = 64; break;                        // RGBA8
    default: break;                                   // 4x4 blocks of 32 bytes
    }
    u32 total = 0;
    for (u32 l = 0; l < levels; l++) {
        total += ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

void textureIdentity(u32 addr, u32 image0, bool mips, u32 levels, const u8* host, u32& id, u32& version) {
    id = version = 0;
    if (s_copyDests.count(addr)) return;  // render target: let Aurora resolve it every time
    const u32 fmt = (image0 >> 20) & 0xF, w = (image0 & 0x3FF) + 1, h = ((image0 >> 10) & 0x3FF) + 1;
    const u32 bytes = textureBytes(fmt, w, h, levels);
    if (!physPtr(addr, bytes)) return;
    const u64 key = (u64(addr) << 32) ^ (u64(image0) << 1) ^ (mips ? 1 : 0) ^ (u64(levels) << 56);
    TexInfo& t = s_texInfo[key];
    if (!t.id) t.id = s_nextTexId++;
    if (t.frame != s_frames) {
        const u64 hsh = XXH3_64bits(host, bytes);
        if (hsh != t.hash) {
            t.hash = hsh;
            t.version++;
        }
        t.frame = s_frames;
    }
    id = t.id;
    version = t.version;
}

void prepareTextures() {
    for (u32 m = 0; m < 8; m++) {
        TexMap& t = s_tex[m];
        const u32 base = texRegBase(m);
        const u32 image3 = s_bp[base + 0x14];
        const u32 tlutReg = s_bp[base + 0x18];
        const u32 image0 = s_bp[base + 0x08];
        const u32 fmt = (image0 >> 20) & 0xF;
        // re-sent when the registers change, once a frame (contents may change behind
        // unchanged registers: movies, render targets), and for palettes
        if (!t.dirty && t.frame == s_frames && !isPalette(fmt)) continue;
        t.dirty = false;
        t.frame = s_frames;
        const u32 addr = (image3 & 0x1FFFFF) << 5;
        if (!addr) continue;
        const u32 w = (image0 & 0x3FF) + 1, h = ((image0 >> 10) & 0x3FF) + 1;
        u8* host = physPtr(addr, 32);
        if (!host) continue;
        const u32 mode0 = s_bp[base + 0x00], mode1 = s_bp[base + 0x04];
        const u32 minFilter = (mode0 >> 5) & 7;
        const bool mips = (minFilter == 1 || minFilter == 2 || minFilter == 5 || minFilter == 6) && ((mode1 >> 8) & 0xFF) > 0;
        const u32 tlut = isPalette(fmt) ? bindTlut(tlutReg) : 0;
        static const bool logTex = envFlag("R2_TEX_LOG");
        if (logTex) {
            u32 sum = 0;
            for (int i = 0; i < 64; i++) sum += host[i];
            logf("tex", "map %u: %08X %ux%u fmt %u tlut %u mips %d mode0 %06X mode1 %06X (first 64 bytes sum %u)", m, addr, w, h,
                 fmt, tlut, int(mips), mode0, mode1, sum);
        }
        aur(GX_AURORA_LOAD_TEXOBJ);
        fifo::write_u8(u8(m));
        fifo::write_u64(reinterpret_cast<u64>(host));
        fifo::write_u32(w);
        fifo::write_u32(h);
        fifo::write_u32(fmt);
        fifo::write_u32(tlut);
        fifo::write_u8(mips ? 1 : 0);
        u32 id, version;
        const u32 levels = mips ? ((mode1 >> 8) & 0xFF) / 16 + 1 : 1;
        textureIdentity(addr, image0, mips, levels, host, id, version);
        fifo::write_u32(id);
        fifo::write_u32(version);
    }
}

// ---------------------------------------------------------------------------------------
// EFB copies
// ---------------------------------------------------------------------------------------
// Inverse of GXSetTexCopyDst's encoding: format bits 3-6, intensity (bits 15-16 = 3), and a
// depth copy when the PE pixel format is Z24.
u32 copyFormat(u32 cp) {
    const u32 f = ((cp >> 4) & 7) | (((cp >> 3) & 1) << 3);
    const bool intensity = ((cp >> 15) & 3) == 3;
    const bool depth = (s_bp[0x43] & 7) == 3;
    if (depth) {
        switch (f) {
        case 0: return 0x30;            // GX_CTF_Z4
        case 1: case 8: return 0x11;    // GX_TF_Z8
        case 6: return 0x16;            // GX_TF_Z24X8
        case 9: return 0x39;            // GX_CTF_Z8M
        case 10: return 0x3A;           // GX_CTF_Z8L
        case 11: case 3: return 0x13;   // GX_TF_Z16
        case 12: return 0x3C;           // GX_CTF_Z16L
        default: return 0x16;
        }
    }
    if (intensity) {
        switch (f) {
        case 0: return 0;     // GX_TF_I4
        case 1: return 1;     // GX_TF_I8
        case 2: return 2;     // GX_TF_IA4
        case 3: return 3;     // GX_TF_IA8
        case 6: return 0x26;  // GX_CTF_YUVA8
        default: return 1;
        }
    }
    switch (f) {
    case 0: return 0x20;           // GX_CTF_R4
    case 1: case 8: return 0x28;   // GX_CTF_R8
    case 2: return 0x22;           // GX_CTF_RA4
    case 3: return 0x23;           // GX_CTF_RA8
    case 4: return 4;              // GX_TF_RGB565
    case 5: return 5;              // GX_TF_RGB5A3
    case 6: return 6;              // GX_TF_RGBA8
    case 7: return 0x27;           // GX_CTF_A8
    case 9: return 0x29;           // GX_CTF_G8
    case 10: return 0x2A;          // GX_CTF_B8
    case 11: return 0x2B;          // GX_CTF_RG8
    case 12: return 0x2C;          // GX_CTF_GB8
    default: return 6;
    }
}

void prepareCopy(u32 cp) {
    const u32 tl = s_bp[0x49], wh = s_bp[0x4A];
    const u32 left = tl & 0x3FF, top = (tl >> 10) & 0x3FF;
    const u32 w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    if (cp & (1u << 14)) {  // to the XFB
        aur(GX_AURORA_LOAD_DISP_COPY_SRC);
        fifo::write_u32(left);
        fifo::write_u32(top);
        fifo::write_u32(w);
        fifo::write_u32(h);
        return;
    }
    const bool half = (cp >> 9) & 1;
    const u32 dw = half ? (w + 1) / 2 : w, dh = half ? (h + 1) / 2 : h;
    const u32 dest = (s_bp[0x4B] & 0xFFFFFF) << 5;
    u8* host = physPtr(dest, 32);
    s_copyDests[dest] = 1;
    aur(GX_AURORA_LOAD_COPY_SRC);
    fifo::write_u32(left);
    fifo::write_u32(top);
    fifo::write_u32(w);
    fifo::write_u32(h);
    aur(GX_AURORA_LOAD_COPY_DST);
    fifo::write_u32(dw);
    fifo::write_u32(dh);
    fifo::write_u32(copyFormat(cp));
    fifo::write_u8(half ? 1 : 0);
    aur(GX_AURORA_LOAD_COPY_DEST);
    fifo::write_u64(reinterpret_cast<u64>(host));
}

// ---------------------------------------------------------------------------------------
// the stream
// ---------------------------------------------------------------------------------------
// R2_GX_DUMP=<frame>: write every command of that presented frame to build/gx_frame.txt
FILE* s_dump = nullptr;
u64 s_dumpFrame = ~0ull;

void dumpCommand(const u8* p, u32 len) {
    const u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        const u32 n = be16(p + 1), vs = n ? (len - 3) / n : 0;
        fprintf(s_dump, "DRAW prim %02X fmt %u count %u vsize %u vcd %08X/%08X vat %08X %08X %08X:", op & 0xF8, op & 7, n,
                vs, s_vcdLo, s_vcdHi, s_vatA[op & 7], s_vatB[op & 7], s_vatC[op & 7]);
        for (u32 i = 3; i < len && i < 3 + 2 * vs; i++) fprintf(s_dump, " %02X", p[i]);
        fputc('\n', s_dump);
    } else if (op == 0x61) {
        fprintf(s_dump, "BP %02X = %06X\n", p[1], be32(p + 1) & 0xFFFFFF);
    } else if (op == 0x08) {
        fprintf(s_dump, "CP %02X = %08X\n", p[1], be32(p + 2));
    } else if (op == 0x10) {
        const u32 h = be32(p + 1), cnt = (h >> 16) + 1, addr = h & 0xFFFF;
        fprintf(s_dump, "XF %04X x%u:", addr, cnt);
        for (u32 i = 0; i < cnt && i < 16; i++) {
            const u32 w = be32(p + 5 + 4 * i);
            float f;
            memcpy(&f, &w, 4);
            fprintf(s_dump, " %08X(%g)", w, f);
        }
        fputc('\n', s_dump);
    } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
        fprintf(s_dump, "LOAD_INDX %c idx %u len %u dst %03X\n", 'A' + (op - 0x20) / 8, be16(p + 1), (be16(p + 3) >> 12) + 1,
                be16(p + 3) & 0xFFF);
    } else {
        fprintf(s_dump, "OP %02X len %u\n", op, len);
    }
}

void onCommand(const u8* p, u32 len) {
    ensureFrame();
    if (s_frames == s_dumpFrame) [[unlikely]] {
        if (!s_dump) s_dump = fopen("build/gx_frame.txt", "w");
        if (s_dump) dumpCommand(p, len);
    } else if (s_dump) {
        fclose(s_dump);
        s_dump = nullptr;
        s_dumpFrame = ~0ull;
        logf("gfx", "frame dumped to build/gx_frame.txt");
    }
    const u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        prepareArrays(p);
        prepareTextures();
        // Aurora merges consecutive draws with unchanged state; on this game's stream merged
        // draws come out wrong (text with missing wedges, see CLAUDE.md), so every draw is
        // made to stand alone: a TLUT load into slot 1023 (none) is always handled and only
        // dirties the texture state. R2_ALLOW_MERGE=1 turns this off for investigation.
        static const bool allowMerge = envFlag("R2_ALLOW_MERGE");
        if (!allowMerge && s_bpMask == 0xFFFFFF) {
            fifo::write_u8(0x61);
            fifo::write_u32(0x650003FFu);
        }
        fifo::write_data(p, len);
        fifo::finish_draw();
        return;
    }
    switch (op) {
    case 0x08: {  // CP register
        const u8 reg = p[1];
        const u32 v = be32(p + 2);
        if (reg >= 0xA0 && reg <= 0xAF) {  // array base: sent as a host pointer when used
            Array& a = s_arrays[reg - 0xA0];
            if (a.base != (v & 0x03FFFFFF)) {
                a.base = v & 0x03FFFFFF;
                a.sentSize = 0;
            }
            return;
        }
        if (reg >= 0xB0 && reg <= 0xBF) s_arrays[reg - 0xB0].stride = v & 0xFF;
        else if (reg == 0x50) s_vcdLo = v;
        else if (reg == 0x60) s_vcdHi = v;
        else if ((reg & 0xF8) == 0x70) s_vatA[reg & 7] = v;
        else if ((reg & 0xF8) == 0x80) s_vatB[reg & 7] = v;
        else if ((reg & 0xF8) == 0x90) s_vatC[reg & 7] = v;
        fifo::write_data(p, len);
        return;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {  // indexed XF load from arrays 12-15
        const u32 n = 12 + (op - 0x20) / 8;
        const u32 idx = be16(p + 1), cnt = (be16(p + 3) >> 12) + 1;
        const u32 stride = s_arrays[n].stride ? s_arrays[n].stride : 4;
        sendArray(n, idx * stride + cnt * 4);
        fifo::write_data(p, len);
        return;
    }
    case 0x61: {  // BP register
        const u32 raw = be32(p + 1);
        const u8 reg = u8(raw >> 24);
        if (reg == 0xFE) {
            s_bpMask = raw & 0xFFFFFF;
            fifo::write_data(p, len);
            return;
        }
        const u32 v = (s_bp[reg] & ~s_bpMask) | (raw & s_bpMask & 0xFFFFFF);
        s_bpMask = 0xFFFFFF;
        s_bp[reg] = v;
        if ((reg >= 0x80 && reg <= 0x9B) || (reg >= 0xA0 && reg <= 0xBB)) {
            const u32 idx = reg & 0x1F;
            if (idx < 0x1C) s_tex[(idx & 3) + (reg >= 0xA0 ? 4 : 0)].dirty = true;
        } else if (reg == 0x59) {
            // Scissor box offset. Hardware: field = (offset + 340) / 2 and the box is
            // raw - 2 * field. Aurora's decoder adds 2 * sign-extended field (its own
            // GXSetScissorBoxOffset writes (offset - 342) / 2), so send the field negated.
            const u32 fx = (0u - (v & 0x3FF)) & 0x3FF, fy = (0u - ((v >> 10) & 0x3FF)) & 0x3FF;
            fifo::write_u8(0x61);
            fifo::write_u32(0xFEFFFFFFu);  // the value below is complete: no pending mask
            fifo::write_u8(0x61);
            fifo::write_u32(0x59000000u | (fy << 10) | fx);
            return;
        } else if (reg == 0x65) {  // TLUT load into TMEM: remember it, bind at draw time
            s_tlutLoads[v & 0x3FF] = {(s_bp[0x64] & 0x1FFFFF) << 5, ((v >> 10) & 0x7FF) * 16};
            return;
        } else if (reg == 0x52) {
            const bool disp = v & (1u << 14);
            if (disp && !s_xfb) {  // present the EFB as it is before the copy clears it
                endFrame();
                ensureFrame();
            }
            prepareCopy(v);
            fifo::write_data(p, len);
            fifo::publish();
            if (disp && s_xfb) endFrame();  // the frame is in the XFB
            return;
        }
        fifo::write_data(p, len);
        return;
    }
    case 0x40:  // CALL_DL: inlined by the CP model
        return;
    default:
        fifo::write_data(p, len);
        return;
    }
}

void sendToBack(void* hwnd) {
    constexpr unsigned SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10;
    SetWindowPos(hwnd, reinterpret_cast<void*>(1) /* HWND_BOTTOM */, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
}

void logCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    static const char* names[] = {"debug", "info", "warn", "error", "fatal"};
    static const bool debug = envFlag("R2_AURORA_DEBUG");
    if (level == LOG_DEBUG && !debug) return;
    logf("aurora", "%s %s: %.*s", names[level], module, int(len), message);
    if (level == LOG_FATAL) fatal("Aurora: %.*s", int(len), message);
}

}  // namespace

bool init(int argc, char** argv) {
    AuroraConfig cfg{};
    cfg.appName = "Red Two";
    cfg.desiredBackend = BACKEND_AUTO;
    cfg.vsync = false;  // the game paces itself on the emulated retrace
    s_testClear = envFlag("R2_GFX_TEST");
    if (const char* d = getenv("R2_GX_DUMP")) s_dumpFrame = strtoull(d, nullptr, 10);
    s_xfb = !envFlag("R2_NO_XFB");
    cfg.emulateXfb = s_xfb;
    cfg.windowWidth = 1280;
    cfg.windowHeight = 960;
    cfg.windowPosX = -1;
    cfg.windowPosY = -1;
    cfg.logCallback = logCallback;
    cfg.logLevel = envFlag("R2_AURORA_DEBUG") ? LOG_DEBUG : LOG_INFO;
    cfg.gxProcessingMode = GX_PROCESSING_INLINE;  // draws read guest memory when they are issued
    cfg.mem1Size = 0;
    cfg.mem2Size = 0;
    if (envFlag("R2_HEADLESS")) cfg.surface.type = AURORA_SURFACE_HEADLESS;
    // R2_BACKGROUND: automated runs - the window opens behind everything without taking focus
    // (it still renders, and PrintWindow can capture it).
    const bool background = envFlag("R2_BACKGROUND");
    if (background) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
    }
    const AuroraInfo info = aurora_initialize(argc, argv, &cfg);
    if (background) {
        if (SDL_Window* w = aurora_get_window()) {
            if (void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
                sendToBack(hwnd);
        }
    }
    static const char* backends[] = {"auto", "D3D11", "D3D12", "Metal", "Vulkan", "OpenGL", "OpenGL ES", "WebGPU", "null"};
    logf("gfx", "Aurora up: %s backend, window %ux%u", backends[info.backend], info.windowSize.width, info.windowSize.height);
    s_enabled = true;
    gxSetSink(onCommand);
    input::init();
    return true;
}

void vblank() {
    if (!s_enabled) return;
    input::poll();
    // Keep the window responsive (and closable) while the game is not presenting: loading
    // screens, or a frame that never ends. Polling only reads SDL events, so it is safe mid-frame.
    if (tbNow() - s_lastPollTb > kTbClock / 10) pollEvents();
}

u64 frames() { return s_frames; }

std::string stats() {
    const AuroraStats* st = aurora_get_stats();
    if (!st) return {};
    char b[256];
    snprintf(b, sizeof b, "aurora: %u draws (%u merged), pipelines %u queued %u created, %u KiB verts, %u KiB tex upload",
             st->drawCallCount, st->mergedDrawCallCount, st->queuedPipelines, st->createdPipelines, st->lastVertSize / 1024,
             st->lastTextureUploadSize / 1024);
    return b;
}

}  // namespace rt::gfx
