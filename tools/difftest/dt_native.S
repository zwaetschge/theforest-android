# dt_native.S - run one x86-64 instruction on the real CPU (Linux/SysV port of
# pcrecomp's difftest64.asm).
#
# Same contract as the MASM original: every load/store is RIP-relative to a
# global, so no GPR is tied up holding a pointer and all fifteen (RSP excluded)
# are both set from the input and captured on the way out. Flags are set with
# POPFQ BEFORE the GPRs are loaded (MOV does not touch flags) and captured with
# PUSHFQ before anything else runs.
#
# Additions over the original: mm0-7 are loaded/stored too (MMX instructions),
# and EMMS runs after the capture so the x87 tag word is empty again before C
# code runs (SysV requires the x87 stack empty at a call boundary).
#
# SysV: rbx rbp r12-r15 are callee-saved; all xmm registers are caller-saved,
# so (unlike Win64) xmm6-15 need no preservation. A fault inside the test
# instruction is recovered with siglongjmp in the C driver, which restores the
# callee-saved registers and rsp from the sigsetjmp in the caller.
#
# DTSTATE layout (must match dtseeds.h):
#   uint64_t gpr[16];   0
#   uint64_t flags;     128
#   uint64_t _pad;      136
#   XMM      xmm[16];   144
#   uint64_t mm[8];     400

        .intel_syntax noprefix
        .set G_RAX,   0
        .set G_RCX,   8
        .set G_RDX,  16
        .set G_RBX,  24
        .set G_RBP,  40
        .set G_RSI,  48
        .set G_RDI,  56
        .set G_R8,   64
        .set G_R9,   72
        .set G_R10,  80
        .set G_R11,  88
        .set G_R12,  96
        .set G_R13, 104
        .set G_R14, 112
        .set G_R15, 120
        .set G_FLAGS, 128
        .set G_XMM, 144
        .set G_MM,  400

        .text
        .globl  run_native
        .type   run_native, @function
run_native:
        push    rbx
        push    rbp
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, 8                  # keep the callee's rsp ABI-aligned

        movups  xmm0,  XMMWORD PTR [rip + dt_in + G_XMM + 16*0]
        movups  xmm1,  XMMWORD PTR [rip + dt_in + G_XMM + 16*1]
        movups  xmm2,  XMMWORD PTR [rip + dt_in + G_XMM + 16*2]
        movups  xmm3,  XMMWORD PTR [rip + dt_in + G_XMM + 16*3]
        movups  xmm4,  XMMWORD PTR [rip + dt_in + G_XMM + 16*4]
        movups  xmm5,  XMMWORD PTR [rip + dt_in + G_XMM + 16*5]
        movups  xmm6,  XMMWORD PTR [rip + dt_in + G_XMM + 16*6]
        movups  xmm7,  XMMWORD PTR [rip + dt_in + G_XMM + 16*7]
        movups  xmm8,  XMMWORD PTR [rip + dt_in + G_XMM + 16*8]
        movups  xmm9,  XMMWORD PTR [rip + dt_in + G_XMM + 16*9]
        movups  xmm10, XMMWORD PTR [rip + dt_in + G_XMM + 16*10]
        movups  xmm11, XMMWORD PTR [rip + dt_in + G_XMM + 16*11]
        movups  xmm12, XMMWORD PTR [rip + dt_in + G_XMM + 16*12]
        movups  xmm13, XMMWORD PTR [rip + dt_in + G_XMM + 16*13]
        movups  xmm14, XMMWORD PTR [rip + dt_in + G_XMM + 16*14]
        movups  xmm15, XMMWORD PTR [rip + dt_in + G_XMM + 16*15]

        movq    mm0, QWORD PTR [rip + dt_in + G_MM + 8*0]
        movq    mm1, QWORD PTR [rip + dt_in + G_MM + 8*1]
        movq    mm2, QWORD PTR [rip + dt_in + G_MM + 8*2]
        movq    mm3, QWORD PTR [rip + dt_in + G_MM + 8*3]
        movq    mm4, QWORD PTR [rip + dt_in + G_MM + 8*4]
        movq    mm5, QWORD PTR [rip + dt_in + G_MM + 8*5]
        movq    mm6, QWORD PTR [rip + dt_in + G_MM + 8*6]
        movq    mm7, QWORD PTR [rip + dt_in + G_MM + 8*7]

        push    QWORD PTR [rip + dt_in + G_FLAGS]
        popfq

        mov     rax, QWORD PTR [rip + dt_in + G_RAX]
        mov     rcx, QWORD PTR [rip + dt_in + G_RCX]
        mov     rdx, QWORD PTR [rip + dt_in + G_RDX]
        mov     rbx, QWORD PTR [rip + dt_in + G_RBX]
        mov     rbp, QWORD PTR [rip + dt_in + G_RBP]
        mov     rsi, QWORD PTR [rip + dt_in + G_RSI]
        mov     rdi, QWORD PTR [rip + dt_in + G_RDI]
        mov     r8,  QWORD PTR [rip + dt_in + G_R8]
        mov     r9,  QWORD PTR [rip + dt_in + G_R9]
        mov     r10, QWORD PTR [rip + dt_in + G_R10]
        mov     r11, QWORD PTR [rip + dt_in + G_R11]
        mov     r12, QWORD PTR [rip + dt_in + G_R12]
        mov     r13, QWORD PTR [rip + dt_in + G_R13]
        mov     r14, QWORD PTR [rip + dt_in + G_R14]
        mov     r15, QWORD PTR [rip + dt_in + G_R15]

        # ---- the instruction under test, as <bytes> RET ----
        call    QWORD PTR [rip + dt_code]

        pushfq
        pop     QWORD PTR [rip + dt_out + G_FLAGS]

        mov     QWORD PTR [rip + dt_out + G_RAX], rax
        mov     QWORD PTR [rip + dt_out + G_RCX], rcx
        mov     QWORD PTR [rip + dt_out + G_RDX], rdx
        mov     QWORD PTR [rip + dt_out + G_RBX], rbx
        mov     QWORD PTR [rip + dt_out + G_RBP], rbp
        mov     QWORD PTR [rip + dt_out + G_RSI], rsi
        mov     QWORD PTR [rip + dt_out + G_RDI], rdi
        mov     QWORD PTR [rip + dt_out + G_R8],  r8
        mov     QWORD PTR [rip + dt_out + G_R9],  r9
        mov     QWORD PTR [rip + dt_out + G_R10], r10
        mov     QWORD PTR [rip + dt_out + G_R11], r11
        mov     QWORD PTR [rip + dt_out + G_R12], r12
        mov     QWORD PTR [rip + dt_out + G_R13], r13
        mov     QWORD PTR [rip + dt_out + G_R14], r14
        mov     QWORD PTR [rip + dt_out + G_R15], r15

        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*0],  xmm0
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*1],  xmm1
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*2],  xmm2
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*3],  xmm3
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*4],  xmm4
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*5],  xmm5
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*6],  xmm6
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*7],  xmm7
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*8],  xmm8
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*9],  xmm9
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*10], xmm10
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*11], xmm11
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*12], xmm12
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*13], xmm13
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*14], xmm14
        movups  XMMWORD PTR [rip + dt_out + G_XMM + 16*15], xmm15

        movq    QWORD PTR [rip + dt_out + G_MM + 8*0], mm0
        movq    QWORD PTR [rip + dt_out + G_MM + 8*1], mm1
        movq    QWORD PTR [rip + dt_out + G_MM + 8*2], mm2
        movq    QWORD PTR [rip + dt_out + G_MM + 8*3], mm3
        movq    QWORD PTR [rip + dt_out + G_MM + 8*4], mm4
        movq    QWORD PTR [rip + dt_out + G_MM + 8*5], mm5
        movq    QWORD PTR [rip + dt_out + G_MM + 8*6], mm6
        movq    QWORD PTR [rip + dt_out + G_MM + 8*7], mm7
        emms

        add     rsp, 8
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbp
        pop     rbx
        ret
        .size   run_native, .-run_native

        .section .note.GNU-stack,"",@progbits
