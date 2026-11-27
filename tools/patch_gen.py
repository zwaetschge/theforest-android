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

PATCHES = [
    ("gen/TheForest/recomp_funcs_0094.c", "L_000140A111C0", FMODF_BODY, "#include <math.h>"),
    ("gen/TheForest/recomp_funcs_0094.c", "L_000140A11690", FMOD_BODY, "#include <math.h>"),
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


def main():
    for path, fn, body, inc in PATCHES:
        patch_func(path, fn, body, inc)


if __name__ == "__main__":
    main()
