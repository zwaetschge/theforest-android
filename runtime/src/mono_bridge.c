/*
 * mono_bridge.c - Native Unity Mono 4.8 (unity-5.6-mbe) bridge for statically recompiled Win64 Unity.
 *
 * Bridges two directions in a shared 64-bit address space:
 *   1) Lifted Win64 guest (TheForest.exe) -> native libmonoboehm-2.0 (215 mono_* + g_free exports)
 *   2) Native libmonoboehm-2.0 (C# icalls & P/Invoke) -> lifted Win64 guest functions via static
 *      trampoline pool (runtime/src/mono_tramp.S, 8192 slots, zero RWX closures, zero libffi).
 */
#define _GNU_SOURCE
#include "w64.h"
#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mono/mini/jit.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/attrdefs.h>
#include <mono/metadata/class.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/object.h>
#include <mono/metadata/reflection.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/tokentype.h>
#include <mono/utils/mono-dl-fallback.h>
#include <mono/utils/mono-logger.h>

/* Unity-specific Mono APIs from third_party/mono/mono/metadata/unity-*.c */
extern void g_free(void *ptr);
extern int mono_unity_class_is_abstract(MonoClass *klass);
extern int mono_unity_class_is_interface(MonoClass *klass);
extern void *mono_class_get_userdata(MonoClass *klass);
extern void mono_class_set_userdata(MonoClass *klass, void *userdata);
extern int mono_class_get_userdata_offset(void);
extern void *mono_unity_liveness_allocate_struct(MonoClass *filter, uint32_t max_count, void *cb, void *ud);
extern void *mono_unity_liveness_calculation_begin(MonoClass *filter, uint32_t max_count, void *cb, void *ud, void *onWorldStart, void *onWorldStop);
extern void mono_unity_liveness_calculation_end(void *state);
extern void mono_unity_liveness_calculation_from_root(MonoObject *root, void *state);
extern void mono_unity_liveness_calculation_from_statics(void *state);
extern void mono_unity_liveness_finalize(void *state);
extern void mono_unity_liveness_free_struct(void *state);
extern void mono_unity_liveness_start_gc_world(void);
extern void mono_unity_liveness_stop_gc_world(void);
extern void mono_unity_set_embeddinghostname(const char *name);
extern void mono_unity_set_unhandled_exception_handler(void *handler);
extern void mono_unity_set_vprintf_func(void *func);
extern void mono_unity_socket_security_enabled_set(int enabled);
extern void mono_set_find_plugin_callback(void *cb);
extern void mono_security_enable_core_clr(void);
extern void mono_security_set_mode(uint32_t mode);
extern void mono_verifier_set_mode(uint32_t mode);
extern void mono_thread_pop_appdomain_ref(void);
extern void mono_thread_push_appdomain_ref(void);
extern void mono_thread_suspend_all_other_threads(void);
extern void mono_threads_set_shutting_down(void);
extern void mono_thread_pool_cleanup(void);
extern void mono_runtime_set_shutting_down(void);
extern int mono_runtime_is_shutting_down(void);
extern void mono_runtime_unhandled_exception_policy_set(int policy);
extern void mono_gc_wbarrier_generic_store(void *ptr, MonoObject *value);
extern char *mono_pmip(void *ip);
extern uint32_t mono_parse_default_optimizations(const char *p);
extern void mono_set_signal_chaining(int chain_signals);
extern void mono_set_defaults(int verbose_level, uint32_t opts);
extern MonoClass *mono_class_get_nesting_type(MonoClass *klass);
extern MonoClass *mono_class_get_nested_types(MonoClass *klass, void **iter);
extern int mono_class_is_generic(MonoClass *klass);
extern int mono_class_is_inflated(MonoClass *klass);
extern int unity_mono_method_is_generic(MonoMethod *method);
extern int unity_mono_method_is_inflated(MonoMethod *method);
extern MonoMethod *mono_method_get_last_managed(void);
extern MonoMethod *unity_mono_reflection_method_get_method(MonoObject *rm);
#define mono_method_is_generic unity_mono_method_is_generic
#define mono_method_is_inflated unity_mono_method_is_inflated
#define mono_reflection_method_get_method unity_mono_reflection_method_get_method
extern MonoObject *mono_object_new_specific(MonoVTable *vtable);
extern MonoObject *mono_object_new_alloc_specific(MonoVTable *vtable);
extern MonoClass *mono_array_class_get(MonoClass *eclass, uint32_t rank);
extern int32_t mono_class_array_element_size(MonoClass *ac);
extern MonoArray *mono_array_new_full(MonoDomain *domain, MonoClass *array_class, uintptr_t *lengths, intptr_t *lower_bounds);
extern MonoDomain *mono_domain_create_appdomain(char *friendly_name, char *configuration_file);
extern int mono_domain_finalize(MonoDomain *domain, uint32_t timeout);
extern void mono_domain_unload(MonoDomain *domain);
extern int mono_gchandle_is_in_domain(uint32_t gchandle, MonoDomain *domain);
extern MonoString *mono_string_from_utf16(uint16_t *data);
extern uint16_t *mono_string_to_utf16(MonoString *s);
extern int mono_metadata_signature_equal(MonoMethodSignature *sig1, MonoMethodSignature *sig2);
extern int mono_assembly_name_parse(const char *name, MonoAssemblyName *aname);

extern uint64_t w64_lookup_export_va(const char *dll, const char *name);

#ifndef FIELD_ATTRIBUTE_STATIC
#define FIELD_ATTRIBUTE_STATIC  0x0010
#define FIELD_ATTRIBUTE_LITERAL 0x0040
#endif

static CPU *ensure_cpu(void)
{
    return w64_cpu();
}

/* ============================================================================
 * BCL directory redirection (Mono 4.8 corlib v156 for Unity 5.6 games)
 * ============================================================================ */
static char g_bcl_dir[1024];

static const char *bcl_dir(void)
{
    if (g_bcl_dir[0]) return g_bcl_dir;
    const char *env = getenv("W64_MONO_BCL");
    if (env && *env) { snprintf(g_bcl_dir, sizeof g_bcl_dir, "%s", env); return g_bcl_dir; }
    char cand[1024];
    snprintf(cand, sizeof cand, "%s/mono_bcl", w64_userdir);
    struct stat st;
    if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) { snprintf(g_bcl_dir, sizeof g_bcl_dir, "%s", cand); return g_bcl_dir; }
    snprintf(cand, sizeof cand, "%s/mono_bcl", w64_gamedir);
    if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) { snprintf(g_bcl_dir, sizeof g_bcl_dir, "%s", cand); return g_bcl_dir; }
    if (stat("/w/build/mono-bcl", &st) == 0 && S_ISDIR(st.st_mode)) { snprintf(g_bcl_dir, sizeof g_bcl_dir, "/w/build/mono-bcl"); return g_bcl_dir; }
    if (stat("build/mono-bcl", &st) == 0 && S_ISDIR(st.st_mode)) { snprintf(g_bcl_dir, sizeof g_bcl_dir, "build/mono-bcl"); return g_bcl_dir; }
    return "";
}

void w64_mono_translate_path(const char *in, char *out, size_t cap)
{
    if (!in) { if (cap) out[0] = 0; return; }
    w64_host_path(in, out, cap);
}

static void mono_path_to_host(const char *in, char *out, size_t cap)
{
    if (!in) { out[0] = 0; return; }
    if (in[0] == '/') { snprintf(out, cap, "%s", in); return; }
    w64_host_path(in, out, cap);
}

/* If basename(path) exists inside our Mono 4.8 BCL directory (mscorlib.dll, System*.dll), redirect there. */
static void redirect_bcl_file(const char *host_path, char *out, size_t cap)
{
    const char *bd = bcl_dir();
    if (bd[0] && host_path && *host_path) {
        const char *slash = strrchr(host_path, '/');
        const char *base = slash ? slash + 1 : host_path;
        char cand[1200];
        snprintf(cand, sizeof cand, "%s/%s", bd, base);
        struct stat st;
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, cap, "%s", cand);
            return;
        }
    }
    snprintf(out, cap, "%s", host_path ? host_path : "");
}

/* ============================================================================
 * Static trampoline pool: Mono (Host ABI) -> Win64 guest (icalls & P/Invoke)
 * ============================================================================ */
typedef struct {
    uint64_t gp[9];
    uint64_t fp[8];
    uint64_t *stack;
    uint64_t ret_gp[2];
    uint64_t ret_fp[4];
} mono_tramp_frame_t;

extern uint8_t w64_mono_tramp_table[];
extern uint32_t w64_mono_tramp_stride;

enum { TK_VOID = 0, TK_INT, TK_F32, TK_F64, TK_STRUCT };
typedef struct {
    uint8_t kind;
    uint8_t hfa_f32;    /* 1..4 if homogeneous float32 struct (Vector2/3/4, Quaternion, Color, Rect) */
    uint16_t size;      /* byte size for TK_STRUCT */
} ttype_t;

#define MAX_TRAMP_ARGS 20
#define MAX_TRAMPS 8192

typedef struct {
    MonoMethod *method;
    uint64_t guest_va;
    ttype_t ret;
    uint8_t nargs;
    ttype_t args[MAX_TRAMP_ARGS];
    char name[96];
} tramp_entry_t;

static tramp_entry_t g_tramps[MAX_TRAMPS];
static int g_ntramps = 0;
static pthread_mutex_t g_tramp_mu = PTHREAD_MUTEX_INITIALIZER;

static int is_struct_all_f32(MonoClass *klass, uint16_t sz)
{
    if (!klass || sz == 0 || (sz & 3) || sz > 16) return 0;
    void *iter = NULL;
    MonoClassField *f;
    int count = 0;
    while ((f = mono_class_get_fields(klass, &iter)) != NULL) {
        uint32_t fl = mono_field_get_flags(f);
        if (fl & (FIELD_ATTRIBUTE_STATIC | FIELD_ATTRIBUTE_LITERAL)) continue;
        MonoType *ft = mono_field_get_type(f);
        int t = mono_type_get_type(ft);
        if (t == MONO_TYPE_R4) count++;
        else if (t == MONO_TYPE_VALUETYPE) {
            MonoClass *fc = mono_class_from_mono_type(ft);
            uint32_t al = 0;
            int32_t fsz = mono_class_value_size(fc, &al);
            int sub = is_struct_all_f32(fc, (uint16_t)fsz);
            if (!sub) return 0;
            count += sub;
        } else return 0;
    }
    return (count * 4 == (int)sz) ? count : 0;
}

static ttype_t classify_mtype(MonoType *t, int is_ret)
{
    ttype_t r = { TK_INT, 0, 8 };
    if (!t) { r.kind = is_ret ? TK_VOID : TK_INT; r.size = 0; return r; }
    if (mono_type_is_byref(t)) { r.kind = TK_INT; r.size = 8; return r; }
    int te = mono_type_get_type(t);
    switch (te) {
    case MONO_TYPE_VOID: r.kind = TK_VOID; r.size = 0; return r;
    case MONO_TYPE_R4:   r.kind = TK_F32;  r.size = 4; return r;
    case MONO_TYPE_R8:   r.kind = TK_F64;  r.size = 8; return r;
    case MONO_TYPE_VALUETYPE: {
        MonoClass *kc = mono_class_from_mono_type(t);
        if (kc && mono_class_is_enum(kc)) { r.kind = TK_INT; r.size = 8; return r; }
        uint32_t al = 0;
        int32_t sz = kc ? mono_class_value_size(kc, &al) : 8;
        if (sz <= 0) sz = 8;
        r.kind = TK_STRUCT;
        r.size = (uint16_t)sz;
        r.hfa_f32 = (uint8_t)is_struct_all_f32(kc, (uint16_t)sz);
        return r;
    }
    default:
        r.kind = TK_INT; r.size = 8; return r;
    }
}

static void *alloc_trampoline(MonoMethod *method, uint64_t guest_va)
{
    pthread_mutex_lock(&g_tramp_mu);
    for (int i = 0; i < g_ntramps; i++) {
        if (g_tramps[i].method == method && g_tramps[i].guest_va == guest_va) {
            pthread_mutex_unlock(&g_tramp_mu);
            return w64_mono_tramp_table + (size_t)i * w64_mono_tramp_stride;
        }
    }
    if (g_ntramps >= MAX_TRAMPS) {
        pthread_mutex_unlock(&g_tramp_mu);
        w64_fatal(NULL, "mono_bridge: trampoline pool exhausted (%d)", MAX_TRAMPS);
    }
    int idx = g_ntramps++;
    tramp_entry_t *e = &g_tramps[idx];
    memset(e, 0, sizeof *e);
    e->method = method;
    e->guest_va = guest_va;
    MonoMethodSignature *sig = mono_method_signature(method);
    e->ret = classify_mtype(mono_signature_get_return_type(sig), 1);
    int n = 0;
    if (mono_signature_is_instance(sig) && n < MAX_TRAMP_ARGS) {
        e->args[n++] = (ttype_t){ TK_INT, 0, 8 };
    }
    void *iter = NULL;
    MonoType *pt;
    while ((pt = mono_signature_get_params(sig, &iter)) != NULL && n < MAX_TRAMP_ARGS) {
        e->args[n++] = classify_mtype(pt, 0);
    }
    e->nargs = (uint8_t)n;
    const char *cn = mono_class_get_name(mono_method_get_class(method));
    const char *mn = mono_method_get_name(method);
    snprintf(e->name, sizeof e->name, "%s::%s", cn ? cn : "?", mn ? mn : "?");
    pthread_mutex_unlock(&g_tramp_mu);
    return w64_mono_tramp_table + (size_t)idx * w64_mono_tramp_stride;
}

#include <signal.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

extern void mono_threads_set_default_stacksize(uint32_t stacksize);

static __thread const char *g_last_tramp_name = NULL;
static __thread uint64_t g_last_tramp_va = 0;
static int g_trace_after_lang = 0;

void w64_mono_log_crash(uint64_t pc, uint64_t lr, uint64_t sp, uint64_t fault)
{
    Dl_info dpc = {0}, dlr = {0};
    dladdr((void *)(uintptr_t)pc, &dpc);
    dladdr((void *)(uintptr_t)lr, &dlr);
    CPU *c = w64_cpu();
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, "theforest",
            "[CRASH] SIGSEGV fault=0x%llx pc=0x%llx (%s+%#llx %s) lr=0x%llx (%s+%#llx %s) sp=0x%llx",
            (unsigned long long)fault,
            (unsigned long long)pc,
            dpc.dli_fname ? dpc.dli_fname : "?",
            (unsigned long long)(dpc.dli_fbase ? pc - (uintptr_t)dpc.dli_fbase : 0),
            dpc.dli_sname ? dpc.dli_sname : "?",
            (unsigned long long)lr,
            dlr.dli_fname ? dlr.dli_fname : "?",
            (unsigned long long)(dlr.dli_fbase ? lr - (uintptr_t)dlr.dli_fbase : 0),
            dlr.dli_sname ? dlr.dli_sname : "?",
            (unsigned long long)sp);
    if (c) {
        __android_log_print(ANDROID_LOG_ERROR, "theforest",
                "[CRASH] guest rip=0x%llx rsp=0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx last_tramp=%s(0x%llx)",
                (unsigned long long)c->rip, (unsigned long long)c->rsp,
                (unsigned long long)c->rax, (unsigned long long)c->rcx,
                (unsigned long long)c->rdx, (unsigned long long)c->r8, (unsigned long long)c->r9,
                g_last_tramp_name ? g_last_tramp_name : "none",
                (unsigned long long)g_last_tramp_va);
    }
#endif
    char *pmip_pc = mono_pmip((void *)(uintptr_t)pc);
    char *pmip_lr = mono_pmip((void *)(uintptr_t)lr);
    if (pmip_pc) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "theforest", "[CRASH] mono_pmip(pc) = %s", pmip_pc);
#endif
    }
    if (pmip_lr) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "theforest", "[CRASH] mono_pmip(lr) = %s", pmip_lr);
#endif
    }
}

/* Host ABI (x86_64 SysV or aarch64 AAPCS64) -> Win64 guest ABI dispatcher */
void w64_mono_tramp_dispatch(int slot, mono_tramp_frame_t *f)
{
    tramp_entry_t *e = &g_tramps[slot];
    g_last_tramp_name = e->name;
    g_last_tramp_va = e->guest_va;
    if (g_trace_after_lang) w64_log("[tramp] %s (0x%llx)", e->name, (unsigned long long)e->guest_va);
    CPU *c = ensure_cpu();

    uint64_t w_args[MAX_TRAMP_ARGS + 2];
    unsigned w_fmask = 0;
    int wn = 0;

    /* Staging space on guest stack for any by-value struct >8 bytes (params or return). */
    uint8_t sret_buf[256];
    uint8_t param_bufs[8][128];
    int pbuf_used = 0;

    int gi = 0, fi = 0, si = 0;
    void *host_sret = NULL;

    /* Does Host ABI pass hidden return pointer? */
    if (e->ret.kind == TK_STRUCT) {
#if defined(__x86_64__)
        if (e->ret.size > 16) host_sret = (void *)(uintptr_t)f->gp[gi++];
#elif defined(__aarch64__)
        if (e->ret.size > 16 && !e->ret.hfa_f32) host_sret = (void *)(uintptr_t)f->gp[8];
#endif
    }

    /* Does Win64 guest ABI expect hidden return pointer in arg 0 (rcx)?
     * On Win64, any struct whose size is NOT 1, 2, 4, or 8 bytes is returned via hidden rcx. */
    int win_sret = (e->ret.kind == TK_STRUCT && e->ret.size != 1 && e->ret.size != 2 && e->ret.size != 4 && e->ret.size != 8);
    if (win_sret) {
        memset(sret_buf, 0, e->ret.size <= sizeof sret_buf ? e->ret.size : sizeof sret_buf);
        w_args[wn++] = (uint64_t)(uintptr_t)(host_sret ? host_sret : sret_buf);
    }

    /* Unpack each host argument and pack into Win64 argument slot */
    for (int i = 0; i < e->nargs; i++) {
        ttype_t a = e->args[i];
        if (a.kind == TK_INT) {
            uint64_t v;
#if defined(__x86_64__)
            v = (gi < 6) ? f->gp[gi++] : f->stack[si++];
#elif defined(__aarch64__)
            v = (gi < 8) ? f->gp[gi++] : f->stack[si++];
#endif
            w_args[wn++] = v;
        } else if (a.kind == TK_F32 || a.kind == TK_F64) {
            uint64_t v;
            v = (fi < 8) ? f->fp[fi++] : f->stack[si++];
            if (a.kind == TK_F32) v &= 0xFFFFFFFFull;
            if (wn < 4) w_fmask |= (1u << wn);
            w_args[wn++] = v;
        } else if (a.kind == TK_STRUCT) {
            uint8_t *dst = param_bufs[pbuf_used < 8 ? pbuf_used++ : 7];
            memset(dst, 0, sizeof param_bufs[0]);
            uint16_t sz = a.size <= sizeof param_bufs[0] ? a.size : (uint16_t)sizeof param_bufs[0];
#if defined(__x86_64__)
            if (sz > 16) {
                int nw = (sz + 7) / 8;
                memcpy(dst, &f->stack[si], sz);
                si += nw;
            } else if (a.hfa_f32) {
                int nreg = (sz + 7) / 8;
                if (fi + nreg <= 8) {
                    for (int k = 0; k < nreg; k++) memcpy(dst + k * 8, &f->fp[fi++], 8);
                } else {
                    memcpy(dst, &f->stack[si], sz);
                    si += nreg;
                }
            } else {
                int nreg = (sz + 7) / 8;
                if (gi + nreg <= 6) {
                    for (int k = 0; k < nreg; k++) memcpy(dst + k * 8, &f->gp[gi++], 8);
                } else {
                    memcpy(dst, &f->stack[si], sz);
                    si += nreg;
                }
            }
#elif defined(__aarch64__)
            if (a.hfa_f32) {
                if (fi + a.hfa_f32 <= 8) {
                    for (int k = 0; k < a.hfa_f32; k++) {
                        uint32_t w = (uint32_t)f->fp[fi++];
                        memcpy(dst + k * 4, &w, 4);
                    }
                } else {
                    fi = 8;
                    int nw = (sz + 7) / 8;
                    memcpy(dst, &f->stack[si], sz);
                    si += nw;
                }
            } else if (sz > 16) {
                /* AAPCS64 passes composite >16 bytes by pointer! */
                uint64_t ptr = (gi < 8) ? f->gp[gi++] : f->stack[si++];
                memcpy(dst, (const void *)(uintptr_t)ptr, sz);
            } else {
                int nreg = (sz + 7) / 8;
                if (gi + nreg <= 8) {
                    for (int k = 0; k < nreg; k++) memcpy(dst + k * 8, &f->gp[gi++], 8);
                } else {
                    gi = 8;
                    memcpy(dst, &f->stack[si], sz);
                    si += nreg;
                }
            }
#endif
            /* Win64 passes structs of size 1,2,4,8 by value in GP reg, else by pointer! */
            if (sz == 1 || sz == 2 || sz == 4 || sz == 8) {
                uint64_t val = 0;
                memcpy(&val, dst, sz);
                w_args[wn++] = val;
            } else {
                w_args[wn++] = (uint64_t)(uintptr_t)dst;
            }
        }
    }

    uint64_t rax = w64_callv(c, e->guest_va, wn, w_args, w_fmask);

    /* Unpack Win64 return value into Host ABI return slots */
    if (e->ret.kind == TK_INT) {
        f->ret_gp[0] = rax;
    } else if (e->ret.kind == TK_F32) {
        f->ret_fp[0] = c->xmm[0].u64[0] & 0xFFFFFFFFull;
    } else if (e->ret.kind == TK_F64) {
        f->ret_fp[0] = c->xmm[0].u64[0];
    } else if (e->ret.kind == TK_STRUCT) {
        const uint8_t *src = win_sret ? (host_sret ? (const uint8_t *)host_sret : sret_buf) : (const uint8_t *)&rax;
        uint16_t sz = e->ret.size;
#if defined(__x86_64__)
        if (sz <= 16) {
            if (e->ret.hfa_f32) {
                memcpy(&f->ret_fp[0], src, sz < 8 ? sz : 8);
                if (sz > 8) memcpy(&f->ret_fp[1], src + 8, sz - 8);
            } else {
                memcpy(&f->ret_gp[0], src, sz < 8 ? sz : 8);
                if (sz > 8) memcpy(&f->ret_gp[1], src + 8, sz - 8);
            }
        } else {
            f->ret_gp[0] = (uint64_t)(uintptr_t)host_sret;
        }
#elif defined(__aarch64__)
        if (e->ret.hfa_f32) {
            for (int k = 0; k < e->ret.hfa_f32 && k < 4; k++) {
                uint32_t w = 0;
                memcpy(&w, src + k * 4, 4);
                f->ret_fp[k] = w;
            }
        } else if (sz <= 16) {
            memcpy(&f->ret_gp[0], src, sz < 8 ? sz : 8);
            if (sz > 8) memcpy(&f->ret_gp[1], src + 8, sz - 8);
        }
#endif
    }
}

/* Hook called by patched third_party/mono/mono/metadata/icall.c */
void *w64_mono_wrap_icall(MonoMethod *method, void *addr)
{
    uint64_t va = (uint64_t)(uintptr_t)addr;
    /* Guest addresses live in our lifted PE image ranges (0x140000000..0x18FFFFFFF) or HLE sentinels. */
    if ((va >= 0x100000000ull && va < 0x200000000ull) || (va >= HLE_BASE && va < HLE_BASE + (uint64_t)W64_SENT_SLOTS * HLE_STRIDE))
        return alloc_trampoline(method, va);
    return addr;
}

/* Hook called by patched third_party/mono/mono/metadata/loader.c for P/Invoke */
void *w64_mono_resolve_pinvoke(MonoMethod *method, const char *dll, const char *sym, uint32_t piflags)
{
    if (!dll || !sym) return NULL;
    /* Let Mono handle its own POSIX/libc/MonoPosixHelper P/Invokes natively */
    if (!strcasecmp(dll, "libc") || !strcasecmp(dll, "libm") || !strcasecmp(dll, "__Internal") ||
        !strncasecmp(dll, "lib", 3) || !strcasecmp(dll, "MonoPosixHelper"))
        return NULL;
    char dname[128];
    snprintf(dname, sizeof dname, "%s", dll);
    if (!strcasestr(dname, ".dll")) strncat(dname, ".dll", sizeof dname - strlen(dname) - 1);
    uint64_t va = w64_lookup_export_va(dname, sym);
    if (!va) {
        /* On POSIX Mono (Android), CharSet.Ansi (2), Auto (6), and NotSpec (0) all marshal
         * strings as 8-bit UTF-8/ANSI; only CharSet.Unicode (4) marshals as 16-bit UTF-16. */
        int want_unicode = ((piflags & 6u) == 4u);
        char symw[256];
        snprintf(symw, sizeof symw, "%s%c", sym, want_unicode ? 'W' : 'A');
        va = w64_lookup_export_va(dname, symw);
        if (!va) {
            snprintf(symw, sizeof symw, "%s%c", sym, want_unicode ? 'A' : 'W');
            va = w64_lookup_export_va(dname, symw);
        }
    }
    if (!va) {
        static int mlog = 0;
        if (mlog++ < 60) w64_log("[mono-pinvoke] unresolved %s!%s", dname, sym);
        return NULL;
    }
    {
        static int rlog = 0;
        if (rlog++ < 80) w64_log("[mono-pinvoke] resolved %s!%s -> 0x%llx", dname, sym, (unsigned long long)va);
    }
    return alloc_trampoline(method, va);
}

/* ============================================================================
 * Callback wrappers (Guest -> Mono)
 * ============================================================================ */
typedef struct { uint64_t fn; uint64_t ud; } cb_pair_t;
static void asm_foreach_cb(void *asm_ptr, void *user_data)
{
    cb_pair_t *p = (cb_pair_t *)user_data;
    w64_call(ensure_cpu(), p->fn, 2, (uint64_t)(uintptr_t)asm_ptr, p->ud);
}

static cb_pair_t g_liveness_cb;
static void liveness_reg_cb(void **arr, int size, void *ud)
{
    (void)ud;
    if (g_liveness_cb.fn)
        w64_call(ensure_cpu(), g_liveness_cb.fn, 3, (uint64_t)(uintptr_t)arr, (uint64_t)(uint32_t)size, g_liveness_cb.ud);
}

static uint64_t g_unhandled_cb = 0;
static void unhandled_exc_cb(MonoObject *exc)
{
    if (g_unhandled_cb) w64_call(ensure_cpu(), g_unhandled_cb, 1, (uint64_t)(uintptr_t)exc);
}

static uint64_t g_find_plugin_cb = 0;
static const char *find_plugin_cb(const char *name)
{
    if (!g_find_plugin_cb) return name;
    return (const char *)(uintptr_t)w64_call(ensure_cpu(), g_find_plugin_cb, 1, (uint64_t)(uintptr_t)name);
}

static int mono_vprintf_bridge(const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (n > 0) w64_log("[mono] %s", buf);
    return n;
}

typedef struct { uint64_t load_fn, sym_fn, close_fn, ud; } dl_fb_t;
static void *dl_fb_load(const char *name, int flags, char **err, void *ud)
{
    dl_fb_t *d = (dl_fb_t *)ud;
    if (!d || !d->load_fn) return NULL;
    return (void *)(uintptr_t)w64_call(ensure_cpu(), d->load_fn, 4, (uint64_t)(uintptr_t)name, (uint64_t)(uint32_t)flags, (uint64_t)(uintptr_t)err, d->ud);
}
static void *dl_fb_sym(void *h, const char *name, char **err, void *ud)
{
    dl_fb_t *d = (dl_fb_t *)ud;
    if (!d || !d->sym_fn) return NULL;
    return (void *)(uintptr_t)w64_call(ensure_cpu(), d->sym_fn, 4, (uint64_t)(uintptr_t)h, (uint64_t)(uintptr_t)name, (uint64_t)(uintptr_t)err, d->ud);
}
static void *dl_fb_close(void *h, void *ud)
{
    dl_fb_t *d = (dl_fb_t *)ud;
    if (!d || !d->close_fn) return NULL;
    return (void *)(uintptr_t)w64_call(ensure_cpu(), d->close_fn, 2, (uint64_t)(uintptr_t)h, d->ud);
}

static int stack_walk_cb(MonoMethod *m, int32_t native_off, int32_t il_off, int managed, void *ud)
{
    cb_pair_t *p = (cb_pair_t *)ud;
    return (int)w64_call(ensure_cpu(), p->fn, 5, (uint64_t)(uintptr_t)m, (uint64_t)(uint32_t)native_off, (uint64_t)(uint32_t)il_off, (uint64_t)(uint32_t)managed, p->ud);
}

/* ============================================================================
 * Exported Win64 mono.dll entry points (215 mono_* + g_free)
 * ============================================================================ */
#define M0(name, ret) static void w_##name(CPU *c) { RET((uint64_t)(uintptr_t)name()); }
#define M0V(name) static void w_##name(CPU *c) { name(); RETV(); }
#define M1(name) static void w_##name(CPU *c) { RET((uint64_t)(uintptr_t)((uintptr_t (*)(uintptr_t))name)(A(0))); }
#define M1V(name) static void w_##name(CPU *c) { ((void (*)(uintptr_t))name)(A(0)); RETV(); }
#define M2(name) static void w_##name(CPU *c) { RET((uint64_t)(uintptr_t)((uintptr_t (*)(uintptr_t,uintptr_t))name)(A(0),A(1))); }
#define M2V(name) static void w_##name(CPU *c) { ((void (*)(uintptr_t,uintptr_t))name)(A(0),A(1)); RETV(); }
#define M3(name) static void w_##name(CPU *c) { RET((uint64_t)(uintptr_t)((uintptr_t (*)(uintptr_t,uintptr_t,uintptr_t))name)(A(0),A(1),A(2))); }
#define M3V(name) static void w_##name(CPU *c) { ((void (*)(uintptr_t,uintptr_t,uintptr_t))name)(A(0),A(1),A(2)); RETV(); }
#define M4(name) static void w_##name(CPU *c) { RET((uint64_t)(uintptr_t)((uintptr_t (*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t))name)(A(0),A(1),A(2),A(3))); }
#define M4V(name) static void w_##name(CPU *c) { ((void (*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t))name)(A(0),A(1),A(2),A(3)); RETV(); }
#define M5(name) static void w_##name(CPU *c) { RET((uint64_t)(uintptr_t)((uintptr_t (*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t))name)(A(0),A(1),A(2),A(3),A(4))); }
#define M6(name) static void w_##name(CPU *c) { RET((uint64_t)(uintptr_t)((uintptr_t (*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t))name)(A(0),A(1),A(2),A(3),A(4),A(5))); }

/* Special path/callback/compat implementations */
static void w_mono_set_dirs(CPU *c)
{
    char a[1024], cfg[1024];
    mono_path_to_host((const char *)AP(0), a, sizeof a);
    mono_path_to_host((const char *)AP(1), cfg, sizeof cfg);
    const char *bd = bcl_dir();
    const char *use_a = bd[0] ? bd : a;
    w64_log("[mono] mono_set_dirs(%s -> %s, %s)", (const char *)AP(0), use_a, cfg);
    mono_set_dirs(use_a, cfg[0] ? cfg : "/etc");
    RETV();
}
static void w_mono_set_assemblies_path(CPU *c)
{
    const char *in = (const char *)AP(0);
    char out[4096] = "";
    const char *bd = bcl_dir();
    if (bd[0]) snprintf(out, sizeof out, "%s", bd);
    if (in && *in) {
        char copy[2048];
        snprintf(copy, sizeof copy, "%s", in);
        char *sv = NULL;
        for (char *tok = strtok_r(copy, ";", &sv); tok; tok = strtok_r(NULL, ";", &sv)) {
            char hp[1024];
            mono_path_to_host(tok, hp, sizeof hp);
            if (out[0]) strncat(out, ":", sizeof out - strlen(out) - 1);
            strncat(out, hp, sizeof out - strlen(out) - 1);
        }
    }
    w64_log("[mono] mono_set_assemblies_path -> %s", out);
    mono_set_assemblies_path(out);
    RETV();
}
static void w_mono_config_parse(CPU *c)
{
    const char *p = (const char *)AP(0);
    if (!p) { mono_config_parse(NULL); RETV(); }
    char hp[1024];
    mono_path_to_host(p, hp, sizeof hp);
    mono_config_parse(hp);
    RETV();
}
static void w_mono_jit_init_version(CPU *c)
{
    const char *fn = (const char *)AP(0);
    const char *ver = (const char *)AP(1);
    /* Give Mono C# threads an 8 MB stack (Android default is 1 MB, too small for lifted calls). */
    mono_threads_set_default_stacksize(8u << 20);
    /* Unity 5.6 passes "v2.0.50727"; our Mono 4.8 BCL is v4.0.30319 (backward-compatible with 2.0 CIL). */
    const char *use_ver = "v4.0.30319";
    w64_log("[mono] mono_jit_init_version(%s, %s -> %s)", fn ? fn : "NULL", ver ? ver : "NULL", use_ver);
    MonoDomain *d = mono_jit_init_version(fn ? fn : "TheForest", use_ver);
    RET((uint64_t)(uintptr_t)d);
}
static void w_mono_jit_init(CPU *c)
{
    const char *fn = (const char *)AP(0);
    w64_log("[mono] mono_jit_init(%s)", fn ? fn : "NULL");
    RET((uint64_t)(uintptr_t)mono_jit_init_version(fn ? fn : "TheForest", "v4.0.30319"));
}
static void w_mono_assembly_open(CPU *c)
{
    char hp[1024], rp[1200];
    mono_path_to_host((const char *)AP(0), hp, sizeof hp);
    redirect_bcl_file(hp, rp, sizeof rp);
    MonoAssembly *a = mono_assembly_open(rp, (MonoImageOpenStatus *)AP(1));
    RET((uint64_t)(uintptr_t)a);
}
static void w_mono_domain_assembly_open(CPU *c)
{
    char hp[1024], rp[1200];
    mono_path_to_host((const char *)AP(1), hp, sizeof hp);
    redirect_bcl_file(hp, rp, sizeof rp);
    MonoAssembly *a = mono_domain_assembly_open((MonoDomain *)AP(0), rp);
    if (!a) w64_log("[mono] domain_assembly_open failed: %s (from %s)", rp, (const char *)AP(1));
    RET((uint64_t)(uintptr_t)a);
}
static void w_mono_assembly_foreach(CPU *c)
{
    cb_pair_t p = { A(0), A(1) };
    mono_assembly_foreach(asm_foreach_cb, &p);
    RETV();
}
static void w_mono_unity_liveness_allocate_struct(CPU *c)
{
    g_liveness_cb.fn = A(2);
    g_liveness_cb.ud = A(3);
    RET((uint64_t)(uintptr_t)mono_unity_liveness_allocate_struct((MonoClass *)AP(0), AU(1), (void *)liveness_reg_cb, NULL));
}
static void w_mono_unity_liveness_calculation_begin(CPU *c)
{
    g_liveness_cb.fn = A(2);
    g_liveness_cb.ud = A(3);
    RET((uint64_t)(uintptr_t)mono_unity_liveness_calculation_begin((MonoClass *)AP(0), AU(1), (void *)liveness_reg_cb, NULL, NULL, NULL));
}
static void w_mono_unity_set_unhandled_exception_handler(CPU *c)
{
    g_unhandled_cb = A(0);
    RETV();
}
static void w_mono_set_find_plugin_callback(CPU *c)
{
    g_find_plugin_cb = A(0);
    mono_set_find_plugin_callback(g_find_plugin_cb ? (void *)find_plugin_cb : NULL);
    RETV();
}
static void w_mono_unity_set_vprintf_func(CPU *c)
{
    mono_unity_set_vprintf_func((void *)mono_vprintf_bridge);
    RETV();
}
static void w_mono_dl_fallback_register(CPU *c)
{
    dl_fb_t *d = calloc(1, sizeof *d);
    d->load_fn = A(0); d->sym_fn = A(1); d->close_fn = A(2); d->ud = A(3);
    RET((uint64_t)(uintptr_t)mono_dl_fallback_register(dl_fb_load, dl_fb_sym, dl_fb_close, d));
}
static void w_mono_stack_walk(CPU *c)
{
    cb_pair_t p = { A(0), A(1) };
    mono_stack_walk((void *)stack_walk_cb, &p);
    RETV();
}
static void w_mono_noop_0(CPU *c) { RET(0); }
static void w_mono_noop_v(CPU *c) { RETV(); }
static void w_mono_unity_seh_handler(CPU *c) { RET(0); }
static void w_mono_set_commandline_arguments(CPU *c)
{
    if (A(0) > 0 && AP(1)) mono_runtime_set_main_args(AI(0), (char **)AP(1));
    RETV();
}

static void w_mono_array_new(CPU *c)
{
    RET((uint64_t)(uintptr_t)mono_array_new((MonoDomain *)AP(0), (MonoClass *)AP(1), (uintptr_t)AU(2)));
}
static void w_mono_array_new_full(CPU *c)
{
    /* Unity 5.6 (Mono 2.6 ABI) passes uint32_t lengths[] and int32_t lower_bounds[];
     * Mono 4.8 mono_array_new_full expects uintptr_t[] and intptr_t[]. */
    MonoDomain *dom = (MonoDomain *)AP(0);
    MonoClass *ac = (MonoClass *)AP(1);
    const uint32_t *len32 = (const uint32_t *)AP(2);
    const int32_t *lb32 = (const int32_t *)AP(3);
    int rank = ac ? mono_class_get_rank(ac) : 1;
    if (rank < 1) rank = 1;
    if (rank > 32) rank = 32;
    uintptr_t lengths[32];
    intptr_t lower[32];
    for (int i = 0; i < rank; i++) {
        lengths[i] = len32 ? (uintptr_t)len32[i] : 0;
        lower[i] = lb32 ? (intptr_t)lb32[i] : 0;
    }
    RET((uint64_t)(uintptr_t)mono_array_new_full(dom, ac, len32 ? lengths : NULL, lb32 ? lower : NULL));
}

/* Direct pass-through wrappers */
M1V(g_free)
M2V(mono_add_internal_call)
M3(mono_array_addr_with_size)
M2(mono_array_class_get)
M1(mono_array_element_size)
M1V(mono_assembly_close)
M2(mono_assembly_fill_assembly_name)
M1(mono_assembly_get_image)
M2(mono_assembly_get_object)
M4(mono_assembly_load_from)
M5(mono_assembly_load_from_full)
M1(mono_assembly_loaded)
M2(mono_assembly_name_parse)
M1(mono_class_array_element_size)
M1(mono_class_enum_basetype)
M1(mono_class_from_mono_type)
M3(mono_class_from_name)
M3(mono_class_from_name_case)
M2(mono_class_get)
M1(mono_class_get_byref_type)
M1(mono_class_get_element_class)
M2(mono_class_get_field_from_name)
M2(mono_class_get_fields)
M1(mono_class_get_flags)
M1(mono_class_get_image)
M2(mono_class_get_interfaces)
M3(mono_class_get_method_from_name)
M2(mono_class_get_methods)
M1(mono_class_get_name)
M1(mono_class_get_namespace)
M2(mono_class_get_nested_types)
M1(mono_class_get_nesting_type)
M1(mono_class_get_parent)
M2(mono_class_get_properties)
M2(mono_class_get_property_from_name)
M1(mono_class_get_rank)
M1(mono_class_get_type)
M1(mono_class_get_type_token)
M1(mono_class_get_userdata)
M0(mono_class_get_userdata_offset, int)
M1(mono_class_instance_size)
M1(mono_class_is_enum)
M1(mono_class_is_generic)
M1(mono_class_is_inflated)
M3(mono_class_is_subclass_of)
M1(mono_class_is_valuetype)
M2V(mono_class_set_userdata)
M2(mono_class_vtable)
M1(mono_custom_attrs_construct)
M1V(mono_custom_attrs_free)
M1(mono_custom_attrs_from_assembly)
M1(mono_custom_attrs_from_class)
M2(mono_custom_attrs_from_field)
M1(mono_custom_attrs_from_method)
M2(mono_custom_attrs_get_attr)
M2(mono_custom_attrs_has_attr)
M1V(mono_debug_free_source_location)
M1V(mono_debug_init)
M3(mono_debug_lookup_source_location)
M3(mono_debug_open_image_from_memory)
M1V(mono_dl_fallback_unregister)
M2(mono_domain_create_appdomain)
M2(mono_domain_finalize)
M0(mono_domain_get, void*)
M1(mono_domain_get_id)
M2(mono_domain_set)
M1V(mono_domain_unload)
M4(mono_exception_from_name_msg)
M5(mono_exception_from_name_two_strings)
M1(mono_field_get_flags)
M1(mono_field_get_name)
M1(mono_field_get_offset)
M1(mono_field_get_parent)
M1(mono_field_get_type)
M3V(mono_field_get_value)
M3V(mono_field_set_value)
M3V(mono_field_static_get_value)
M1V(mono_gc_collect)
M0(mono_gc_get_heap_size, int64_t)
M0(mono_gc_get_used_size, int64_t)
M0(mono_gc_max_generation, int)
M2V(mono_gc_wbarrier_generic_store)
M1V(mono_gchandle_free)
M1(mono_gchandle_get_target)
M2(mono_gchandle_is_in_domain)
M2(mono_gchandle_new)
M2(mono_gchandle_new_weakref)
M0(mono_get_array_class, void*)
M0(mono_get_boolean_class, void*)
M0(mono_get_byte_class, void*)
M0(mono_get_char_class, void*)
M0(mono_get_corlib, void*)
M0(mono_get_double_class, void*)
M0(mono_get_enum_class, void*)
M2(mono_get_exception_argument_null)
M0(mono_get_exception_class, void*)
M0(mono_get_int16_class, void*)
M0(mono_get_int32_class, void*)
M0(mono_get_int64_class, void*)
M0(mono_get_object_class, void*)
M0(mono_get_root_domain, void*)
M0(mono_get_single_class, void*)
M0(mono_get_string_class, void*)
M1V(mono_image_close)
M1(mono_image_get_assembly)
M1(mono_image_get_filename)
M1(mono_image_get_name)
M2(mono_image_get_table_rows)
M5(mono_image_open_from_data_full)
M6(mono_image_open_from_data_with_name)
M0(mono_is_debugger_attached, int)
M1V(mono_jit_cleanup)
M4(mono_jit_exec)
M1(mono_jit_info_get_code_size)
M1(mono_jit_info_get_code_start)
M2(mono_jit_info_table_find)
M2V(mono_jit_parse_options)
M2(mono_metadata_signature_equal)
M2(mono_method_full_name)
M1(mono_method_get_class)
M0(mono_method_get_last_managed, void*)
M1(mono_method_get_name)
M3(mono_method_get_object)
M1(mono_method_is_generic)
M1(mono_method_is_inflated)
M1(mono_method_signature)
M1(mono_object_get_class)
M1(mono_object_get_size)
M2(mono_object_get_virtual_method)
M2(mono_object_isinst)
M2(mono_object_new)
M1(mono_object_new_alloc_specific)
M1(mono_object_new_specific)
M1(mono_object_unbox)
M1(mono_parse_default_optimizations)
M1(mono_pmip)
M1(mono_property_get_get_method)
M1V(mono_raise_exception)
M3(mono_reflection_get_custom_attrs_by_type)
M1(mono_reflection_method_get_method)
M1V(mono_runtime_cleanup)
M3(mono_runtime_delegate_invoke)
M3(mono_runtime_exec_main)
static void w_mono_runtime_invoke(CPU *c)
{
    MonoMethod *m = (MonoMethod *)AP(0);
    static int trace_enabled = -1;
    if (trace_enabled < 0) trace_enabled = getenv("W64_TRACE_INVOKE") ? 1 : 0;
    if (trace_enabled && m) {
        MonoClass *kc = mono_method_get_class(m);
        const char *cn = kc ? mono_class_get_name(kc) : "?";
        const char *mn = mono_method_get_name(m);
        if (!g_trace_after_lang && cn && strcmp(cn, "LanguageMenu") == 0) {
            g_trace_after_lang = 1;
            struct sigaction sa;
            sigset_t sm;
            memset(&sa, 0, sizeof sa);
            sigaction(SIGSEGV, NULL, &sa);
            sigprocmask(0, NULL, &sm);
            Dl_info dsa = {0};
            dladdr((void *)sa.sa_sigaction, &dsa);
            w64_log("[sigcheck] SIGSEGV handler=%p (%s+%#llx %s) flags=0x%x blocked=%d",
                    (void *)sa.sa_sigaction,
                    dsa.dli_fname ? dsa.dli_fname : "?",
                    (unsigned long long)(dsa.dli_fbase ? (uintptr_t)sa.sa_sigaction - (uintptr_t)dsa.dli_fbase : 0),
                    dsa.dli_sname ? dsa.dli_sname : "?",
                    (unsigned)sa.sa_flags, sigismember(&sm, SIGSEGV));
            if (!(sa.sa_flags & SA_SIGINFO) && sa.sa_sigaction) {
                sa.sa_flags |= SA_SIGINFO | SA_ONSTACK;
                sigaction(SIGSEGV, &sa, NULL);
                w64_log("[sigcheck] restored SA_SIGINFO on SIGSEGV -> flags=0x%x", (unsigned)sa.sa_flags);
            }
        }
        if (g_trace_after_lang)
            w64_log("[invoke] %s::%s", cn ? cn : "?", mn ? mn : "?");
    }
    RET((uint64_t)(uintptr_t)mono_runtime_invoke(m, (void *)AP(1), (void **)AP(2), (MonoObject **)AP(3)));
}
M4(mono_runtime_invoke_array)
M0(mono_runtime_is_shutting_down, int)
M1V(mono_runtime_object_init)
M0V(mono_runtime_set_shutting_down)
M1V(mono_runtime_unhandled_exception_policy_set)
M0V(mono_security_enable_core_clr)
M1V(mono_security_set_mode)
M2V(mono_set_defaults)
M1V(mono_set_signal_chaining)
M1(mono_signature_get_param_count)
M2(mono_signature_get_params)
M1(mono_signature_get_return_type)
M1(mono_signature_is_instance)
M1(mono_string_from_utf16)
M3(mono_string_new_len)
M1(mono_string_new_wrapper)
M1(mono_string_to_utf16)
M1(mono_string_to_utf8)
M1(mono_stringify_assembly_name)
M1(mono_thread_attach)
M0(mono_thread_current, void*)
M1V(mono_thread_detach)
M0V(mono_thread_exit)
M0V(mono_thread_pool_cleanup)
M0V(mono_thread_pop_appdomain_ref)
M0V(mono_thread_push_appdomain_ref)
M1V(mono_thread_set_main)
M0V(mono_thread_suspend_all_other_threads)
M0V(mono_threads_set_shutting_down)
M1V(mono_trace_set_level_string)
M1V(mono_trace_set_mask_string)
M1(mono_type_get_class)
M1(mono_type_get_name)
M2(mono_type_get_object)
M1(mono_type_get_type)
M1(mono_unity_class_is_abstract)
M1(mono_unity_class_is_interface)
M1V(mono_unity_liveness_calculation_end)
M2V(mono_unity_liveness_calculation_from_root)
M1V(mono_unity_liveness_calculation_from_statics)
M1V(mono_unity_liveness_finalize)
M1V(mono_unity_liveness_free_struct)
M0V(mono_unity_liveness_start_gc_world)
M0V(mono_unity_liveness_stop_gc_world)
M1V(mono_unity_set_embeddinghostname)
M1V(mono_unity_socket_security_enabled_set)
M3(mono_value_box)
M1V(mono_verifier_set_mode)

#define ME(n) { "mono.dll", #n, w_##n }
static const w64_export_t mono_exports[] = {
    ME(g_free),
    ME(mono_add_internal_call), ME(mono_array_addr_with_size), ME(mono_array_class_get), ME(mono_array_element_size),
    ME(mono_array_new), ME(mono_array_new_full), ME(mono_assembly_close), ME(mono_assembly_fill_assembly_name),
    ME(mono_assembly_foreach), ME(mono_assembly_get_image), ME(mono_assembly_get_object), ME(mono_assembly_load_from),
    ME(mono_assembly_load_from_full), ME(mono_assembly_loaded), ME(mono_assembly_name_parse), ME(mono_assembly_open),
    ME(mono_class_array_element_size), ME(mono_class_enum_basetype), ME(mono_class_from_mono_type), ME(mono_class_from_name),
    ME(mono_class_from_name_case), ME(mono_class_get), ME(mono_class_get_byref_type), ME(mono_class_get_element_class),
    ME(mono_class_get_field_from_name), ME(mono_class_get_fields), ME(mono_class_get_flags), ME(mono_class_get_image),
    ME(mono_class_get_interfaces), ME(mono_class_get_method_from_name), ME(mono_class_get_methods), ME(mono_class_get_name),
    ME(mono_class_get_namespace), ME(mono_class_get_nested_types), ME(mono_class_get_nesting_type), ME(mono_class_get_parent),
    ME(mono_class_get_properties), ME(mono_class_get_property_from_name), ME(mono_class_get_rank), ME(mono_class_get_type),
    ME(mono_class_get_type_token), ME(mono_class_get_userdata), ME(mono_class_get_userdata_offset), ME(mono_class_instance_size),
    ME(mono_class_is_enum), ME(mono_class_is_generic), ME(mono_class_is_inflated), ME(mono_class_is_subclass_of),
    ME(mono_class_is_valuetype), ME(mono_class_set_userdata), ME(mono_class_vtable), ME(mono_config_parse),
    ME(mono_custom_attrs_construct), ME(mono_custom_attrs_free), ME(mono_custom_attrs_from_assembly), ME(mono_custom_attrs_from_class),
    ME(mono_custom_attrs_from_field), ME(mono_custom_attrs_from_method), ME(mono_custom_attrs_get_attr), ME(mono_custom_attrs_has_attr),
    ME(mono_debug_free_source_location), ME(mono_debug_init), ME(mono_debug_lookup_source_location), ME(mono_debug_open_image_from_memory),
    ME(mono_dl_fallback_register), ME(mono_dl_fallback_unregister), ME(mono_domain_assembly_open), ME(mono_domain_create_appdomain),
    ME(mono_domain_finalize), ME(mono_domain_get), ME(mono_domain_get_id), ME(mono_domain_set), ME(mono_domain_unload),
    ME(mono_exception_from_name_msg), ME(mono_exception_from_name_two_strings), ME(mono_field_get_flags), ME(mono_field_get_name),
    ME(mono_field_get_offset), ME(mono_field_get_parent), ME(mono_field_get_type), ME(mono_field_get_value), ME(mono_field_set_value),
    ME(mono_field_static_get_value), ME(mono_gc_collect), ME(mono_gc_get_heap_size), ME(mono_gc_get_used_size), ME(mono_gc_max_generation),
    ME(mono_gc_wbarrier_generic_store), ME(mono_gchandle_free), ME(mono_gchandle_get_target), ME(mono_gchandle_is_in_domain),
    ME(mono_gchandle_new), ME(mono_gchandle_new_weakref), ME(mono_get_array_class), ME(mono_get_boolean_class), ME(mono_get_byte_class),
    ME(mono_get_char_class), ME(mono_get_corlib), ME(mono_get_double_class), ME(mono_get_enum_class), ME(mono_get_exception_argument_null),
    ME(mono_get_exception_class), ME(mono_get_int16_class), ME(mono_get_int32_class), ME(mono_get_int64_class), ME(mono_get_object_class),
    ME(mono_get_root_domain), ME(mono_get_single_class), ME(mono_get_string_class), ME(mono_image_close), ME(mono_image_get_assembly),
    ME(mono_image_get_filename), ME(mono_image_get_name), ME(mono_image_get_table_rows), ME(mono_image_open_from_data_full),
    ME(mono_image_open_from_data_with_name), ME(mono_is_debugger_attached), ME(mono_jit_cleanup), ME(mono_jit_exec),
    ME(mono_jit_info_get_code_size), ME(mono_jit_info_get_code_start), ME(mono_jit_info_table_find), ME(mono_jit_init),
    ME(mono_jit_init_version), ME(mono_jit_parse_options),
    { "mono.dll", "mono_loader_error_prepare_exception", w_mono_noop_0 },
    { "mono.dll", "mono_loader_get_last_error", w_mono_noop_0 },
    ME(mono_metadata_signature_equal), ME(mono_method_full_name), ME(mono_method_get_class), ME(mono_method_get_last_managed),
    ME(mono_method_get_name), ME(mono_method_get_object), ME(mono_method_is_generic), ME(mono_method_is_inflated),
    ME(mono_method_signature), ME(mono_object_get_class), ME(mono_object_get_size), ME(mono_object_get_virtual_method),
    ME(mono_object_isinst), ME(mono_object_new), ME(mono_object_new_alloc_specific), ME(mono_object_new_specific), ME(mono_object_unbox),
    ME(mono_parse_default_optimizations), ME(mono_pmip), ME(mono_property_get_get_method), ME(mono_raise_exception),
    ME(mono_reflection_get_custom_attrs_by_type), ME(mono_reflection_method_get_method), ME(mono_runtime_cleanup),
    ME(mono_runtime_delegate_invoke), ME(mono_runtime_exec_main), ME(mono_runtime_invoke), ME(mono_runtime_invoke_array),
    ME(mono_runtime_is_shutting_down), ME(mono_runtime_object_init), ME(mono_runtime_set_shutting_down),
    ME(mono_runtime_unhandled_exception_policy_set), ME(mono_security_enable_core_clr),
    { "mono.dll", "mono_security_set_core_clr_platform_callback", w_mono_noop_v },
    ME(mono_security_set_mode), ME(mono_set_assemblies_path),
    { "mono.dll", "mono_set_break_policy", w_mono_noop_v },
    ME(mono_set_commandline_arguments), ME(mono_set_defaults), ME(mono_set_dirs), ME(mono_set_find_plugin_callback),
    { "mono.dll", "mono_set_ignore_version_and_key_when_finding_assemblies_already_loaded", w_mono_noop_v },
    ME(mono_set_signal_chaining), ME(mono_signature_get_param_count), ME(mono_signature_get_params), ME(mono_signature_get_return_type),
    ME(mono_signature_is_instance), ME(mono_stack_walk), ME(mono_string_from_utf16), ME(mono_string_new_len),
    ME(mono_string_new_wrapper), ME(mono_string_to_utf16), ME(mono_string_to_utf8), ME(mono_stringify_assembly_name),
    ME(mono_thread_attach), ME(mono_thread_current), ME(mono_thread_detach), ME(mono_thread_exit), ME(mono_thread_pool_cleanup),
    ME(mono_thread_pop_appdomain_ref), ME(mono_thread_push_appdomain_ref), ME(mono_thread_set_main),
    ME(mono_thread_suspend_all_other_threads), ME(mono_threads_set_shutting_down), ME(mono_trace_set_level_string),
    ME(mono_trace_set_mask_string), ME(mono_type_get_class), ME(mono_type_get_name), ME(mono_type_get_object), ME(mono_type_get_type),
    ME(mono_unity_class_is_abstract), ME(mono_unity_class_is_interface), ME(mono_unity_liveness_allocate_struct),
    ME(mono_unity_liveness_calculation_begin), ME(mono_unity_liveness_calculation_end), ME(mono_unity_liveness_calculation_from_root),
    ME(mono_unity_liveness_calculation_from_statics), ME(mono_unity_liveness_finalize), ME(mono_unity_liveness_free_struct),
    ME(mono_unity_liveness_start_gc_world), ME(mono_unity_liveness_stop_gc_world), ME(mono_unity_seh_handler),
    ME(mono_unity_set_embeddinghostname), ME(mono_unity_set_unhandled_exception_handler), ME(mono_unity_set_vprintf_func),
    ME(mono_unity_socket_security_enabled_set), ME(mono_value_box), ME(mono_verifier_set_mode),
    { "mono.dll", "unity_mono_method_is_generic", w_mono_method_is_generic },
    { "mono.dll", "unity_mono_method_is_inflated", w_mono_method_is_inflated },
    { "mono.dll", "unity_mono_reflection_method_get_method", w_mono_reflection_method_get_method },
    { 0 }
};

__attribute__((constructor(102))) static void mono_bridge_init(void) { w64_register(mono_exports); }
