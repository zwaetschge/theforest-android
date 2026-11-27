/*
 * fmt.c - printf formatting for guest varargs / Win64 va_list.
 *
 * Win64 passes every variadic argument in an 8-byte slot (doubles too, as raw
 * bits), and `long` is 32 bits. Each conversion is rebuilt as a host format
 * spec and formatted with the host snprintf.
 */
#include "w64.h"

static void put(char *out, size_t cap, size_t *n, const char *s, size_t len)
{
    if (*n < cap) {
        size_t k = cap - *n - 1 < len ? cap - *n - 1 : len;
        if (cap) memcpy(out + *n, s, k);
    }
    *n += len;
}

int w64_vformat(char *out, size_t cap, const char *fmt, w64_va *v)
{
    size_t n = 0;
    if (!fmt) fmt = "(null)";
    for (const char *p = fmt; *p; ) {
        if (*p != '%') {
            const char *q = strchr(p, '%');
            size_t len = q ? (size_t)(q - p) : strlen(p);
            put(out, cap, &n, p, len);
            p += len;
            continue;
        }
        const char *start = p++;
        if (*p == '%') { put(out, cap, &n, "%", 1); p++; continue; }
        char spec[64];
        size_t sl = 0;
        spec[sl++] = '%';
        while (*p && strchr("-+ #0'", *p)) { if (sl < 40) spec[sl++] = *p; p++; }
        if (*p == '*') { int w = (int)(int32_t)w64_va_next(v); sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", w); p++; }
        else while (*p >= '0' && *p <= '9') { if (sl < 50) spec[sl++] = *p; p++; }
        if (*p == '.') {
            spec[sl++] = '.'; p++;
            if (*p == '*') { int pr = (int)(int32_t)w64_va_next(v); sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", pr); p++; }
            else while (*p >= '0' && *p <= '9') { if (sl < 56) spec[sl++] = *p; p++; }
        }
        int len64 = 0, lenh = 0, wide = 0;
        for (;;) {
            if (*p == 'l') { if (p[1] == 'l') { len64 = 1; p += 2; } else { wide = 1; p++; } }   /* l = 32-bit long / wide char */
            else if (*p == 'h') { lenh++; p++; }
            else if (*p == 'z' || *p == 'j' || *p == 't' || *p == 'q' || *p == 'L') { len64 = 1; p++; }
            else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { len64 = 1; p += 3; }
            else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
            else if (*p == 'I') { len64 = 1; p++; }
            else if (*p == 'w') { wide = 1; p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        char buf[512];
        int k = 0;
        switch (conv) {
        case 'd': case 'i': {
            uint64_t a = w64_va_next(v);
            long long x = len64 ? (long long)(int64_t)a : lenh == 1 ? (short)a : lenh >= 2 ? (signed char)a : (int32_t)a;
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            k = snprintf(buf, sizeof buf, spec, x);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t a = w64_va_next(v);
            unsigned long long x = len64 ? a : lenh == 1 ? (unsigned short)a : lenh >= 2 ? (unsigned char)a : (uint32_t)a;
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            k = snprintf(buf, sizeof buf, spec, x);
            break;
        }
        case 'c': {
            uint64_t a = w64_va_next(v);
            spec[sl++] = 'c'; spec[sl] = 0;
            k = snprintf(buf, sizeof buf, spec, (int)(a & 0xFF));
            break;
        }
        case 'p': {
            uint64_t a = w64_va_next(v);
            k = snprintf(buf, sizeof buf, "0x%016llx", (unsigned long long)a);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t a = w64_va_next(v);
            double d; memcpy(&d, &a, 8);
            spec[sl++] = conv; spec[sl] = 0;
            k = snprintf(buf, sizeof buf, spec, d);
            break;
        }
        case 's': case 'S': {
            uint64_t a = w64_va_next(v);
            spec[sl++] = 's'; spec[sl] = 0;
            if (!a) { k = snprintf(buf, sizeof buf, spec, "(null)"); break; }
            if (wide || conv == 'S') {
                char *u = w16_to_utf8((const uint16_t *)(uintptr_t)a, -1);
                int need = snprintf(NULL, 0, spec, u);
                char *t = malloc((size_t)need + 1);
                snprintf(t, (size_t)need + 1, spec, u);
                put(out, cap, &n, t, (size_t)need);
                free(t); free(u);
                continue;
            } else {
                const char *s = (const char *)(uintptr_t)a;
                int need = snprintf(NULL, 0, spec, s);
                if (need < (int)sizeof buf) { k = snprintf(buf, sizeof buf, spec, s); break; }
                char *t = malloc((size_t)need + 1);
                snprintf(t, (size_t)need + 1, spec, s);
                put(out, cap, &n, t, (size_t)need);
                free(t);
                continue;
            }
        }
        case 'n': {
            uint64_t a = w64_va_next(v);
            if (a) wr32(a, (uint32_t)n);
            continue;
        }
        default:
            put(out, cap, &n, start, (size_t)(p - start));
            continue;
        }
        if (k > 0) put(out, cap, &n, buf, (size_t)(k < (int)sizeof buf ? k : (int)sizeof buf - 1));
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return (int)n;
}
