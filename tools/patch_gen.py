#!/usr/bin/env python3
"""Post-lift patches for lifted Win64 CRT functions that use x87 fprem or inline jump tables."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MEMMOVE_BODY = """{
    /* Native CRT memcpy/memmove replacement (bypasses inline small-copy jump table). */
    void *dst = (void *)(uintptr_t)c_->rcx;
    const void *src = (const void *)(uintptr_t)c_->rdx;
    size_t n = (size_t)c_->r8;
    if (n) memmove(dst, src, n);
    c_->rax = c_->rcx;
    c_->rsp += 8;
}"""

MEMSET_BODY = """{
    /* Native CRT memset replacement (bypasses inline small-fill jump table). */
    void *dst = (void *)(uintptr_t)c_->rcx;
    int val = (int)(c_->rdx & 0xFFu);
    size_t n = (size_t)c_->r8;
    if (n) memset(dst, val, n);
    c_->rax = c_->rcx;
    c_->rsp += 8;
}"""

MEMCMP_BODY = """{
    /* Native CRT memcmp replacement (ARM64 NEON memcmp). */
    const void *a = (const void *)(uintptr_t)c_->rcx;
    const void *b = (const void *)(uintptr_t)c_->rdx;
    size_t n = (size_t)c_->r8;
    int r = n ? memcmp(a, b, n) : 0;
    c_->rax = (uint64_t)(int64_t)(r < 0 ? -1 : (r > 0 ? 1 : 0));
    c_->rsp += 8;
}"""

SKIN4_MAT_BODY = """{
    /* Native ARM64 NEON 4-bone matrix blend (Unity SkinMesh4 helper L_000140426D00). */
    float *out = (float *)(uintptr_t)c_->rcx;
    const float *bones = (const float *)(uintptr_t)c_->rdx;
    const uint8_t *wrec = (const uint8_t *)(uintptr_t)(c_->r8 + (c_->r9 << 5));
    const float *w = (const float *)wrec;
    const int32_t *idx = (const int32_t *)(wrec + 16);
    const float *b0 = bones + ((int64_t)idx[0] << 4);
    const float *b1 = bones + ((int64_t)idx[1] << 4);
    const float *b2 = bones + ((int64_t)idx[2] << 4);
    const float *b3 = bones + ((int64_t)idx[3] << 4);
#ifdef __ARM_NEON
    float32x4_t w0 = vdupq_n_f32(w[0]);
    float32x4_t w1 = vdupq_n_f32(w[1]);
    float32x4_t w2 = vdupq_n_f32(w[2]);
    float32x4_t w3 = vdupq_n_f32(w[3]);
    for (int col = 0; col < 16; col += 4) {
        float32x4_t r = vmulq_f32(vld1q_f32(b0 + col), w0);
        r = vfmaq_f32(r, vld1q_f32(b1 + col), w1);
        r = vfmaq_f32(r, vld1q_f32(b2 + col), w2);
        r = vfmaq_f32(r, vld1q_f32(b3 + col), w3);
        vst1q_f32(out + col, r);
    }
#else
    float w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];
    for (int k = 0; k < 16; k++)
        out[k] = b0[k]*w0 + b1[k]*w1 + b2[k]*w2 + b3[k]*w3;
#endif
    c_->rsp += 8;
}"""

SKIN4_PNT_BODY = """{
    /* Native ARM64 NEON 4-bone Pos+Norm+Tang skinning loop (Unity L_000140427B70). */
    int32_t count = (int32_t)R32(c_->rdx);
    if (count > 0) {
        const uint8_t *in_p = (const uint8_t *)(uintptr_t)c_->rcx;
        int64_t norm_off = (int64_t)(int32_t)R32(c_->r8);
        int64_t tang_off = (int64_t)(int32_t)R32(c_->r9);
        int64_t in_stride = (int64_t)(int32_t)rd32(c_->rsp + 0x28ull);
        uint8_t *out_p = (uint8_t *)(uintptr_t)rd64(c_->rsp + 0x30ull);
        int64_t out_stride = (int64_t)(int32_t)rd32(c_->rsp + 0x38ull);
        const float *bones = (const float *)(uintptr_t)rd64(c_->rsp + 0x40ull);
        const uint8_t *wbase = (const uint8_t *)(uintptr_t)rd64(c_->rsp + 0x48ull);
#ifdef __ARM_NEON
        static const uint32_t wmask_arr[4] = { 0u, 0u, 0u, 0xFFFFFFFFu };
        uint32x4_t wmask = vld1q_u32(wmask_arr);
        for (int32_t i = 0; i < count; i++, in_p += in_stride, out_p += out_stride, wbase += 32) {
            const float *w = (const float *)wbase;
            const int32_t *idx = (const int32_t *)(wbase + 16);
            const float *b0 = bones + ((int64_t)idx[0] << 4);
            const float *b1 = bones + ((int64_t)idx[1] << 4);
            const float *b2 = bones + ((int64_t)idx[2] << 4);
            const float *b3 = bones + ((int64_t)idx[3] << 4);
            float32x4_t w0 = vdupq_n_f32(w[0]);
            float32x4_t w1 = vdupq_n_f32(w[1]);
            float32x4_t w2 = vdupq_n_f32(w[2]);
            float32x4_t w3 = vdupq_n_f32(w[3]);
            float32x4_t c0 = vfmaq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(vld1q_f32(b0 + 0), w0),  vld1q_f32(b1 + 0), w1),  vld1q_f32(b2 + 0), w2),  vld1q_f32(b3 + 0), w3);
            float32x4_t c1 = vfmaq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(vld1q_f32(b0 + 4), w0),  vld1q_f32(b1 + 4), w1),  vld1q_f32(b2 + 4), w2),  vld1q_f32(b3 + 4), w3);
            float32x4_t c2 = vfmaq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(vld1q_f32(b0 + 8), w0),  vld1q_f32(b1 + 8), w1),  vld1q_f32(b2 + 8), w2),  vld1q_f32(b3 + 8), w3);
            float32x4_t c3 = vfmaq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(vld1q_f32(b0 + 12), w0), vld1q_f32(b1 + 12), w1), vld1q_f32(b2 + 12), w2), vld1q_f32(b3 + 12), w3);

            float32x4_t vp = vld1q_f32((const float *)in_p);
            float32x4_t vn = vld1q_f32((const float *)(in_p + norm_off));
            float32x4_t vt = vld1q_f32((const float *)(in_p + tang_off));

            float32x4_t op = vfmaq_laneq_f32(vfmaq_laneq_f32(vfmaq_laneq_f32(c3, c0, vp, 0), c1, vp, 1), c2, vp, 2);
            float32x4_t on = vfmaq_laneq_f32(vfmaq_laneq_f32(vmulq_laneq_f32(c0, vn, 0), c1, vn, 1), c2, vn, 2);
            float32x4_t ot = vfmaq_laneq_f32(vfmaq_laneq_f32(vmulq_laneq_f32(c0, vt, 0), c1, vt, 1), c2, vt, 2);

            op = vbslq_f32(wmask, vp, op);
            on = vbslq_f32(wmask, vn, on);
            ot = vbslq_f32(wmask, vt, ot);

            vst1q_f32((float *)out_p, op);
            vst1q_f32((float *)(out_p + norm_off), on);
            vst1q_f32((float *)(out_p + tang_off), ot);
        }
#else
        for (int32_t i = 0; i < count; i++, in_p += in_stride, out_p += out_stride, wbase += 32) {
            const float *w = (const float *)wbase;
            const int32_t *idx = (const int32_t *)(wbase + 16);
            const float *b0 = bones + ((int64_t)idx[0] << 4);
            const float *b1 = bones + ((int64_t)idx[1] << 4);
            const float *b2 = bones + ((int64_t)idx[2] << 4);
            const float *b3 = bones + ((int64_t)idx[3] << 4);
            float m[16];
            for (int k = 0; k < 16; k++)
                m[k] = b0[k]*w[0] + b1[k]*w[1] + b2[k]*w[2] + b3[k]*w[3];
            float vp[4], vn[4], vt[4], op[4], on[4], ot[4];
            memcpy(vp, in_p, 16); memcpy(vn, in_p + norm_off, 16); memcpy(vt, in_p + tang_off, 16);
            for (int j = 0; j < 3; j++) {
                op[j] = m[j]*vp[0] + m[4+j]*vp[1] + m[8+j]*vp[2] + m[12+j];
                on[j] = m[j]*vn[0] + m[4+j]*vn[1] + m[8+j]*vn[2];
                ot[j] = m[j]*vt[0] + m[4+j]*vt[1] + m[8+j]*vt[2];
            }
            op[3] = vp[3]; on[3] = vn[3]; ot[3] = vt[3];
            memcpy(out_p, op, 16); memcpy(out_p + norm_off, on, 16); memcpy(out_p + tang_off, ot, 16);
        }
#endif
    }
    c_->rsp += 8;
}"""

FMOD_BS_ADVANCE_HELPER = """static inline void fmod_bs_advance_native(uint8_t *bs, int32_t nbits)
{
    unsigned __int128 bits;
    memcpy(&bits, bs, 16);
    uint32_t un = (uint32_t)nbits;
    bits = (un < 128u) ? (bits >> un) : 0;
    int32_t avail = *(int32_t *)(bs + 0x10) - nbits;
    *(int32_t *)(bs + 0x10) = avail;
    memcpy(bs, &bits, 16);
    if (avail >= 64) return;
    int32_t rem = *(int32_t *)(bs + 0x40);
    if (rem <= 0) return;
    uint8_t *ptr = *(uint8_t **)(bs + 0x20);
    uint32_t sh = (uint32_t)(64 - avail);
    uint64_t lo, hi;
    memcpy(&lo, ptr, 8);
    memcpy(&hi, ptr + 8, 8);
    uint32_t half = (*(uint32_t *)(bs + 0x30)) ^= 1u;
    unsigned __int128 inc = ((unsigned __int128)(half ? hi : lo)) << 64;
    if (sh < 128u) bits |= (inc >> sh);
    memcpy(bs, &bits, 16);
    *(uint8_t **)(bs + 0x20) = ptr + ((size_t)half << 4);
    int32_t take = (rem < 8) ? rem : 8;
    *(int32_t *)(bs + 0x40) = rem - take;
    *(int32_t *)(bs + 0x10) = avail + (take << 3);
}"""

FMOD_BS_ADVANCE_BODY = """{
    /* Native 128-bit Vorbis bitreader advance+refill (replaces 2x128-case switch L_0001800022D0). */
    fmod_bs_advance_native((uint8_t *)(uintptr_t)c_->rcx, (int32_t)R32(c_->rdx));
    c_->rsp += 8;
}"""

FMOD_HUFFMAN_BODY = """{
    /* Native ARM64 Vorbis Huffman codeword decoder (FMOD L_0001800055A0). */
    const uint8_t *book = (const uint8_t *)(uintptr_t)c_->rcx;
    uint8_t *bs = (uint8_t *)(uintptr_t)c_->rdx;
    int32_t max_bits = *(const int8_t *)(book + 0x35);
    int32_t lut_bits = *(const int8_t *)(book + 0x3A);
    uint32_t word = *(const uint32_t *)bs;
    int32_t avail = *(const int32_t *)(bs + 0x10);
    int32_t peek;
    if (avail < lut_bits) peek = -1;
    else if (lut_bits <= 0) peek = 0;
    else peek = (int32_t)(word & (0xFFFFFFFFu >> (32 - lut_bits)));

    int32_t start_idx = 0;
    if (peek >= 0) {
        const uint16_t *lut = *(const uint16_t *const *)(book + 0x18);
        uint16_t e = lut[peek];
        if (!(e & 0x8000u)) {
            int32_t sym = (int32_t)e - 1;
            const int8_t *lens = *(const int8_t *const *)(book + 0x10);
            fmod_bs_advance_native(bs, (int32_t)lens[sym]);
            c_->rax = (uint64_t)(uint32_t)sym;
            c_->rsp += 8;
            return;
        }
        start_idx = (int32_t)(e & 0x7FFFu);
    }

    uint32_t bits;
    if (avail <= 0) {
        bits = 0xFFFFFFFFu;
    } else {
        int32_t k = (avail < max_bits) ? avail : max_bits;
        bits = (k <= 0) ? 0u : (word & (0xFFFFFFFFu >> (32 - k)));
    }
#if defined(__clang__) && defined(__aarch64__)
    uint32_t rev = __builtin_bitreverse32(bits);
#else
    uint32_t rev = (bits >> 16) | (bits << 16);
    rev = ((rev & 0xFF00FF00u) >> 8) | ((rev & 0x00FF00FFu) << 8);
    rev = ((rev & 0xF0F0F0F0u) >> 4) | ((rev & 0x0F0F0F0Fu) << 4);
    rev = ((rev & 0xCCCCCCCCu) >> 2) | ((rev & 0x33333333u) << 2);
    rev = ((rev & 0xAAAAAAAAu) >> 1) | ((rev & 0x55555555u) << 1);
#endif
    int32_t res;
    if (*(const uint8_t *)(book + 0x38) == 2) {
        int32_t n = *(const int16_t *)(book + 0x32);
        const uint16_t *tab = *(const uint16_t *const *)(book + 0x8);
        uint16_t key = (uint16_t)(rev >> 16);
        int32_t idx = start_idx & ~7;
        if (n - idx > 16) {
            int32_t lo = idx, hi = n;
            while (lo < hi) {
                int32_t mid = lo + ((hi - lo) >> 1);
                if (tab[mid] <= key) lo = mid + 1;
                else hi = mid;
            }
            res = lo - 1;
        } else {
            while (idx < n && tab[idx] <= key) idx++;
            res = idx - 1;
        }
    } else {
        const uint32_t *tab = *(const uint32_t *const *)(book + 0x8);
        uint32_t key = rev >> 1;
        int32_t idx = start_idx & ~3;
        while ((tab[idx] >> 1) <= key) idx++;
        res = idx - 1;
    }
    const int8_t *lens = *(const int8_t *const *)(book + 0x10);
    fmod_bs_advance_native(bs, (int32_t)lens[res]);
    c_->rax = (uint64_t)(uint32_t)res;
    c_->rsp += 8;
}"""

CHKSTK_BODY = """{
    /* Native __chkstk no-op: guest stacks are pre-mapped 16 MiB anonymous memory (no guard pages). */
    c_->rsp += 8;
}"""

FMODF_BODY = """{
    /* Native CRT fmodf replacement (bypasses x87 fprem). */
    float a = c_->xmm[0].f32[0];
    float b = c_->xmm[1].f32[0];
    c_->xmm[0].f32[0] = fmodf(a, b);
    c_->rsp += 8;
}"""

FMOD_BODY = """{
    /* Native CRT fmod replacement (bypasses x87 fprem). */
    double a = c_->xmm[0].f64[0];
    double b = c_->xmm[1].f64[0];
    c_->xmm[0].f64[0] = fmod(a, b);
    c_->rsp += 8;
}"""

TLS_GFX_BODY = """{
    /* Native GetGfxDevice() fast TLS lookup (bypasses c_shadow + dispatch + TlsGetValue). */
    extern uint64_t w64_tls_get(uint32_t i);
    c_->rax = w64_tls_get(rd32(GVA(0x1414470AC)));
    c_->rsp += 8;
}"""

PATCHES = [
    ("gen/TheForest/recomp_funcs_0023.c", "L_0001402C1C00", TLS_GFX_BODY, ""),
    ("gen/TheForest/recomp_funcs_0036.c", "L_000140426D00", SKIN4_MAT_BODY, "#ifdef __ARM_NEON\n#include <arm_neon.h>\n#endif"),
    ("gen/TheForest/recomp_funcs_0036.c", "L_000140427B70", SKIN4_PNT_BODY, "#include <string.h>\n#ifdef __ARM_NEON\n#include <arm_neon.h>\n#endif"),
    ("gen/TheForest/recomp_funcs_0093.c", "L_000140A0CB90", MEMMOVE_BODY, "#include <string.h>"),
    ("gen/TheForest/recomp_funcs_0093.c", "L_000140A0CFA0", MEMSET_BODY, "#include <string.h>"),
    ("gen/TheForest/recomp_funcs_0094.c", "L_000140A0F550", CHKSTK_BODY, ""),
    ("gen/TheForest/recomp_funcs_0094.c", "L_000140A10EE0", MEMCMP_BODY, "#include <string.h>"),
    ("gen/TheForest/recomp_funcs_0094.c", "L_000140A111C0", FMODF_BODY, "#include <math.h>"),
    ("gen/TheForest/recomp_funcs_0094.c", "L_000140A11690", FMOD_BODY, "#include <math.h>"),
    ("gen/fmod/recomp_funcs_0000.c", "L_0001800022D0", FMOD_BS_ADVANCE_BODY, "#include <stdint.h>\n#include <string.h>\n" + FMOD_BS_ADVANCE_HELPER),
    ("gen/fmod/recomp_funcs_0000.c", "L_0001800055A0", FMOD_HUFFMAN_BODY, ""),
    ("gen/fmod/recomp_funcs_0009.c", "L_000180100AF0", MEMSET_BODY, "#include <string.h>"),
    ("gen/fmod/recomp_funcs_0009.c", "L_000180100DC0", MEMMOVE_BODY, "#include <string.h>"),
    ("gen/fmodstudio/recomp_funcs_0021.c", "L_00018113F660", MEMSET_BODY, "#include <string.h>"),
    ("gen/fmodstudio/recomp_funcs_0021.c", "L_00018113F0E0", MEMMOVE_BODY, "#include <string.h>"),
]


def patch_func(path, fn, body, inc):
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        print(f"SKIP (missing): {path}")
        return
    text = open(full, "r").read()
    sig = f"void {fn}(CPU *c_)"
    idx = text.find(sig)
    if idx < 0:
        raise RuntimeError(f"{fn} not found in {path}")
    brace_start = text.find("{", idx)
    depth = 0
    end = -1
    for i in range(brace_start, len(text)):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end < 0:
        raise RuntimeError(f"Unmatched braces for {fn} in {path}")
    if inc and inc not in text[:1024]:
        text = inc + "\n" + text
        shift = len(inc) + 1
        brace_start += shift
        end += shift
    new_text = text[:brace_start] + body + text[end:]
    if new_text != text:
        open(full, "w").write(new_text)
        print(f"Patched {fn} in {path}")
    else:
        print(f"Already patched {fn} in {path}")


def patch_o0_tail(path, start_fn):
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        print(f"SKIP (missing): {path}")
        return
    text = open(full, "r").read()
    if "#pragma clang optimize off" in text:
        print(f"Already pragmad {path}")
        return
    marker = f"void {start_fn}(CPU *c_)"
    if marker not in text:
        raise RuntimeError(f"{start_fn} not found in {path}")
    text = text.replace(marker, "#pragma clang optimize off\n" + marker, 1) + "\n#pragma clang optimize on\n"
    open(full, "w").write(text)
    print(f"Applied optnone tail from {start_fn} in {path}")


def main():
    for path, fn, body, inc in PATCHES:
        patch_func(path, fn, body, inc)
    patch_o0_tail("gen/TheForest/recomp_funcs_0018.c", "L_000140237E10")


if __name__ == "__main__":
    main()
