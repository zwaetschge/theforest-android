/*
 * mono_tramp.S - Static native-to-Win64 trampoline pool (8192 slots) for Mono icalls & P/Invoke.
 *
 * No RWX pages and no libffi needed: each slot loads its index and branches to a single common
 * stub that saves all host ABI argument registers into mono_tramp_frame_t, calls
 * w64_mono_tramp_dispatch(slot, &frame), and restores host ABI return registers.
 *
 * Layout of mono_tramp_frame_t (matches runtime/src/mono_bridge.c):
 *   0..71   : uint64_t gp[9]      (x86_64: rdi,rsi,rdx,rcx,r8,r9 | aarch64: x0..x8)
 *   72..135 : uint64_t fp[8]      (x86_64: xmm0..xmm7 low 64b    | aarch64: d0..d7)
 *   136     : uint64_t *stack     (caller stack args)
 *   144..159: uint64_t ret_gp[2]  (x86_64: rax, rdx              | aarch64: x0, x1)
 *   160..191: uint64_t ret_fp[4]  (x86_64: xmm0, xmm1 low 64b    | aarch64: d0..d3)
 */

#if defined(__x86_64__)
.text
.intel_syntax noprefix

.globl w64_mono_tramp_table
.globl w64_mono_tramp_stride
.extern w64_mono_tramp_dispatch

.balign 16
w64_mono_tramp_common:
    push rbp
    mov rbp, rsp
    sub rsp, 256
    mov [rsp + 0], rdi
    mov [rsp + 8], rsi
    mov [rsp + 16], rdx
    mov [rsp + 24], rcx
    mov [rsp + 32], r8
    mov [rsp + 40], r9
    mov qword ptr [rsp + 48], 0
    mov qword ptr [rsp + 56], 0
    mov qword ptr [rsp + 64], 0
    movq [rsp + 72], xmm0
    movq [rsp + 80], xmm1
    movq [rsp + 88], xmm2
    movq [rsp + 96], xmm3
    movq [rsp + 104], xmm4
    movq [rsp + 112], xmm5
    movq [rsp + 120], xmm6
    movq [rsp + 128], xmm7
    lea r10, [rbp + 16]
    mov [rsp + 136], r10
    mov qword ptr [rsp + 144], 0
    mov qword ptr [rsp + 152], 0
    mov qword ptr [rsp + 160], 0
    mov qword ptr [rsp + 168], 0
    mov qword ptr [rsp + 176], 0
    mov qword ptr [rsp + 184], 0
    mov edi, eax
    mov rsi, rsp
    call w64_mono_tramp_dispatch
    mov rax, [rsp + 144]
    mov rdx, [rsp + 152]
    movq xmm0, [rsp + 160]
    movq xmm1, [rsp + 168]
    leave
    ret

.balign 16
w64_mono_tramp_table:
.set _i, 0
.rept 8192
    .balign 16
    mov eax, _i
    jmp w64_mono_tramp_common
    .set _i, _i + 1
.endr

.section .rodata
w64_mono_tramp_stride:
    .long 16

#elif defined(__aarch64__)
.text

.globl w64_mono_tramp_table
.globl w64_mono_tramp_stride
.extern w64_mono_tramp_dispatch

.balign 16
w64_mono_tramp_common:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    str x8, [sp, #64]
    stp d0, d1, [sp, #72]
    stp d2, d3, [sp, #88]
    stp d4, d5, [sp, #104]
    stp d6, d7, [sp, #120]
    add x9, x29, #16
    str x9, [sp, #136]
    stp xzr, xzr, [sp, #144]
    stp xzr, xzr, [sp, #160]
    stp xzr, xzr, [sp, #176]
    mov w0, w16
    mov x1, sp
    bl w64_mono_tramp_dispatch
    ldp x0, x1, [sp, #144]
    ldp d0, d1, [sp, #160]
    ldp d2, d3, [sp, #176]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret

.balign 16
w64_mono_tramp_table:
.set _i, 0
.rept 8192
    mov w16, #_i
    b w64_mono_tramp_common
    .set _i, _i + 1
.endr

.section .rodata
w64_mono_tramp_stride:
    .long 8
#else
#error "Unsupported architecture for mono_tramp.S"
#endif

.section .note.GNU-stack,"",@progbits
