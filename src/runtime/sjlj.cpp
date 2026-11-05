#include "sjlj.h"

#if !defined(_WIN64)
#error "sjlj.cpp: only the Win64 ABI is implemented"
#endif

// rcx = buffer (Win64 calling convention)
extern "C" __attribute__((naked, returns_twice)) int rt_setjmp(rt::JmpBuf*) {
    asm volatile(
        "movq %rbx, 0(%rcx)\n\t"
        "movq %rbp, 8(%rcx)\n\t"
        "movq %rdi, 16(%rcx)\n\t"
        "movq %rsi, 24(%rcx)\n\t"
        "leaq 8(%rsp), %rdx\n\t"
        "movq %rdx, 32(%rcx)\n\t"
        "movq %r12, 40(%rcx)\n\t"
        "movq %r13, 48(%rcx)\n\t"
        "movq %r14, 56(%rcx)\n\t"
        "movq %r15, 64(%rcx)\n\t"
        "movq (%rsp), %rdx\n\t"
        "movq %rdx, 72(%rcx)\n\t"
        "movdqa %xmm6, 96(%rcx)\n\t"
        "movdqa %xmm7, 112(%rcx)\n\t"
        "movdqa %xmm8, 128(%rcx)\n\t"
        "movdqa %xmm9, 144(%rcx)\n\t"
        "movdqa %xmm10, 160(%rcx)\n\t"
        "movdqa %xmm11, 176(%rcx)\n\t"
        "movdqa %xmm12, 192(%rcx)\n\t"
        "movdqa %xmm13, 208(%rcx)\n\t"
        "movdqa %xmm14, 224(%rcx)\n\t"
        "movdqa %xmm15, 240(%rcx)\n\t"
        "xorl %eax, %eax\n\t"
        "retq\n\t");
}

// rcx = buffer, edx = value (0 becomes 1)
extern "C" __attribute__((naked)) void rt_longjmp(rt::JmpBuf*, int) {
    asm volatile(
        "movl %edx, %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "movl $1, %eax\n\t"
        "1:\n\t"
        "movq 0(%rcx), %rbx\n\t"
        "movq 8(%rcx), %rbp\n\t"
        "movq 16(%rcx), %rdi\n\t"
        "movq 24(%rcx), %rsi\n\t"
        "movq 40(%rcx), %r12\n\t"
        "movq 48(%rcx), %r13\n\t"
        "movq 56(%rcx), %r14\n\t"
        "movq 64(%rcx), %r15\n\t"
        "movdqa 96(%rcx), %xmm6\n\t"
        "movdqa 112(%rcx), %xmm7\n\t"
        "movdqa 128(%rcx), %xmm8\n\t"
        "movdqa 144(%rcx), %xmm9\n\t"
        "movdqa 160(%rcx), %xmm10\n\t"
        "movdqa 176(%rcx), %xmm11\n\t"
        "movdqa 192(%rcx), %xmm12\n\t"
        "movdqa 208(%rcx), %xmm13\n\t"
        "movdqa 224(%rcx), %xmm14\n\t"
        "movdqa 240(%rcx), %xmm15\n\t"
        "movq 32(%rcx), %rsp\n\t"
        "jmpq *72(%rcx)\n\t");
}
