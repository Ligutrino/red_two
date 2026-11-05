// Minimal setjmp/longjmp for jumping back up the host stack of the current fiber.
//
// Saves every callee-saved register of the Win64 ABI (rbx rbp rdi rsi rsp r12-r15,
// xmm6-xmm15). Unlike the CRT's longjmp it does not unwind (no destructors run), which is
// what we want: the frames skipped are recompiled guest functions.
#pragma once
#include <cstdint>

namespace rt {

struct alignas(16) JmpBuf {
    uint64_t gpr[10];  // rbx rbp rdi rsi rsp r12 r13 r14 r15 rip
    uint64_t pad[2];
    uint8_t xmm[10][16];
};

}  // namespace rt

extern "C" __attribute__((returns_twice)) int rt_setjmp(rt::JmpBuf* b);
extern "C" [[noreturn]] void rt_longjmp(rt::JmpBuf* b, int v);
