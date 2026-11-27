/*
 * w64.h - Win64 guest runtime (House Party static recompilation; derived from the Mewgenics port).
 *
 * The lifted code (gen/) runs on a CPU struct and calls everything through
 * dispatch(). Calls that leave the game - IAT imports, functions handed out by
 * GetProcAddress, the SDL3 dynapi jump table and OpenGL entry points - land on
 * "host functions": C functions with the signature `void fn(CPU *c)` that read
 * their arguments with the Win64 calling convention and return like a guest
 * `ret` does (pop the return address).
 *
 * Memory is identity mapped: a guest pointer is a host pointer.
 */
#pragma once
#include "cpu64.h"
#include "w64_gen.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*w64_fn)(CPU *c);

/* ---- arguments / return (valid at host-function entry: [rsp] = return address) ---- */
static inline uint64_t w64_arg(CPU *c, int i)
{
    switch (i) {
    case 0: return c->rcx;
    case 1: return c->rdx;
    case 2: return c->r8;
    case 3: return c->r9;
    }
    return rd64(c->rsp + 8 + 8 * (uint64_t)i);
}
static inline float w64_argf(CPU *c, int i)
{
    if (i < 4) return c->xmm[i].f32[0];
    uint32_t b = rd32(c->rsp + 8 + 8 * (uint64_t)i); float f; memcpy(&f, &b, 4); return f;
}
static inline double w64_argd(CPU *c, int i)
{
    if (i < 4) return c->xmm[i].f64[0];
    uint64_t b = rd64(c->rsp + 8 + 8 * (uint64_t)i); double d; memcpy(&d, &b, 8); return d;
}
#define A(i)   w64_arg(c, (i))
#define AP(i)  ((void *)(uintptr_t)w64_arg(c, (i)))
#define AS(i)  ((const char *)(uintptr_t)w64_arg(c, (i)))
#define AW(i)  ((const uint16_t *)(uintptr_t)w64_arg(c, (i)))
#define AI(i)  ((int32_t)w64_arg(c, (i)))
#define AU(i)  ((uint32_t)w64_arg(c, (i)))
#define AF(i)  w64_argf(c, (i))
#define AD(i)  w64_argd(c, (i))

#define RET(v)   do { c->rax = (uint64_t)(v); c->rsp += 8; return; } while (0)
#define RETV()   do { c->rsp += 8; return; } while (0)
#define RETF(v)  do { c->xmm[0].f32[0] = (float)(v); c->xmm[0].f32[1] = 0; c->xmm[0].u64[1] = 0; c->rsp += 8; return; } while (0)
#define RETD(v)  do { c->xmm[0].f64[0] = (double)(v); c->xmm[0].u64[1] = 0; c->rsp += 8; return; } while (0)

/* ---- host-function registry ---- */
typedef struct { const char *dll; const char *name; w64_fn fn; } w64_export_t;
/* Each HLE module defines a table terminated by {0}. */
void w64_register(const w64_export_t *tab);
w64_fn w64_find(const char *dll, const char *name);          /* dll may be NULL = any */
/* Sentinel guest address for a host function (allocated once per fn). */
uint64_t w64_hostfn(const char *name, w64_fn fn);
uint64_t w64_hostfn_named(const char *dll, const char *name); /* 0 if not implemented */
const char *w64_hostfn_name(uint64_t va);

#define W64_SENT_SLOTS 0x10000u                 /* imports + dynamic host functions */
#define W64_IS_SENTINEL(va) ((uint64_t)(va) - HLE_BASE < (uint64_t)W64_SENT_SLOTS * HLE_STRIDE)

/* ---- host -> guest calls ---- */
uint64_t w64_call(CPU *c, uint64_t fn, int n, ...);           /* integer/pointer args */
uint64_t w64_callv(CPU *c, uint64_t fn, int n, const uint64_t *args, unsigned float_mask);
uint64_t w64_callv_fn(CPU *c, w64_fn host_fn, uint64_t va, int n, const uint64_t *args, unsigned float_mask);
w64_fn   w64_lookup_fn(uint64_t va);
CPU *w64_cpu(void);                  /* this host thread's guest CPU (created on demand) */
void w64_gc_push_guest_roots(void (*push_fn)(void *lo, void *hi));
extern uint32_t g_w64_com_calls;
extern uint32_t g_w64_tramp_calls;
extern int g_w64_prof_enabled;

/* ---- modules (modules.c) ---- */
#define W64_MAX_MODULES 16
typedef struct {
    const w64_modspec_t *spec;
    uint64_t base, size, entry;
    int mapped, attached, is_dll, tls_index, load_order, no_thread_calls;
    uint64_t tls_start, tls_end, tls_zero, tls_index_va, tls_callbacks;
    uint32_t exp_rva, exp_size, pdata_rva, pdata_size;
} w64_module_t;
extern w64_module_t g_mods[W64_MAX_MODULES];
extern int g_nmods;
w64_module_t *w64_module_find(const char *name);
w64_module_t *w64_module_by_va(uint64_t va);
w64_module_t *w64_module_by_base(uint64_t base);
uint64_t w64_module_export(w64_module_t *m, const char *name);   /* name may be an ordinal (< 0x10000) */
int  w64_module_attach(w64_module_t *m);
int  w64_modules_init(const char *gamedir);
void w64_modules_thread_attach(CPU *c);
uint64_t w64_tls_array_new(void);
void w64_tls_array_free(uint64_t arr);

/* ---- image (the executable, g_mods[0]) ---- */
extern uint64_t w64_image_base, w64_image_size, w64_entry;
int  w64_load(const char *gamedir);
int  w64_run_entry(void);
void w64_thread_attach(CPU *c);      /* TEB + TLS for the calling host thread */
void w64_thread_detach(void);        /* free them again when a guest thread ends */
uint64_t w64_thread_id(void);        /* guest thread id (TEB) */

/* ---- printf with Win64 argument passing (fmt.c): the remaining register/stack arguments of the
 * current host-function call, or a Win64 va_list (a pointer to consecutive 8-byte slots) ---- */
typedef struct { CPU *c; int idx; uint64_t va; } w64_va;
static inline uint64_t w64_va_next(w64_va *v)
{
    if (v->c) return w64_arg(v->c, v->idx++);
    uint64_t r = rd64(v->va); v->va += 8; return r;
}
int w64_vformat(char *out, size_t cap, const char *fmt, w64_va *v);   /* returns full length like vsnprintf */

/* ---- logging ---- */
extern int w64_trace;                /* W64_TRACE: 1 = imports, 2 = + SDL/GL */
void w64_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void w64_fatal(CPU *c, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
void w64_backtrace(CPU *c);

/* ---- strings ---- */
size_t w16len(const uint16_t *s);
char  *w16_to_utf8(const uint16_t *s, int len);               /* malloc'd; len<0 = NUL-terminated */
int    utf8_to_w16(const char *s, uint16_t *out, int cap);    /* returns units written incl. NUL */
uint16_t *utf8_to_w16_dup(const char *s);

/* ---- paths ---- */
extern char w64_gamedir[1024];       /* host directory holding resources.gpak */
extern char w64_userdir[1024];       /* host directory for saves/config (C:\ maps here) */
void w64_host_path(const char *win, char *out, size_t cap);   /* Windows path -> host path */

/* ---- handles ---- */
enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_SEM, H_THREAD, H_FIND, H_MAPPING, H_HEAP, H_MODULE, H_OTHER };
typedef struct w64_obj w64_obj;
uint64_t w64_handle_new(int type, void *obj);
/* native_call.c: calls from guest code into native (DXVK) object methods */
int w64_native_code(uint64_t va);
void w64_native_call(CPU *c, uint64_t fn);
void    *w64_handle_get(uint64_t h, int type);
int      w64_handle_type(uint64_t h);
void     w64_handle_close(uint64_t h);

/* last error lives in the TEB like on Windows */
void     w64_set_last_error(uint32_t e);
uint32_t w64_get_last_error(void);

/* time */
uint64_t w64_now_ns(void);

/* the TEB of the calling thread */
extern __thread uint8_t *w64_teb;
#define TEB_STACK_BASE   0x08
#define TEB_STACK_LIMIT  0x10
#define TEB_SELF         0x30
#define TEB_PID          0x40
#define TEB_TID          0x48
#define TEB_TLS_PTR      0x58
#define TEB_PEB          0x60
#define TEB_LAST_ERROR   0x68
#define TEB_TLS_SLOTS    0x1480
#define TEB_TLS_EXP      0x1780
#define TEB_SIZE         0x2000
