/*
 * native_call.c - guest calls into native objects, and the d3d11/dxgi entry points.
 *
 * The guest is identity-mapped, so the COM objects DXVK creates are directly usable by the
 * recompiled game: same memory, and the COM layout (vtable pointer first, methods in declaration
 * order, `this` first) is the same in the Win64 and the native ABI. Only the call itself differs:
 * the guest's `call [rax+N]` lands in dispatch() with a host code address, which forwards it here.
 *
 * Arguments are moved from the Win64 registers/stack to native ones. Integer and pointer
 * arguments map positionally (the native callee ignores surplus ones), so every method is called
 * through one 14-argument prototype - except the few with float arguments or a float result, and
 * the DXGI descriptions whose WCHAR strings are 4 bytes wide in DXVK native. Those are found once
 * per method address (hp_native_classify in gfx/hp_d3d11.cpp) and remembered.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>

int hp_native_classify(void *self, uint64_t fn);                       /* gfx/hp_d3d11.cpp */
int32_t hp_native_desc_call(int kind, void *self, uint64_t fn, void *out);
uint64_t hp_native_trace(int kind, uint64_t fn, const uint64_t *a);
int32_t hp_native_present(int kind, void *self, uint64_t fn, uint32_t sync, uint32_t flags, const void *params);
int32_t hp_d3d11_create_device(void *adapter, uint32_t driver, uint32_t flags, const void *levels, uint32_t nlevels,
                               uint32_t sdk, void **device, void *level, void **context);
int32_t hp_dxgi_create_factory(uint32_t flags, const void *riid, void **out);

enum { NK_UNKNOWN = 0, NK_GENERIC, NK_CLEAR_DSV, NK_SET_MINLOD, NK_GET_MINLOD, NK_PRESENT, NK_PRESENT1, NK_CREATE_SC, NK_CREATE_SC_HWND, NK_RESIZE_BUFFERS, NK_REG4, NK_REG8, NK_DESC_FIRST = 16 };

/* ---- the native image: code range (call targets) and whole range (vtables) ---- */
static uint64_t g_code_lo, g_code_hi, g_img_lo, g_img_hi;
static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    uint64_t me = (uint64_t)(uintptr_t)data;
    uint64_t lo = ~0ull, hi = 0, clo = ~0ull, chi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD) continue;
        uint64_t a = info->dlpi_addr + p->p_vaddr, b = a + p->p_memsz;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
        if (p->p_flags & PF_X) { if (a < clo) clo = a; if (b > chi) chi = b; }
    }
    if (me >= clo && me < chi) { g_img_lo = lo; g_img_hi = hi; g_code_lo = clo; g_code_hi = chi; return 1; }
    return 0;
}
static void range_init(void)
{
    dl_iterate_phdr(phdr_cb, (void *)(uintptr_t)&range_init);
    if (!g_code_hi) w64_fatal(NULL, "native_call: own image not found");
}
int w64_native_code(uint64_t va)
{
    if (!g_code_hi) range_init();
    /* lifted guest code never shares addresses with the host image */
    return va >= g_code_lo && va < g_code_hi;
}
int w64_native_image(uint64_t va) { if (!g_code_hi) range_init(); return va >= g_img_lo && va < g_img_hi; }

/* ---- method address -> kind, filled on first call ---- */
#define KBITS 14
static struct { uint64_t key; int kind; } g_kinds[1 << KBITS];
static pthread_mutex_t g_kmu = PTHREAD_MUTEX_INITIALIZER;
static __thread struct { uint64_t fn; int kind; } t_kcache[64];
static int kind_of(CPU *c, uint64_t fn)
{
    uint32_t ci = (uint32_t)((fn >> 4) ^ (fn >> 10)) & 63u;
    if (__builtin_expect(t_kcache[ci].fn == fn, 1)) return t_kcache[ci].kind;
    uint32_t h = (uint32_t)((fn * 0x9E3779B97F4A7C15ull) >> (64 - KBITS));
    for (uint32_t k = 0; k < (1u << KBITS); k++, h = (h + 1) & ((1u << KBITS) - 1)) {
        uint64_t key = __atomic_load_n(&g_kinds[h].key, __ATOMIC_ACQUIRE);
        if (key == fn) { int kd = g_kinds[h].kind; t_kcache[ci].fn = fn; t_kcache[ci].kind = kd; return kd; }
        if (!key) break;
    }
    /* first call of this method: `this` is the object it is called on */
    int kind = NK_GENERIC;
    void *self = (void *)(uintptr_t)c->rcx;
    if (self && c->rcx > 0x10000 && w64_native_image(rd64(c->rcx))) kind = hp_native_classify(self, fn);
    if (kind <= 0) kind = NK_GENERIC;
    pthread_mutex_lock(&g_kmu);
    h = (uint32_t)((fn * 0x9E3779B97F4A7C15ull) >> (64 - KBITS));
    for (uint32_t k = 0; k < (1u << KBITS); k++, h = (h + 1) & ((1u << KBITS) - 1)) {
        if (g_kinds[h].key == fn) break;
        if (!g_kinds[h].key) {
            g_kinds[h].kind = kind;
            __atomic_store_n(&g_kinds[h].key, fn, __ATOMIC_RELEASE);
            break;
        }
    }
    pthread_mutex_unlock(&g_kmu);
    t_kcache[ci].fn = fn; t_kcache[ci].kind = kind;
    if (kind != NK_GENERIC && w64_trace) w64_log("[native] %#llx kind %d", (unsigned long long)fn, kind);
    return kind;
}

typedef uint64_t (*ncall4_t)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*ncall8_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*ncall_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

void w64_native_call(CPU *c, uint64_t fn)
{
    __atomic_fetch_add(&g_w64_com_calls, 1, __ATOMIC_RELAXED);
    int kind = kind_of(c, fn);
    if (__builtin_expect(kind == NK_REG4, 1)) {
        c->rax = ((ncall4_t)(uintptr_t)fn)(c->rcx, c->rdx, c->r8, c->r9);
        c->rsp += 8;
        return;
    }
    if (__builtin_expect(kind == NK_REG8, 0)) {
        uint64_t sp = c->rsp;
        c->rax = ((ncall8_t)(uintptr_t)fn)(c->rcx, c->rdx, c->r8, c->r9,
                                           rd64(sp + 0x28), rd64(sp + 0x30), rd64(sp + 0x38), rd64(sp + 0x40));
        c->rsp += 8;
        return;
    }
    switch (kind) {
    case NK_CLEAR_DSV:          /* (this, dsv, UINT flags, FLOAT depth, UINT8 stencil) */
        ((void (*)(void *, void *, uint32_t, float, uint8_t))(uintptr_t)fn)(AP(0), AP(1), AU(2), c->xmm[3].f32[0], (uint8_t)A(4));
        break;
    case NK_SET_MINLOD:         /* (this, resource, FLOAT) */
        ((void (*)(void *, void *, float))(uintptr_t)fn)(AP(0), AP(1), c->xmm[2].f32[0]);
        break;
    case NK_GET_MINLOD: {       /* FLOAT (this, resource) */
        float r = ((float (*)(void *, void *))(uintptr_t)fn)(AP(0), AP(1));
        c->xmm[0].u64[0] = 0; c->xmm[0].u64[1] = 0; c->xmm[0].f32[0] = r;
        break;
    }
    case NK_PRESENT:
    case NK_PRESENT1:
        c->rax = (uint64_t)(uint32_t)hp_native_present(kind, AP(0), fn, AU(1), AU(2), AP(3));
        break;
    case NK_CREATE_SC:          /* (this, device, DXGI_SWAP_CHAIN_DESC*, out) */
        w64_log("[dxgi] CreateSwapChain %ux%u fmt %u, %u buffers, effect %u, flags %#x, windowed %u, hwnd %#llx",
                rd32(A(2)), rd32(A(2) + 4), rd32(A(2) + 16), rd32(A(2) + 40), rd32(A(2) + 64), rd32(A(2) + 68), rd32(A(2) + 56),
                (unsigned long long)rd64(A(2) + 48));
        goto generic;
    case NK_CREATE_SC_HWND:     /* (this, device, hwnd, DXGI_SWAP_CHAIN_DESC1*, fullscreen desc*, output, out) */
        w64_log("[dxgi] CreateSwapChainForHwnd %#llx: %ux%u fmt %u, %u buffers, scaling %u, effect %u, flags %#x, fullscreen desc %s windowed %u",
                (unsigned long long)A(2), rd32(A(3)), rd32(A(3) + 4), rd32(A(3) + 8), rd32(A(3) + 24), rd32(A(3) + 28),
                rd32(A(3) + 32), rd32(A(3) + 40), A(4) ? "yes" : "no", A(4) ? rd32(A(4) + 16) : 1);
        goto generic;
    case NK_RESIZE_BUFFERS:     /* (this, count, w, h, fmt, flags) */
        w64_log("[dxgi] ResizeBuffers(%u, %ux%u, fmt %u, flags %#x)", AU(1), AU(2), AU(3), AU(4), AU(5));
        goto generic;
    generic:
    default:
        if (kind >= 40) {
            uint64_t a[14];
            for (int i = 0; i < 14; i++) a[i] = A(i);
            c->rax = hp_native_trace(kind, fn, a);
            break;
        }
        if (kind >= NK_DESC_FIRST) { c->rax = (uint64_t)(uint32_t)hp_native_desc_call(kind, AP(0), fn, AP(1)); break; }
        c->rax = ((ncall_t)(uintptr_t)fn)(A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11), A(12), A(13));
        break;
    }
    c->rsp += 8;
}

/* ---- d3d11.dll / dxgi.dll ---- */
static void D3D11CreateDevice_(CPU *c)
{
    /* (pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
     *  ppDevice, pFeatureLevel, ppImmediateContext) */
    int32_t hr = hp_d3d11_create_device(AP(0), AU(1), AU(3), AP(4), AU(5), AU(6), (void **)AP(7), AP(8), (void **)AP(9));
    /* who creates devices (Unity creates more than one): the first module return addresses */
    char who[256]; int n = 0, k = 0;
    who[0] = 0;
    for (uint64_t p = c->rsp; k < 8 && p < c->rsp + 0x4000; p += 8) {
        uint64_t v = rd64(p);
        w64_module_t *m = w64_module_by_va(v);
        if (m && v > m->base + 0x1000) { n += snprintf(who + n, sizeof who - (size_t)n, " %#llx", (unsigned long long)v); k++; }
    }
    w64_log("[d3d11] device created from%s", who);
    RET((uint32_t)hr);
}
static void D3D11CreateDeviceAndSwapChain_(CPU *c) { w64_log("[d3d11] D3D11CreateDeviceAndSwapChain not supported"); RET(0x80004001u); }
static void CreateDXGIFactory_(CPU *c) { RET((uint32_t)hp_dxgi_create_factory(0, AP(0), (void **)AP(1))); }
static void CreateDXGIFactory2_(CPU *c) { RET((uint32_t)hp_dxgi_create_factory(AU(0), AP(1), (void **)AP(2))); }
static void DXGIGetDebugInterface1_(CPU *c) { RET(0x80004002u); }      /* E_NOINTERFACE */
static void D3DPerf_(CPU *c) { RET(0); }

/* DXVK waitable objects as guest handles (gfx/dxvk/prepare.py) */
uint64_t w64_sem_create(int32_t initial, int32_t max);
void w64_sem_release(uint64_t h);
void *hp_dxvk_sem_create(uint32_t initial, uint32_t max)
{
    uint64_t h = w64_sem_create((int32_t)initial, (int32_t)max);
    w64_log("[dxgi] frame latency waitable object %#llx (initial %u)", (unsigned long long)h, initial);
    return (void *)(uintptr_t)h;
}
void hp_dxvk_sem_release(void *h) { w64_sem_release((uint64_t)(uintptr_t)h); }
void hp_dxvk_event_set(void *h) { w64_sem_release((uint64_t)(uintptr_t)h); }
void hp_dxvk_handle_close(void *h) { w64_handle_close((uint64_t)(uintptr_t)h); }
/* DXVK also writes every line to stderr itself; on Android that goes nowhere, so the runtime log
 * (logcat) gets warnings and errors. */
void hp_dxvk_log(int level, const char *line)
{
#ifdef __ANDROID__
    static int min = -1;
    if (min < 0) { const char *e = getenv("HP_DXVK_LOG"); min = e && *e ? atoi(e) : 3; }   /* 2 = info */
    if (level >= min) w64_log("[dxvk] %s", line);
#else
    (void)level; (void)line;
#endif
}

static const w64_export_t d3d_exports[] = {
    { "d3d11.dll", "D3D11CreateDevice", D3D11CreateDevice_ },
    { "d3d11.dll", "D3D11CreateDeviceAndSwapChain", D3D11CreateDeviceAndSwapChain_ },
    { "dxgi.dll", "CreateDXGIFactory", CreateDXGIFactory_ },
    { "dxgi.dll", "CreateDXGIFactory1", CreateDXGIFactory_ },
    { "dxgi.dll", "CreateDXGIFactory2", CreateDXGIFactory2_ },
    { "dxgi.dll", "DXGIGetDebugInterface1", DXGIGetDebugInterface1_ },
    { "d3d9.dll", "D3DPERF_BeginEvent", D3DPerf_ }, { "d3d9.dll", "D3DPERF_EndEvent", D3DPerf_ },
    { 0 }
};
__attribute__((constructor(102))) static void native_call_init(void) { w64_register(d3d_exports); }

/* DXVK (gfx/dxvk/prepare.py): what to do when the game changes render targets.
 *   0 = DXVK's own timer-driven implicit flush, 1 = end the render pass + full barrier,
 *   2 = a submission (Flush). Adreno drivers draw some passes wrongly when several render
 *   passes sharing attachments sit in one command buffer (see trackmania-port-android). */
int hp_rp_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("HP_RP_MODE");
        m = e && *e ? atoi(e) : 0;
        w64_log("[d3d11] render-target change: %s", m == 2 ? "flush" : m == 1 ? "barrier" : "DXVK default");
    }
    return m;
}

const char *hp_userdir(void) { return w64_userdir; }
