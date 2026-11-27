// hp_d3d11.cpp - Direct3D 11 + DXGI for the recompiled 64-bit guest, backed by DXVK native.
//
// No wrapper objects: the guest is identity-mapped and receives DXVK's own interface pointers
// (runtime/src/native_call.c forwards its vtable calls). This file creates the device and the
// factory, and knows the handful of methods the generic forwarding cannot call:
//   - float arguments / results (ClearDepthStencilView, Set/GetResourceMinLOD),
//   - DXGI descriptions with WCHAR names (DXVK native: 4-byte wchar_t, Windows: 2 bytes).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <sys/stat.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
void w64_log(const char* fmt, ...);
void* hp_wsi_host_open(uint32_t w, uint32_t h);
void* hp_wsi_window_handle(void);
void hp_wsi_present_begin(void);
void hp_wsi_present_end(void);
void w64_sem_release(uint64_t h);
}

extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID, void**);

namespace {
enum { NK_GENERIC = 1, NK_CLEAR_DSV, NK_SET_MINLOD, NK_GET_MINLOD, NK_PRESENT, NK_PRESENT1, NK_CREATE_SC, NK_CREATE_SC_HWND, NK_RESIZE_BUFFERS, NK_REG4, NK_REG8, NK_DESC_FIRST = 16,
       NK_TR_MAP = 40, NK_TR_UPD, NK_TR_UPD1, NK_TR_DI, NK_TR_D, NK_TR_DII, NK_TR_DIN, NK_TEX2D, NK_TEX1D, NK_TEX3D, NK_BUF };
enum { D_ADAPTER, D_ADAPTER1, D_ADAPTER2, D_ADAPTER3, D_OUTPUT, D_OUTPUT1 };

// vtable index of a virtual method from its pointer-to-member (Itanium C++ ABI; AArch64 uses the
// ARM variant with the virtual flag in the adjustment).
template <class T, class M> int vidx(M T::*pm) {
    struct { uintptr_t ptr; intptr_t adj; } r;
    static_assert(sizeof(pm) == sizeof(r), "pointer to member layout");
    std::memcpy(&r, &pm, sizeof r);
#if defined(__aarch64__) || defined(__arm__)
    return (r.adj & 1) ? int(r.ptr / sizeof(void*)) : -1;
#else
    return (r.ptr & 1) ? int((r.ptr - 1) / sizeof(void*)) : -1;
#endif
}

struct Slot { int idx, kind; };

// Is `self` (exactly) an object of interface T, and fn its method at one of the slots?
template <class T> int probe(void* self, uint64_t fn, const Slot* slots, int n) {
    IUnknown* u = static_cast<IUnknown*>(self);
    void** vt0 = *reinterpret_cast<void***>(self);
    int cand = 0;       // only ask the object (QueryInterface) when fn sits in one of the slots
    for (int i = 0; i < n; i++)
        if (slots[i].idx >= 0 && reinterpret_cast<uint64_t>(vt0[slots[i].idx]) == fn) cand = 1;
    if (!cand) return 0;
    void* p = nullptr;
    if (FAILED(u->QueryInterface(__uuidof(T), &p)) || !p) return 0;
    bool same = p == self;
    static_cast<IUnknown*>(p)->Release();
    if (!same) return 0;
    void** vt = *reinterpret_cast<void***>(self);
    for (int i = 0; i < n; i++)
        if (slots[i].idx >= 0 && reinterpret_cast<uint64_t>(vt[slots[i].idx]) == fn) return slots[i].kind;
    return 0;
}

bool g_logged_slots = false;
}  // namespace

// ---- D3D11 trace (HP_D3DTRACE="first-last" frames): draw parameters and hashes of the data the
// GPU reads, to compare the host (x86-64, lavapipe) with a device draw by draw.
static unsigned long g_frame;
static unsigned g_draw_no;
static int g_tr_first = -1, g_tr_last = -1;
static bool trace_on() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("HP_D3DTRACE");
        on = e && std::sscanf(e, "%d-%d", &g_tr_first, &g_tr_last) == 2;
        if (on) w64_log("[trace] D3D11 trace for frames %d-%d", g_tr_first, g_tr_last);
    }
    return on > 0;
}
static bool tr_now() { return trace_on() && (long)g_frame >= g_tr_first && (long)g_frame <= g_tr_last; }
struct Mem { const uint8_t* p = nullptr; size_t n = 0; std::vector<uint8_t> copy; };
static std::mutex g_tr_mu;
static std::unordered_map<void*, Mem> g_mem;      // buffer -> CPU-side contents
static uint64_t fnv(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static size_t buf_size(ID3D11Resource* r) {
    D3D11_RESOURCE_DIMENSION d; r->GetType(&d);
    if (d != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC bd; static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
    return bd.ByteWidth;
}
static const uint8_t* mem_of(void* buf, size_t* n) {
    auto it = g_mem.find(buf);
    if (it == g_mem.end()) { *n = 0; return nullptr; }
    *n = it->second.n;
    return it->second.copy.empty() ? it->second.p : it->second.copy.data();
}
static void tr_draw(ID3D11DeviceContext* c, const char* what, UINT count, UINT start, INT base, UINT inst) {
    std::lock_guard<std::mutex> l(g_tr_mu);
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT fmt; UINT ioff = 0;
    c->IAGetIndexBuffer(&ib, &fmt, &ioff);
    ID3D11Buffer* vb[2] = {}; UINT st[2] = {}, vo[2] = {};
    c->IAGetVertexBuffers(0, 2, vb, st, vo);
    char line[1024]; int o = 0;
    o += std::snprintf(line + o, sizeof line - o, "[tr] f%lu d%u %s n=%u s=%u b=%d i=%u", g_frame, g_draw_no, what, count, start, base, inst);
    long lo = 0, hi = -1;
    if (ib && (what[0] == 'I')) {
        size_t n; const uint8_t* m = mem_of(ib, &n);
        UINT isz = fmt == DXGI_FORMAT_R16_UINT ? 2 : 4;
        size_t a = ioff + (size_t)start * isz, len = (size_t)count * isz;
        if (m && a + len <= n) {
            lo = 0x7fffffff; hi = -1;
            for (UINT k = 0; k < count; k++) {
                uint32_t v = isz == 2 ? *reinterpret_cast<const uint16_t*>(m + a + k * 2) : *reinterpret_cast<const uint32_t*>(m + a + k * 4);
                if ((long)v < lo) lo = v; if ((long)v > hi) hi = v;
            }
            o += std::snprintf(line + o, sizeof line - o, " ih=%016llx[%ld..%ld]", (unsigned long long)fnv(m + a, len), lo, hi);
        } else o += std::snprintf(line + o, sizeof line - o, " ih=?");
    } else if (what[0] != 'I') { lo = start; hi = (long)start + count - 1; }
    for (int s = 0; s < 2; s++) {
        if (!vb[s]) continue;
        size_t n; const uint8_t* m = mem_of(vb[s], &n);
        long f = lo + (what[0] == 'I' ? base : 0), t = hi + (what[0] == 'I' ? base : 0);
        size_t a = vo[s] + (size_t)(f < 0 ? 0 : f) * st[s], len = (t >= f) ? (size_t)(t - f + 1) * st[s] : 0;
        if (m && len && a + len <= n) o += std::snprintf(line + o, sizeof line - o, " v%d=%016llx/%u", s, (unsigned long long)fnv(m + a, len), st[s]);
        else o += std::snprintf(line + o, sizeof line - o, " v%d=?/%u", s, st[s]);
    }
    ID3D11DeviceContext1* c1 = nullptr;
    c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
    for (int stage = 0; stage < 2; stage++) {
        ID3D11Buffer* cb[4] = {}; UINT first[4] = {}, num[4] = {};
        if (c1) { if (stage) c1->PSGetConstantBuffers1(0, 4, cb, first, num); else c1->VSGetConstantBuffers1(0, 4, cb, first, num); }
        for (int k = 0; k < 4; k++) {
            if (!cb[k]) continue;
            size_t n; const uint8_t* m = mem_of(cb[k], &n);
            size_t a = (size_t)first[k] * 16, len = num[k] ? (size_t)num[k] * 16 : n;
            if (m && a < n) { if (a + len > n) len = n - a; o += std::snprintf(line + o, sizeof line - o, " %c%d=%016llx", stage ? 'p' : 'v', k, (unsigned long long)fnv(m + a, len)); }
            else o += std::snprintf(line + o, sizeof line - o, " %c%d=?", stage ? 'p' : 'v', k);
            cb[k]->Release();
        }
    }
    if (c1) c1->Release();
    if (ib) ib->Release();
    for (auto* b : vb) if (b) b->Release();
    w64_log("%s", line);
}

// ---- render target dump (HP_D3DDUMP="frame:first-last" draw numbers within that frame) -------
// After each selected draw the first bound render target is copied to a staging texture and
// written raw to <user dir>/dump/f<frame>_d<draw>.raw (header: w h dxgi-format row-pitch).
extern "C" const char* hp_userdir(void);
static long g_dump_frame = -1, g_dump_a = 0, g_dump_b = -1;
static void dump_rt(ID3D11DeviceContext* c) {
    static int init = 0;
    if (!init) { init = 1; const char* e = getenv("HP_D3DDUMP"); if (e) std::sscanf(e, "%ld:%ld-%ld", &g_dump_frame, &g_dump_a, &g_dump_b); }
    if ((long)g_frame != g_dump_frame || (long)g_draw_no < g_dump_a || (long)g_draw_no > g_dump_b) return;
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* res = nullptr; rtv->GetResource(&res);
    D3D11_RENDER_TARGET_VIEW_DESC vd; rtv->GetDesc(&vd);
    rtv->Release();
    ID3D11Texture2D* tex = nullptr;
    if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) { if (res) res->Release(); return; }
    res->Release();
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    UINT mip = vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? vd.Texture2D.MipSlice : 0;
    UINT slice = vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY ? vd.Texture2DArray.FirstArraySlice : 0;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Width = std::max(1u, d.Width >> mip); sd.Height = std::max(1u, d.Height >> mip);
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    if (dev && d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        c->CopySubresourceRegion(st, 0, 0, 0, 0, tex, D3D11CalcSubresource(mip, slice, d.MipLevels), nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            char path[1200];
            std::snprintf(path, sizeof path, "%s/dump", hp_userdir());
            mkdir(path, 0777);
            std::snprintf(path, sizeof path, "%s/dump/f%lu_d%03u.raw", hp_userdir(), g_frame, g_draw_no);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fprintf(f, "%u %u %u %u\n", sd.Width, sd.Height, unsigned(d.Format), m.RowPitch);
                std::fwrite(m.pData, 1, (size_t)m.RowPitch * sd.Height, f);
                std::fclose(f);
                w64_log("[trace] dumped draw %u: %ux%u fmt %u (view fmt %u) mip %u slice %u -> %s", g_draw_no, sd.Width, sd.Height,
                        unsigned(d.Format), unsigned(vd.Format), mip, slice, path);
            }
            c->Unmap(st, 0);
        }
        st->Release();
    }
    if (dev) dev->Release();
    tex->Release();
}

// ---- HP_RESLOG: live D3D11 resource memory by kind ---------------------------------------------
// Every created buffer/texture gets a private-data object whose last Release (when DXVK destroys
// the resource) takes its estimated size off again; hp_reslog_report() logs the totals.
enum { RK_TEX, RK_CUBE, RK_RT, RK_DEPTH, RK_UAVTEX, RK_TEX3D, RK_STAGING, RK_DYNAMIC, RK_VB, RK_IB, RK_CB, RK_SBUF, RK_BUF, RK_N };
static const char* const rk_name[RK_N] = { "tex", "cube", "rt", "depth", "uav-tex", "tex3d", "staging", "dynamic", "vb", "ib", "cb", "sbuf", "buf" };
static std::atomic<int64_t> g_rk_bytes[RK_N], g_rk_count[RK_N], g_rk_peak;
static const GUID HP_RES_GUID = { 0x5e7d0001, 0x48b2, 0x4c1e, { 0x9a, 0x51, 0x68, 0x70, 0x2d, 0x72, 0x65, 0x73 } };
struct ResTrack final : public IUnknown {
    std::atomic<ULONG> ref{ 1 }; int kind = 0; int64_t bytes = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { if (p) *p = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref;
        if (!r) { g_rk_bytes[kind] -= bytes; g_rk_count[kind]--; delete this; }
        return r;
    }
};
static uint32_t fmt_bits(DXGI_FORMAT f, bool* bc) {
    *bc = false;
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: *bc = true; return 4;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: *bc = true; return 8;
    default: break;
    }
    if (f >= DXGI_FORMAT_R32G32B32A32_TYPELESS && f <= DXGI_FORMAT_R32G32B32A32_SINT) return 128;
    if (f >= DXGI_FORMAT_R32G32B32_TYPELESS && f <= DXGI_FORMAT_R32G32B32_SINT) return 96;
    if (f >= DXGI_FORMAT_R16G16B16A16_TYPELESS && f <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) return 64;
    if (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_X24_TYPELESS_G8_UINT) return 32;
    if (f >= DXGI_FORMAT_R8G8_TYPELESS && f <= DXGI_FORMAT_R16_SINT) return 16;
    if (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_A8_UNORM) return 8;
    if (f >= DXGI_FORMAT_B5G6R5_UNORM && f <= DXGI_FORMAT_B5G5R5A1_UNORM) return 16;
    return 32;
}
static int64_t tex_bytes(uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t arr, DXGI_FORMAT f) {
    bool bc; uint32_t bits = fmt_bits(f, &bc);
    if (!mips) { uint32_t m = std::max(w, std::max(h, d)); mips = 1; while (m > 1) { m >>= 1; mips++; } }
    int64_t t = 0;
    for (uint32_t i = 0; i < mips; i++) {
        uint64_t mw = std::max(1u, w >> i), mh = std::max(1u, h >> i), md = std::max(1u, d >> i);
        t += bc ? (int64_t)((mw + 3) / 4 * ((mh + 3) / 4) * bits * 2 * md) : (int64_t)(mw * mh * md * bits / 8);
    }
    return t * arr;
}
extern "C" void hp_reslog_report(void) {
    char line[1024]; int o = 0; int64_t tot = 0;
    for (int k = 0; k < RK_N; k++) {
        int64_t b = g_rk_bytes[k];
        tot += b;
        if (g_rk_count[k]) o += std::snprintf(line + o, sizeof line - o, " %s %lld MB/%lld", rk_name[k], (long long)(b >> 20), (long long)g_rk_count[k].load());
    }
    if (tot > g_rk_peak) g_rk_peak = tot;
    w64_log("[res] live %lld MB (peak %lld):%s", (long long)(tot >> 20), (long long)(g_rk_peak >> 20), line);
}
static void res_track(uint64_t out, int kind, int64_t bytes) {
    auto* r = out ? *reinterpret_cast<ID3D11Resource**>(out) : nullptr;
    if (!r) return;
    g_rk_bytes[kind] += bytes; g_rk_count[kind]++;
    auto* t = new ResTrack; t->kind = kind; t->bytes = bytes;
    r->SetPrivateDataInterface(HP_RES_GUID, t);
    t->Release();
    static std::atomic<unsigned> n{ 0 };
    if (++n % 2000 == 0 || bytes >= (32 << 20)) {
        if (bytes >= (32 << 20)) w64_log("[res] big %s %lld MB", rk_name[kind], (long long)(bytes >> 20));
        hp_reslog_report();
    }
}
static int tex_kind(UINT usage, UINT bind, UINT misc, bool is3d) {
    if (usage == D3D11_USAGE_STAGING) return RK_STAGING;
    if (bind & D3D11_BIND_DEPTH_STENCIL) return RK_DEPTH;
    if (bind & D3D11_BIND_RENDER_TARGET) return RK_RT;
    if (bind & D3D11_BIND_UNORDERED_ACCESS) return RK_UAVTEX;
    if (is3d) return RK_TEX3D;
    if (misc & D3D11_RESOURCE_MISC_TEXTURECUBE) return RK_CUBE;
    if (usage == D3D11_USAGE_DYNAMIC) return RK_DYNAMIC;
    return RK_TEX;
}

using NFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" uint64_t hp_native_trace(int kind, uint64_t fn, const uint64_t* a) {
    auto call = [&]() { return reinterpret_cast<NFn>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13]); };
    auto* c = reinterpret_cast<ID3D11DeviceContext*>(a[0]);
    switch (kind) {
    case NK_TEX2D: {            // (this, desc*, initial data*, out**): HP_TEXLOG / HP_RESLOG
        auto* d = reinterpret_cast<const D3D11_TEXTURE2D_DESC*>(a[1]);
        static unsigned long n, bytes;
        if (d && d->Width * d->Height >= 512 * 512 && getenv("HP_TEXLOG")) {
            n++; bytes += (unsigned long)d->Width * d->Height * d->ArraySize / 1024;
            if (n <= 60 || n % 100 == 0)
                w64_log("[tex] #%lu %ux%u mips %u arr %u fmt %u bind %#x  (sum ~%lu MB of px)", n, d->Width, d->Height, d->MipLevels,
                        d->ArraySize, unsigned(d->Format), d->BindFlags, bytes / 1024);
        }
        uint64_t r = call();
        if (uint32_t(r) == 0 && d) {
            int k = tex_kind(d->Usage, d->BindFlags, d->MiscFlags, false);
            int64_t b = tex_bytes(d->Width, d->Height, 1, d->MipLevels, d->ArraySize, d->Format) * std::max(1u, d->SampleDesc.Count);
            static int verbose = -1;
            if (verbose < 0) { const char* e = getenv("HP_RESLOG"); verbose = e && atoi(e) >= 2; }
            if (verbose && b >= (2 << 20) && k != RK_TEX)
                w64_log("[res] %s %ux%u mips %u arr %u fmt %u ms %u bind %#x misc %#x: %lld KB", rk_name[k], d->Width, d->Height, d->MipLevels,
                        d->ArraySize, unsigned(d->Format), d->SampleDesc.Count, d->BindFlags, d->MiscFlags, (long long)(b >> 10));
            res_track(a[3], k, b);
        }
        return r;
    }
    case NK_TEX1D: {
        auto* d = reinterpret_cast<const D3D11_TEXTURE1D_DESC*>(a[1]);
        uint64_t r = call();
        if (uint32_t(r) == 0 && d) res_track(a[3], tex_kind(d->Usage, d->BindFlags, d->MiscFlags, false), tex_bytes(d->Width, 1, 1, d->MipLevels, d->ArraySize, d->Format));
        return r;
    }
    case NK_TEX3D: {
        auto* d = reinterpret_cast<const D3D11_TEXTURE3D_DESC*>(a[1]);
        uint64_t r = call();
        if (uint32_t(r) == 0 && d) res_track(a[3], tex_kind(d->Usage, d->BindFlags, d->MiscFlags, true), tex_bytes(d->Width, d->Height, d->Depth, d->MipLevels, 1, d->Format));
        return r;
    }
    case NK_BUF: {
        auto* d = reinterpret_cast<const D3D11_BUFFER_DESC*>(a[1]);
        uint64_t r = call();
        if (uint32_t(r) == 0 && d) {
            int k = d->Usage == D3D11_USAGE_STAGING ? RK_STAGING : (d->BindFlags & D3D11_BIND_VERTEX_BUFFER) ? RK_VB
                  : (d->BindFlags & D3D11_BIND_INDEX_BUFFER) ? RK_IB : (d->BindFlags & D3D11_BIND_CONSTANT_BUFFER) ? RK_CB
                  : (d->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || (d->BindFlags & D3D11_BIND_UNORDERED_ACCESS) ? RK_SBUF : RK_BUF;
            res_track(a[3], k, d->ByteWidth);
        }
        return r;
    }
    case NK_TR_MAP: {           // (this, res, sub, type, flags, D3D11_MAPPED_SUBRESOURCE*)
        uint64_t r = call();
        if (uint32_t(r) == 0 && a[5]) {
            auto* res = reinterpret_cast<ID3D11Resource*>(a[1]);
            size_t n = buf_size(res);
            if (n) { std::lock_guard<std::mutex> l(g_tr_mu); Mem& m = g_mem[res]; m.p = *reinterpret_cast<uint8_t**>(a[5]); m.n = n; m.copy.clear(); }
        }
        return r;
    }
    case NK_TR_UPD: case NK_TR_UPD1: {  // (this, dst, sub, box, src, rowpitch, depthpitch[, flags])
        auto* res = reinterpret_cast<ID3D11Resource*>(a[1]);
        size_t n = buf_size(res);
        if (n && a[4]) {
            std::lock_guard<std::mutex> l(g_tr_mu);
            Mem& m = g_mem[res];
            if (m.copy.size() != n) { m.copy.assign(n, 0); }
            m.n = n; m.p = nullptr;
            const D3D11_BOX* box = reinterpret_cast<const D3D11_BOX*>(a[3]);
            size_t lo = box ? box->left : 0, hi = box ? box->right : n;
            if (hi > n) hi = n;
            if (hi > lo) std::memcpy(m.copy.data() + lo, reinterpret_cast<const void*>(a[4]), hi - lo);
        }
        return call();
    }
    case NK_TR_DI: case NK_TR_D: case NK_TR_DII: case NK_TR_DIN: {
        static unsigned long last_frame;
        if (g_frame != last_frame) { last_frame = g_frame; g_draw_no = 0; }
        g_draw_no++;
        if (tr_now()) {
            if (kind == NK_TR_DI) tr_draw(c, "IDX", uint32_t(a[1]), uint32_t(a[2]), int32_t(a[3]), 1);
            else if (kind == NK_TR_D) tr_draw(c, "DRW", uint32_t(a[1]), uint32_t(a[2]), 0, 1);
            else if (kind == NK_TR_DII) tr_draw(c, "IDXI", uint32_t(a[1]), uint32_t(a[3]), int32_t(a[4]), uint32_t(a[2]));
            else tr_draw(c, "DRWI", uint32_t(a[1]), uint32_t(a[3]), 0, uint32_t(a[2]));
        }
        uint64_t r = call();
        dump_rt(c);
        return r;
    }
    }
    return call();
}

extern "C" int hp_native_classify(void* self, uint64_t fn) {
    static const Slot ctx[] = {
        { vidx(&ID3D11DeviceContext::ClearDepthStencilView), NK_CLEAR_DSV },
        { vidx(&ID3D11DeviceContext::SetResourceMinLOD), NK_SET_MINLOD },
        { vidx(&ID3D11DeviceContext::GetResourceMinLOD), NK_GET_MINLOD },
    };
    static const Slot adapter[] = {
        { vidx(&IDXGIAdapter::GetDesc), NK_DESC_FIRST + D_ADAPTER },
        { vidx(&IDXGIAdapter1::GetDesc1), NK_DESC_FIRST + D_ADAPTER1 },
        { vidx(&IDXGIAdapter2::GetDesc2), NK_DESC_FIRST + D_ADAPTER2 },
        { vidx(&IDXGIAdapter4::GetDesc3), NK_DESC_FIRST + D_ADAPTER3 },
    };
    static const Slot output[] = {
        { vidx(&IDXGIOutput::GetDesc), NK_DESC_FIRST + D_OUTPUT },
        { vidx(&IDXGIOutput6::GetDesc1), NK_DESC_FIRST + D_OUTPUT1 },
    };
    static const Slot swap[] = {
        { vidx(&IDXGISwapChain::Present), NK_PRESENT },
        { vidx(&IDXGISwapChain1::Present1), NK_PRESENT1 },
        { vidx(&IDXGISwapChain::ResizeBuffers), NK_RESIZE_BUFFERS },
    };
    static const Slot factory[] = {
        { vidx(&IDXGIFactory::CreateSwapChain), NK_CREATE_SC },
        { vidx(&IDXGIFactory2::CreateSwapChainForHwnd), NK_CREATE_SC_HWND },
    };
    if (!g_logged_slots) {
        g_logged_slots = true;
        w64_log("[d3d11] vtable slots: ClearDSV %d MinLOD %d/%d, adapter desc %d/%d/%d/%d, output desc %d/%d",
                ctx[0].idx, ctx[1].idx, ctx[2].idx, adapter[0].idx, adapter[1].idx, adapter[2].idx, adapter[3].idx,
                output[0].idx, output[1].idx);
    }
    int k;
    if ((k = probe<ID3D11DeviceContext>(self, fn, ctx, 3))) return k;
    if (getenv("HP_TEXLOG") || getenv("HP_RESLOG")) {
        static const Slot dev[] = {
            { vidx(&ID3D11Device::CreateTexture2D), NK_TEX2D }, { vidx(&ID3D11Device::CreateTexture1D), NK_TEX1D },
            { vidx(&ID3D11Device::CreateTexture3D), NK_TEX3D }, { vidx(&ID3D11Device::CreateBuffer), NK_BUF },
        };
        if ((k = probe<ID3D11Device>(self, fn, dev, 4))) return k;
    }
    if (trace_on()) {
        static const Slot tr[] = {
            { vidx(&ID3D11DeviceContext::Map), NK_TR_MAP },
            { vidx(&ID3D11DeviceContext::UpdateSubresource), NK_TR_UPD },
            { vidx(&ID3D11DeviceContext1::UpdateSubresource1), NK_TR_UPD1 },
            { vidx(&ID3D11DeviceContext::DrawIndexed), NK_TR_DI },
            { vidx(&ID3D11DeviceContext::Draw), NK_TR_D },
            { vidx(&ID3D11DeviceContext::DrawIndexedInstanced), NK_TR_DII },
            { vidx(&ID3D11DeviceContext::DrawInstanced), NK_TR_DIN },
        };
        if ((k = probe<ID3D11DeviceContext1>(self, fn, tr, 7))) return k;
    }
    if ((k = probe<IDXGIAdapter4>(self, fn, adapter, 4))) return k;
    if ((k = probe<IDXGIAdapter1>(self, fn, adapter, 2))) return k;
    if ((k = probe<IDXGIOutput6>(self, fn, output, 2))) return k;
    if ((k = probe<IDXGIOutput>(self, fn, output, 1))) return k;
    if ((k = probe<IDXGISwapChain1>(self, fn, swap, 3))) return k;
    if ((k = probe<IDXGISwapChain>(self, fn, swap, 3))) return k;
    if ((k = probe<IDXGIFactory2>(self, fn, factory, 2))) return k;
    if ((k = probe<IDXGIFactory>(self, fn, factory, 1))) return k;
    // Fast register-only dispatch for hot ID3D11DeviceContext1 methods (avoids 10 guest stack reads/call)
    static const Slot ctx_fast[] = {
        { vidx(&ID3D11DeviceContext::VSSetConstantBuffers), NK_REG4 },
        { vidx(&ID3D11DeviceContext::PSSetShaderResources), NK_REG4 },
        { vidx(&ID3D11DeviceContext::PSSetShader), NK_REG4 },
        { vidx(&ID3D11DeviceContext::PSSetSamplers), NK_REG4 },
        { vidx(&ID3D11DeviceContext::VSSetShader), NK_REG4 },
        { vidx(&ID3D11DeviceContext::DrawIndexed), NK_REG4 },
        { vidx(&ID3D11DeviceContext::Draw), NK_REG4 },
        { vidx(&ID3D11DeviceContext::Unmap), NK_REG4 },
        { vidx(&ID3D11DeviceContext::PSSetConstantBuffers), NK_REG4 },
        { vidx(&ID3D11DeviceContext::IASetInputLayout), NK_REG4 },
        { vidx(&ID3D11DeviceContext::IASetIndexBuffer), NK_REG4 },
        { vidx(&ID3D11DeviceContext::GSSetConstantBuffers), NK_REG4 },
        { vidx(&ID3D11DeviceContext::GSSetShader), NK_REG4 },
        { vidx(&ID3D11DeviceContext::IASetPrimitiveTopology), NK_REG4 },
        { vidx(&ID3D11DeviceContext::VSSetShaderResources), NK_REG4 },
        { vidx(&ID3D11DeviceContext::VSSetSamplers), NK_REG4 },
        { vidx(&ID3D11DeviceContext::OMSetRenderTargets), NK_REG4 },
        { vidx(&ID3D11DeviceContext::OMSetBlendState), NK_REG4 },
        { vidx(&ID3D11DeviceContext::OMSetDepthStencilState), NK_REG4 },
        { vidx(&ID3D11DeviceContext::RSSetState), NK_REG4 },
        { vidx(&ID3D11DeviceContext::RSSetViewports), NK_REG4 },
        { vidx(&ID3D11DeviceContext::RSSetScissorRects), NK_REG4 },
        { vidx(&ID3D11DeviceContext::ClearRenderTargetView), NK_REG4 },
        { vidx(&ID3D11DeviceContext::CopyResource), NK_REG4 },
        { vidx(&ID3D11DeviceContext::Dispatch), NK_REG4 },
        { vidx(&ID3D11DeviceContext::Flush), NK_REG4 },
        { vidx(&ID3D11DeviceContext::CSSetShader), NK_REG4 },
        { vidx(&ID3D11DeviceContext::CSSetShaderResources), NK_REG4 },
        { vidx(&ID3D11DeviceContext::CSSetConstantBuffers), NK_REG4 },
        { vidx(&ID3D11DeviceContext::Map), NK_REG8 },
        { vidx(&ID3D11DeviceContext::IASetVertexBuffers), NK_REG8 },
        { vidx(&ID3D11DeviceContext::DrawIndexedInstanced), NK_REG8 },
        { vidx(&ID3D11DeviceContext::DrawInstanced), NK_REG8 },
        { vidx(&ID3D11DeviceContext::UpdateSubresource), NK_REG8 },
        { vidx(&ID3D11DeviceContext1::VSSetConstantBuffers1), NK_REG8 },
        { vidx(&ID3D11DeviceContext1::PSSetConstantBuffers1), NK_REG8 },
    };
    if ((k = probe<ID3D11DeviceContext1>(self, fn, ctx_fast, (int)(sizeof ctx_fast / sizeof ctx_fast[0])))) return k;
    return NK_GENERIC;
}

// GetDesc*(this, out): call with a native-layout struct, convert the name to UTF-16.
extern "C" int32_t hp_native_desc_call(int kind, void* self, uint64_t fn, void* out) {
    alignas(16) uint8_t buf[2048];
    std::memset(buf, 0, sizeof buf);
    using F = HRESULT (*)(void*, void*);
    HRESULT hr = reinterpret_cast<F>(fn)(self, out ? buf : nullptr);
    if (FAILED(hr) || !out) return hr;
    size_t nchars = 0, nsize = 0;
    switch (kind - NK_DESC_FIRST) {
    case D_ADAPTER:  nchars = 128; nsize = sizeof(DXGI_ADAPTER_DESC); break;
    case D_ADAPTER1: nchars = 128; nsize = sizeof(DXGI_ADAPTER_DESC1); break;
    case D_ADAPTER2: nchars = 128; nsize = sizeof(DXGI_ADAPTER_DESC2); break;
    case D_ADAPTER3: nchars = 128; nsize = sizeof(DXGI_ADAPTER_DESC3); break;
    case D_OUTPUT:   nchars = 32;  nsize = sizeof(DXGI_OUTPUT_DESC); break;
    case D_OUTPUT1:  nchars = 32;  nsize = sizeof(DXGI_OUTPUT_DESC1); break;
    default: return hr;
    }
    const WCHAR* name = reinterpret_cast<const WCHAR*>(buf);
    uint16_t* g = static_cast<uint16_t*>(out);
    for (size_t i = 0; i < nchars; i++) g[i] = uint16_t(name[i]);
    std::memcpy(static_cast<uint8_t*>(out) + nchars * 2, buf + nchars * sizeof(WCHAR), nsize - nchars * sizeof(WCHAR));
    return hr;
}

extern "C" int32_t hp_d3d11_create_device(void* adapter, uint32_t driver, uint32_t flags, const void* levels,
                                          uint32_t nlevels, uint32_t sdk, void** device, void* level, void** context) {
#ifndef __ANDROID__
    // host test: present into an X window (Xvfb), sized like the runtime's screen
    if (!hp_wsi_window_handle()) {
        uint32_t w = 1280, h = 720;
        if (const char* s = getenv("W64_SCREEN")) std::sscanf(s, "%ux%u", &w, &h);
        hp_wsi_host_open(w, h);
    }
#endif
    flags &= ~uint32_t(D3D11_CREATE_DEVICE_DEBUG);
    HRESULT hr = D3D11CreateDevice(static_cast<IDXGIAdapter*>(adapter), D3D_DRIVER_TYPE(driver), nullptr, flags,
                                   static_cast<const D3D_FEATURE_LEVEL*>(levels), nlevels, sdk,
                                   reinterpret_cast<ID3D11Device**>(device), static_cast<D3D_FEATURE_LEVEL*>(level),
                                   reinterpret_cast<ID3D11DeviceContext**>(context));
    unsigned fl = 0;
    if (device && *device) fl = static_cast<ID3D11Device*>(*device)->GetFeatureLevel();
    w64_log("[d3d11] D3D11CreateDevice(adapter %p, driver %u, flags %#x, %u levels) -> %#x, feature level %#x",
            adapter, driver, flags, nlevels, unsigned(hr), fl);
    return hr;
}

extern "C" int32_t hp_dxgi_create_factory(uint32_t flags, const void* riid, void** out) {
    HRESULT hr = CreateDXGIFactory1(*static_cast<const IID*>(riid), out);
    w64_log("[dxgi] CreateDXGIFactory(flags %#x) -> %#x %p", flags, unsigned(hr), out ? *out : nullptr);
    return hr;
}

// IDXGISwapChain::Present / IDXGISwapChain1::Present1. Waits while the app has no surface
// (paused), and keeps the frame latency waitable object counting when DXVK skips a present
// (DXGI_STATUS_OCCLUDED, no swap chain): on Windows it is signalled for every Present, and Unity
// waits on it before each frame.
extern "C" unsigned long hp_present_count(void) { return __atomic_load_n(&g_frame, __ATOMIC_RELAXED); }
static std::atomic<uint32_t> g_st_gp_n{0}, g_st_cp_n{0}, g_st_sp_n{0};
static std::atomic<uint64_t> g_st_gp_us{0}, g_st_cp_us{0}, g_st_sp_us{0};
static int stutter_on() {
    static int on = -1;
    if (on < 0) on = getenv("HP_STUTTER") && atoi(getenv("HP_STUTTER")) != 0 ? 1 : 0;
    return on;
}
extern "C" void hp_stutter_add_pipe(int is_compute, uint64_t us) {
    if (!stutter_on()) return;
    if (is_compute) { g_st_cp_n.fetch_add(1, std::memory_order_relaxed); g_st_cp_us.fetch_add(us, std::memory_order_relaxed); }
    else            { g_st_gp_n.fetch_add(1, std::memory_order_relaxed); g_st_gp_us.fetch_add(us, std::memory_order_relaxed); }
}
extern "C" void hp_stutter_add_susp(uint64_t us) {
    if (!stutter_on()) return;
    g_st_sp_n.fetch_add(1, std::memory_order_relaxed);
    g_st_sp_us.fetch_add(us, std::memory_order_relaxed);
}
static uint64_t mono_us() {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}
extern "C" uint32_t g_w64_com_calls;
extern "C" uint32_t g_w64_tramp_calls;
extern "C" int32_t hp_native_present(int kind, void* self, uint64_t fn, uint32_t sync, uint32_t flags, const void* params) {
    static int sync_ovr = -2;
    if (sync_ovr == -2) { const char* e = getenv("HP_SYNC_INTERVAL"); sync_ovr = e && *e ? atoi(e) : -1; }
    if (sync_ovr >= 0) sync = (uint32_t)sync_ovr;
    int st = stutter_on();
    uint64_t pt0 = st ? mono_us() : 0;
    hp_wsi_present_begin();
    HRESULT hr;
    if (kind == NK_PRESENT1)
        hr = reinterpret_cast<HRESULT (*)(void*, UINT, UINT, const void*)>(fn)(self, sync, flags, params);
    else
        hr = reinterpret_cast<HRESULT (*)(void*, UINT, UINT)>(fn)(self, sync, flags);
    hp_wsi_present_end();
    uint64_t pt1 = st ? mono_us() : 0;
    static unsigned long n, bad;
    n++;
    __atomic_store_n(&g_frame, n, __ATOMIC_RELAXED);
    if (st && !(flags & DXGI_PRESENT_TEST)) {
        static uint64_t prev_t = 0;
        static float dts[150], prs[150];
        static int dti = 0;
        static uint32_t bkt[6], w_gp_n, w_cp_n, w_sp_n;
        static uint64_t w_gp_us, w_cp_us, w_sp_us;
        static double pr_sum = 0.0;
        uint32_t gp_n = g_st_gp_n.exchange(0, std::memory_order_relaxed);
        uint64_t gp_us = g_st_gp_us.exchange(0, std::memory_order_relaxed);
        uint32_t cp_n = g_st_cp_n.exchange(0, std::memory_order_relaxed);
        uint64_t cp_us = g_st_cp_us.exchange(0, std::memory_order_relaxed);
        uint32_t sp_n = g_st_sp_n.exchange(0, std::memory_order_relaxed);
        uint64_t sp_us = g_st_sp_us.exchange(0, std::memory_order_relaxed);
        w_gp_n += gp_n; w_gp_us += gp_us; w_cp_n += cp_n; w_cp_us += cp_us; w_sp_n += sp_n; w_sp_us += sp_us;
        if (prev_t) {
            float dt = (float)(pt1 - prev_t) / 1000.0f;
            float pr = (float)(pt1 - pt0) / 1000.0f;
            int bi = dt < 20 ? 0 : dt < 30 ? 1 : dt < 45 ? 2 : dt < 80 ? 3 : dt < 150 ? 4 : 5;
            bkt[bi]++;
            if (dt >= 45.0f) {
                static int spk = 0;
                if (spk++ < 120)
                    w64_log("[spike] f%lu dt=%.1fms pres=%.1fms sync=%u | gp=%u(%.1fms) cp=%u(%.1fms) susp=%u(%.1fms)",
                            n, dt, pr, sync, gp_n, gp_us / 1000.0, cp_n, cp_us / 1000.0, sp_n, sp_us / 1000.0);
            }
            dts[dti] = dt;
            prs[dti] = pr;
            pr_sum += pr;
            dti++;
            if (dti == 150) {
                std::sort(dts, dts + 150);
                std::sort(prs, prs + 150);
                uint32_t com_n = __atomic_exchange_n(&g_w64_com_calls, 0, __ATOMIC_RELAXED);
                uint32_t tr_n = __atomic_exchange_n(&g_w64_tramp_calls, 0, __ATOMIC_RELAXED);
                w64_log("[stutter] 150f sync=%u p50=%.1f p95=%.1f p99=%.1f max=%.1f ms (pres p50=%.1f avg=%.1f) | com=%u/f tramp=%u/f | <20:%u 20-30:%u 30-45:%u 45-80:%u 80-150:%u >150:%u | gp=%u(%.0fms) cp=%u(%.0fms) susp=%u(%.0fms)",
                        sync, dts[75], dts[142], dts[148], dts[149], prs[75], pr_sum / 150.0, com_n / 150u, tr_n / 150u,
                        bkt[0], bkt[1], bkt[2], bkt[3], bkt[4], bkt[5],
                        w_gp_n, w_gp_us / 1000.0, w_cp_n, w_cp_us / 1000.0, w_sp_n, w_sp_us / 1000.0);
                dti = 0; pr_sum = 0.0; std::memset(bkt, 0, sizeof bkt);
                w_gp_n = w_cp_n = w_sp_n = 0; w_gp_us = w_cp_us = w_sp_us = 0;
            }
        }
        prev_t = pt1;
    }
    if (hr != S_OK && !(flags & DXGI_PRESENT_TEST)) {
        if (bad++ < 20) w64_log("[dxgi] Present #%lu (sync %u flags %#x) -> %#x", n, sync, flags, unsigned(hr));
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(self)->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&sc2))) && sc2) {
            HANDLE h = sc2->GetFrameLatencyWaitableObject();
            if (h) w64_sem_release(reinterpret_cast<uint64_t>(h));
            sc2->Release();
        }
    }
    if (n == 1 || n == 100 || n == 1000) w64_log("[dxgi] %lu presents", n);
    static struct timespec t0;
    if (n % 300 == 1) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        if (n > 1) w64_log("[dxgi] %.1f fps", 300.0 / ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9));
        t0 = t;
    }
    return hr;
}
