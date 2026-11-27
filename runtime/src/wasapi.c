/*
 * wasapi.c - the Windows Core Audio interfaces Unity's FMOD output uses, as COM objects the
 * recompiled code calls directly (vtables of host-function sentinels).
 *
 *   CoCreateInstance(CLSID_MMDeviceEnumerator) -> IMMDeviceEnumerator -> IMMDevice (one render
 *   endpoint, 48 kHz stereo float) -> IAudioClient(3) -> IAudioRenderClient / IAudioClock /
 *   ISimpleAudioVolume.
 *
 * Shared mode only. The client's buffer is a ring; a pump thread takes one period at a time,
 * hands it to the platform sink (SDL audio stream on Android, android_main.c; none on the host,
 * where the pump just keeps time) and signals the client's event - FMOD's mixer thread waits on
 * it and refills (GetCurrentPadding / GetBuffer / ReleaseBuffer).
 */
#define _GNU_SOURCE
#include "w64.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>

void w64_sem_release(uint64_t h);

/* platform sink (weak: the host build has none) */
__attribute__((weak)) int w64_audio_sink_open(int rate, int channels) { (void)rate; (void)channels; return -1; }
__attribute__((weak)) void w64_audio_sink_write(const float *frames, int n) { (void)frames; (void)n; }
__attribute__((weak)) int w64_audio_sink_queued(void) { return 0; }

#define S_OK_ 0u
#define E_NOINTERFACE_ 0x80004002u
#define E_POINTER_ 0x80004003u
#define E_NOTFOUND_ 0x80070490u
#define AUDCLNT_E_NOT_INITIALIZED 0x88890001u
#define AUDCLNT_E_ALREADY_INITIALIZED 0x88890002u
#define AUDCLNT_E_BUFFER_TOO_LARGE 0x88890006u
#define AUDCLNT_E_UNSUPPORTED_FORMAT 0x88890008u
#define RATE 48000
#define CH 2

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } guid_t;
static const guid_t CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const guid_t IID_IUnknown_ = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const guid_t IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const guid_t IID_IMMEndpoint = { 0x1BE09788, 0x6894, 0x4089, { 0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5 } };
static const guid_t IID_IMMDevice = { 0xD666063F, 0x1587, 0x4E43, { 0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F } };
static const guid_t IID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const guid_t IID_IAudioClient2 = { 0x726778CD, 0xF60A, 0x4EDA, { 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA } };
static const guid_t IID_IAudioClient3 = { 0x7ED4EE07, 0x8E67, 0x4CD4, { 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42 } };
static const guid_t IID_IAudioRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const guid_t IID_IAudioClock = { 0xCD63314F, 0x3FBA, 0x4A1B, { 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7 } };
static const guid_t IID_ISimpleAudioVolume = { 0x87CE5498, 0x68D6, 0x44E5, { 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8 } };
static const guid_t KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const guid_t KSDATAFORMAT_SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
/* PKEY_Device_FriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0},14 */
static const guid_t PKEY_Device_FriendlyName = { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } };

static int geq(uint64_t p, const guid_t *g) { return p && !memcmp((const void *)(uintptr_t)p, g, 16); }
static char *gstr(uint64_t p)
{
    static __thread char b[40];
    if (!p) return "(null)";
    const guid_t *g = (const guid_t *)(uintptr_t)p;
    snprintf(b, sizeof b, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g->a, g->b, g->c,
             g->d[0], g->d[1], g->d[2], g->d[3], g->d[4], g->d[5], g->d[6], g->d[7]);
    return b;
}

/* ---- objects: { vtbl, refs, kind, ... } in host memory the guest reads directly ---- */
typedef struct obj { uint64_t vtbl; int32_t refs; int kind; } obj_t;
enum { K_ENUM, K_DEVICE, K_COLL, K_PROPS, K_CLIENT, K_RENDER, K_CLOCK, K_VOLUME, K_ENDPOINT };

typedef struct {
    obj_t o;
    /* shared state of the one stream */
    int inited, started;
    uint32_t buf_frames, period_frames, fmt_ch, fmt_bits, fmt_float;
    uint8_t *ring;                  /* buf_frames * frame bytes */
    uint32_t frame_bytes, rd, wr, fill;
    uint64_t event, played;
    uint8_t *stage; size_t stage_n; int staged;     /* GetBuffer across the ring's end */
    pthread_mutex_t mu;
    pthread_t pump;
    float volume;
} client_t;
static client_t *g_client;          /* render client/clock/volume objects point here */
typedef struct { obj_t o; client_t *cl; } sub_t;

static uint64_t mkvtbl(const char **names, w64_fn *fns, int n)
{
    uint64_t *v = calloc((size_t)n, 8);
    for (int i = 0; i < n; i++) v[i] = w64_hostfn(names[i], fns[i]);
    return (uint64_t)(uintptr_t)v;
}

/* IUnknown for all of them (QueryInterface per kind below) */
static void qi_for(CPU *c, obj_t *o, uint64_t riid, uint64_t ppv);
static void QI_(CPU *c) { qi_for(c, AP(0), A(1), A(2)); }
static void AddRef_(CPU *c) { obj_t *o = AP(0); RET((uint32_t)__atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED)); }
static void Release_(CPU *c) { obj_t *o = AP(0); int r = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_RELAXED); RET((uint32_t)(r < 0 ? 0 : r)); }

static uint64_t g_vt[9];
static obj_t *new_obj(int kind, size_t size)
{
    obj_t *o = calloc(1, size);
    o->vtbl = g_vt[kind]; o->refs = 1; o->kind = kind;
    return o;
}

/* ---- IMMDeviceEnumerator ---- */
static obj_t *g_device;
static obj_t *device(void) { if (!g_device) g_device = new_obj(K_DEVICE, sizeof(obj_t)); __atomic_add_fetch(&g_device->refs, 1, __ATOMIC_RELAXED); return g_device; }
static void EnumAudioEndpoints_(CPU *c)       /* (this, flow, mask, IMMDeviceCollection**) */
{
    obj_t *col = new_obj(K_COLL, sizeof(obj_t));
    col->kind = K_COLL | (AU(1) == 1 ? 0x100 : 0);              /* capture: empty */
    wr64(A(3), (uint64_t)(uintptr_t)col);
    RET(S_OK_);
}
static void GetDefaultAudioEndpoint_(CPU *c)  /* (this, flow, role, IMMDevice**) */
{
    if (AU(1) != 0 /* eRender */) { wr64(A(3), 0); RET(E_NOTFOUND_); }
    wr64(A(3), (uint64_t)(uintptr_t)device());
    RET(S_OK_);
}
static void GetDevice_(CPU *c) { wr64(A(2), (uint64_t)(uintptr_t)device()); RET(S_OK_); }
static void RegisterCb_(CPU *c) { RET(S_OK_); }
/* ---- IMMDeviceCollection ---- */
static void CollGetCount_(CPU *c) { obj_t *o = AP(0); wr32(A(1), (o->kind & 0x100) ? 0 : 1); RET(S_OK_); }
static void CollItem_(CPU *c) { obj_t *o = AP(0); if ((o->kind & 0x100) || AU(1)) { wr64(A(2), 0); RET(E_NOTFOUND_); } wr64(A(2), (uint64_t)(uintptr_t)device()); RET(S_OK_); }
/* ---- IMMDevice ---- */
static client_t *new_client(void)
{
    client_t *cl = (client_t *)new_obj(K_CLIENT, sizeof(client_t));
    pthread_mutex_init(&cl->mu, NULL);
    cl->volume = 1.0f;
    return cl;
}
static void Activate_(CPU *c)                 /* (this, iid, ctx, params, void**) */
{
    uint64_t iid = A(1);
    if (geq(iid, &IID_IAudioClient) || geq(iid, &IID_IAudioClient2) || geq(iid, &IID_IAudioClient3)) {
        wr64(A(4), (uint64_t)(uintptr_t)new_client());
        RET(S_OK_);
    }
    w64_log("[wasapi] IMMDevice::Activate(%s): not provided", gstr(iid));
    wr64(A(4), 0);
    RET(E_NOINTERFACE_);
}
static void OpenPropertyStore_(CPU *c) { wr64(A(2), (uint64_t)(uintptr_t)new_obj(K_PROPS, sizeof(obj_t))); RET(S_OK_); }
static uint64_t co_wstr(const char *s)
{
    size_t n = strlen(s) + 1;
    uint16_t *w = malloc(n * 2);                         /* CoTaskMemFree = free (win_extra.c) */
    utf8_to_w16(s, w, (int)n);
    return (uint64_t)(uintptr_t)w;
}
static void GetId_(CPU *c) { wr64(A(1), co_wstr("{0.0.0.00000000}.{a1b2c3d4-0000-4000-8000-414e44524f49}")); RET(S_OK_); }
static void GetState_(CPU *c) { wr32(A(1), 1 /* DEVICE_STATE_ACTIVE */); RET(S_OK_); }
/* ---- IMMEndpoint (a separate object: QueryInterface on the device) ---- */
static void GetDataFlow_(CPU *c) { wr32(A(1), 0 /* eRender */); RET(S_OK_); }
/* ---- IPropertyStore ---- */
static void PropGetCount_(CPU *c) { wr32(A(1), 0); RET(S_OK_); }
static void PropGetAt_(CPU *c) { RET(0x80070057u); }
static void write_mixfmt(uint8_t *w);
/* {1da5d803-d492-4edd-8c23-e0c0ffee7f0e}: AudioEndpoint_FormFactor 0, _PhysicalSpeakers 3, _GUID 4 */
static const guid_t PKEY_AudioEndpoint = { 0x1DA5D803, 0xD492, 0x4EDD, { 0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E } };
/* {f19f064d-082c-4e27-bc73-6882a1bb8e4c}: AudioEngine_DeviceFormat 0, {e4870e26-...}: OEMFormat 3 */
static const guid_t PKEY_AudioEngine_DeviceFormat = { 0xF19F064D, 0x082C, 0x4E27, { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } };
static const guid_t PKEY_AudioEngine_OEMFormat = { 0xE4870E26, 0x3CC5, 0x4CD2, { 0xBA, 0x46, 0xCA, 0x0A, 0x9A, 0x70, 0xED, 0x04 } };
/* {026e516e-b814-414b-83cd-856d6fef4822},2: DeviceInterface_FriendlyName */
static const guid_t PKEY_DeviceInterface_FriendlyName = { 0x026E516E, 0xB814, 0x414B, { 0x83, 0xCD, 0x85, 0x6D, 0x6F, 0xEF, 0x48, 0x22 } };
static void PropGetValue_(CPU *c)             /* (this, PROPERTYKEY*, PROPVARIANT*) */
{
    uint64_t k = A(1), pv = A(2);
    uint32_t pid = rd32(k + 16);
    memset(AP(2), 0, 24);
    if ((geq(k, &PKEY_Device_FriendlyName) && (pid == 14 || pid == 2)) || (geq(k, &PKEY_DeviceInterface_FriendlyName) && pid == 2)) {
        wr16(pv, 31 /* VT_LPWSTR */);
        wr64(pv + 8, co_wstr(pid == 14 ? "Speakers (Android)" : "Speakers"));
    } else if ((geq(k, &PKEY_AudioEngine_DeviceFormat) && pid == 0) || (geq(k, &PKEY_AudioEngine_OEMFormat) && pid == 3)) {
        uint8_t *w = malloc(40);
        write_mixfmt(w);
        wr16(pv, 65 /* VT_BLOB */); wr32(pv + 8, 40); wr64(pv + 16, (uint64_t)(uintptr_t)w);
    } else if (geq(k, &PKEY_AudioEndpoint) && (pid == 0 || pid == 3)) {
        wr16(pv, 19 /* VT_UI4 */); wr32(pv + 8, pid == 0 ? 1 /* Speakers */ : 3 /* front L/R */);
    } else if (geq(k, &PKEY_AudioEndpoint) && pid == 4) {
        wr16(pv, 31); wr64(pv + 8, co_wstr("{a1b2c3d4-0000-4000-8000-414e44524f49}"));
    } else {
        w64_log("[wasapi] IPropertyStore::GetValue(%s,%u): empty", gstr(k), pid);
    }
    RET(S_OK_);
}
static void PropSetValue_(CPU *c) { RET(0x80030005u); }
static void PropCommit_(CPU *c) { RET(S_OK_); }

/* ---- IAudioClient ---- */
static void write_mixfmt(uint8_t *w)          /* WAVEFORMATEXTENSIBLE, 40 bytes */
{
    memset(w, 0, 40);
    uint16_t tag = 0xFFFE, ch = CH, bits = 32, align = CH * 4, cb = 22, valid = 32;
    uint32_t rate = RATE, bps = RATE * CH * 4, mask = 0x3;
    memcpy(w, &tag, 2); memcpy(w + 2, &ch, 2); memcpy(w + 4, &rate, 4); memcpy(w + 8, &bps, 4);
    memcpy(w + 12, &align, 2); memcpy(w + 14, &bits, 2); memcpy(w + 16, &cb, 2); memcpy(w + 18, &valid, 2);
    memcpy(w + 20, &mask, 4); memcpy(w + 24, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 16);
}
/* what the client may give: 48 kHz, 1-2 channels (or more, downmixed by dropping), float32 or s16 */
static int parse_fmt(uint64_t f, uint32_t *ch, uint32_t *bits, uint32_t *isfloat)
{
    uint16_t tag = rd16(f), nch = rd16(f + 2), bps = rd16(f + 14);
    uint32_t rate = rd32(f + 4);
    int fl;
    if (tag == 0xFFFE && rd16(f + 16) >= 22) fl = geq(f + 24, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? 1 : geq(f + 24, &KSDATAFORMAT_SUBTYPE_PCM) ? 0 : -1;
    else fl = tag == 3 ? 1 : tag == 1 ? 0 : -1;
    if (fl < 0 || rate != RATE || !nch || nch > 8) return 0;
    if (fl && bps != 32) return 0;
    if (!fl && bps != 16) return 0;
    *ch = nch; *bits = bps; *isfloat = (uint32_t)fl;
    return 1;
}
static void *pump_thread(void *arg);
static void Initialize_(CPU *c)               /* (this, mode, flags, hnsBuffer, hnsPeriod, fmt, session) */
{
    client_t *cl = AP(0);
    if (cl->inited) RET(AUDCLNT_E_ALREADY_INITIALIZED);
    uint32_t ch, bits, fl;
    if (AU(1) != 0 /* shared */ || !parse_fmt(A(5), &ch, &bits, &fl)) {
        w64_log("[wasapi] Initialize: mode %u, format tag %#x %u ch %u Hz %u bit - unsupported", AU(1), rd16(A(5)), rd16(A(5) + 2), rd32(A(5) + 4), rd16(A(5) + 14));
        RET(AUDCLNT_E_UNSUPPORTED_FORMAT);
    }
    int64_t hns = (int64_t)A(3);
    uint32_t period = RATE / 100;                                   /* 10 ms */
    uint32_t frames = (uint32_t)(hns > 0 ? hns * RATE / 10000000 : 0);
    if (frames < period * 2) frames = period * 2;
    if (frames > RATE) frames = RATE;
    cl->fmt_ch = ch; cl->fmt_bits = bits; cl->fmt_float = fl;
    cl->frame_bytes = ch * bits / 8;
    cl->buf_frames = frames; cl->period_frames = period;
    cl->ring = calloc(frames, cl->frame_bytes);
    cl->inited = 1;
    g_client = cl;
    w64_log("[wasapi] Initialize: %u ch %u-bit %s, flags %#x, buffer %u frames (asked %lld hns)", ch, bits, fl ? "float" : "pcm",
            AU(2), frames, (long long)hns);
    pthread_create(&cl->pump, NULL, pump_thread, cl);
    RET(S_OK_);
}
static void GetBufferSize_(CPU *c) { client_t *cl = AP(0); if (!cl->inited) RET(AUDCLNT_E_NOT_INITIALIZED); wr32(A(1), cl->buf_frames); RET(S_OK_); }
static void GetStreamLatency_(CPU *c) { wr64(A(1), 200000); RET(S_OK_); }
static void GetCurrentPadding_(CPU *c)
{
    client_t *cl = AP(0);
    if (!cl->inited) RET(AUDCLNT_E_NOT_INITIALIZED);
    pthread_mutex_lock(&cl->mu); uint32_t f = cl->fill; pthread_mutex_unlock(&cl->mu);
    wr32(A(1), f);
    RET(S_OK_);
}
static void IsFormatSupported_(CPU *c)        /* (this, mode, fmt, closest**) */
{
    uint32_t ch, bits, fl;
    if (A(3)) wr64(A(3), 0);
    if (AU(1) == 0 && parse_fmt(A(2), &ch, &bits, &fl)) RET(S_OK_);
    if (A(3) && AU(1) == 0) { uint8_t *w = malloc(40); write_mixfmt(w); wr64(A(3), (uint64_t)(uintptr_t)w); RET(1 /* S_FALSE */); }
    RET(AUDCLNT_E_UNSUPPORTED_FORMAT);
}
static void GetMixFormat_(CPU *c) { uint8_t *w = malloc(40); write_mixfmt(w); wr64(A(1), (uint64_t)(uintptr_t)w); RET(S_OK_); }
static void GetDevicePeriod_(CPU *c) { if (A(1)) wr64(A(1), 100000); if (A(2)) wr64(A(2), 30000); RET(S_OK_); }
static void Start_(CPU *c) { client_t *cl = AP(0); cl->started = 1; RET(S_OK_); }
static void Stop_(CPU *c) { client_t *cl = AP(0); cl->started = 0; RET(S_OK_); }
static void Reset_(CPU *c)
{
    client_t *cl = AP(0);
    pthread_mutex_lock(&cl->mu); cl->rd = cl->wr = cl->fill = 0; cl->played = 0; pthread_mutex_unlock(&cl->mu);
    RET(S_OK_);
}
static void SetEventHandle_(CPU *c) { client_t *cl = AP(0); cl->event = A(1); RET(S_OK_); }
static void GetService_(CPU *c)               /* (this, iid, void**) */
{
    client_t *cl = AP(0);
    uint64_t iid = A(1);
    int kind = geq(iid, &IID_IAudioRenderClient) ? K_RENDER : geq(iid, &IID_IAudioClock) ? K_CLOCK
             : geq(iid, &IID_ISimpleAudioVolume) ? K_VOLUME : -1;
    if (kind < 0) { w64_log("[wasapi] IAudioClient::GetService(%s): not provided", gstr(iid)); wr64(A(2), 0); RET(E_NOINTERFACE_); }
    sub_t *s = (sub_t *)new_obj(kind, sizeof(sub_t));
    s->cl = cl;
    wr64(A(2), (uint64_t)(uintptr_t)s);
    RET(S_OK_);
}
/* IAudioClient2/3 */
static void IsOffloadCapable_(CPU *c) { if (A(2)) wr32(A(2), 0); RET(S_OK_); }
static void SetClientProperties_(CPU *c) { RET(S_OK_); }
static void GetBufferSizeLimits_(CPU *c) { if (A(3)) wr64(A(3), 30000); if (A(4)) wr64(A(4), 20000000); RET(S_OK_); }
static void GetSharedModeEnginePeriod_(CPU *c)  /* (this, fmt, default, fundamental, min, max) */
{
    for (int i = 2; i <= 5; i++) if (A(i)) wr32(A(i), i == 2 || i == 5 ? RATE / 100 : RATE / 100);
    RET(S_OK_);
}
static void GetCurrentSharedModeEnginePeriod_(CPU *c) { if (A(1)) { uint8_t *w = malloc(40); write_mixfmt(w); wr64(A(1), (uint64_t)(uintptr_t)w); } if (A(2)) wr32(A(2), RATE / 100); RET(S_OK_); }
static void InitializeSharedAudioStream_(CPU *c) { RET(0x80004001u); }

/* ---- IAudioRenderClient ---- */
static void GetBuffer_(CPU *c)                /* (this, frames, BYTE**) */
{
    client_t *cl = ((sub_t *)AP(0))->cl;
    uint32_t n = AU(1);
    pthread_mutex_lock(&cl->mu);
    if (n > cl->buf_frames - cl->fill) { pthread_mutex_unlock(&cl->mu); wr64(A(2), 0); RET(AUDCLNT_E_BUFFER_TOO_LARGE); }
    uint64_t p;
    cl->staged = cl->wr + n > cl->buf_frames;      /* contiguous memory for a request that wraps */
    if (!cl->staged) p = (uint64_t)(uintptr_t)(cl->ring + (size_t)cl->wr * cl->frame_bytes);
    else {
        size_t need = (size_t)n * cl->frame_bytes;
        if (cl->stage_n < need) { free(cl->stage); cl->stage = malloc(need); cl->stage_n = need; }
        p = (uint64_t)(uintptr_t)cl->stage;
    }
    pthread_mutex_unlock(&cl->mu);
    wr64(A(2), p);
    RET(S_OK_);
}
static void ReleaseBuffer_(CPU *c)            /* (this, frames, flags) */
{
    client_t *cl = ((sub_t *)AP(0))->cl;
    uint32_t n = AU(1), fl = AU(2);
    pthread_mutex_lock(&cl->mu);
    if (n > cl->buf_frames - cl->fill) n = cl->buf_frames - cl->fill;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *dst = cl->ring + (size_t)((cl->wr + i) % cl->buf_frames) * cl->frame_bytes;
        if (fl & 2 /* AUDCLNT_BUFFERFLAGS_SILENT */) memset(dst, 0, cl->frame_bytes);
        else if (cl->staged) memcpy(dst, cl->stage + (size_t)i * cl->frame_bytes, cl->frame_bytes);
    }
    cl->staged = 0;
    cl->wr = (cl->wr + n) % cl->buf_frames;
    cl->fill += n;
    pthread_mutex_unlock(&cl->mu);
    RET(S_OK_);
}
/* ---- IAudioClock ---- */
static void ClockGetFrequency_(CPU *c) { wr64(A(1), RATE); RET(S_OK_); }
static void ClockGetPosition_(CPU *c)
{
    client_t *cl = ((sub_t *)AP(0))->cl;
    wr64(A(1), cl->played);
    if (A(2)) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); wr64(A(2), (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100); }
    RET(S_OK_);
}
static void ClockGetCharacteristics_(CPU *c) { wr32(A(1), 0); RET(S_OK_); }
/* ---- ISimpleAudioVolume ---- */
static void SetMasterVolume_(CPU *c) { client_t *cl = ((sub_t *)AP(0))->cl; cl->volume = c->xmm[1].f32[0]; RET(S_OK_); }
static void GetMasterVolume_(CPU *c) { client_t *cl = ((sub_t *)AP(0))->cl; float v = cl->volume; memcpy(AP(1), &v, 4); RET(S_OK_); }
static void SetMute_(CPU *c) { RET(S_OK_); }
static void GetMute_(CPU *c) { wr32(A(1), 0); RET(S_OK_); }

/* ---- the pump: one period every 10 ms to the sink, then the client's event ---- */
static void *pump_thread(void *arg)
{
    client_t *cl = arg;
    int have_sink = w64_audio_sink_open(RATE, CH) == 0;
    w64_log("[wasapi] output %s", have_sink ? "to the platform sink" : "discarded (no sink)");
    float *out = malloc(sizeof(float) * CH * cl->period_frames);
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 10000000;
        if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
        /* with a sink, keep ~40 ms queued there instead of following the clock blindly */
        if (have_sink && w64_audio_sink_queued() > (int)(cl->period_frames * 4)) {
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
            continue;
        }
        uint32_t n = cl->period_frames;
        pthread_mutex_lock(&cl->mu);
        uint32_t take = cl->started ? (cl->fill < n ? cl->fill : n) : 0;
        for (uint32_t i = 0; i < n; i++) {
            float l = 0, r = 0;
            if (i < take) {
                const uint8_t *f = cl->ring + (size_t)((cl->rd + i) % cl->buf_frames) * cl->frame_bytes;
                if (cl->fmt_float) { memcpy(&l, f, 4); if (cl->fmt_ch > 1) memcpy(&r, f + 4, 4); else r = l; }
                else { int16_t a, b; memcpy(&a, f, 2); if (cl->fmt_ch > 1) memcpy(&b, f + 2, 2); else b = a; l = a / 32768.0f; r = b / 32768.0f; }
            }
            out[2 * i] = l * cl->volume; out[2 * i + 1] = r * cl->volume;
        }
        cl->rd = (cl->rd + take) % cl->buf_frames;
        cl->fill -= take;
        if (cl->started) cl->played += n;
        uint64_t ev = cl->event;
        pthread_mutex_unlock(&cl->mu);
        if (have_sink && cl->started) w64_audio_sink_write(out, (int)n);
        if (ev && cl->started) w64_sem_release(ev);
        /* level meter for the log: peak and underruns over ~10 s (is FMOD producing sound?) */
        {
            static float peak; static unsigned blocks, starved;
            for (uint32_t i = 0; i < 2 * n; i++) { float a = out[i] < 0 ? -out[i] : out[i]; if (a > peak) peak = a; }
            if (cl->started && take < n) starved++;
            if (++blocks >= 1000) {
                w64_log("[wasapi] last 10 s: peak %.3f, %u of %u blocks short, volume %.2f", peak, starved, blocks, cl->volume);
                peak = 0; blocks = starved = 0;
            }
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

/* ---- QueryInterface ---- */
static void qi_for(CPU *c, obj_t *o, uint64_t riid, uint64_t ppv)
{
    int ok = geq(riid, &IID_IUnknown_);
    switch (o->kind & 0xFF) {
    case K_ENUM: ok |= geq(riid, &IID_IMMDeviceEnumerator); break;
    case K_DEVICE:
        ok |= geq(riid, &IID_IMMDevice);
        if (geq(riid, &IID_IMMEndpoint)) { wr64(ppv, (uint64_t)(uintptr_t)new_obj(K_ENDPOINT, sizeof(obj_t))); RET(S_OK_); }
        break;
    case K_ENDPOINT: ok |= geq(riid, &IID_IMMEndpoint); break;
    case K_CLIENT: ok |= geq(riid, &IID_IAudioClient) || geq(riid, &IID_IAudioClient2) || geq(riid, &IID_IAudioClient3); break;
    case K_RENDER: ok |= geq(riid, &IID_IAudioRenderClient); break;
    case K_CLOCK: ok |= geq(riid, &IID_IAudioClock); break;
    case K_VOLUME: ok |= geq(riid, &IID_ISimpleAudioVolume); break;
    }
    if (!ok) { w64_log("[wasapi] QueryInterface(kind %d, %s): no", o->kind & 0xFF, gstr(riid)); if (ppv) wr64(ppv, 0); RET(E_NOINTERFACE_); }
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
    wr64(ppv, (uint64_t)(uintptr_t)o);
    RET(S_OK_);
}

#define VT(kind, ...) do { static const char *n[] = { "QueryInterface", "AddRef", "Release", __VA_ARGS__ }; \
    g_vt[kind] = mkvtbl(n, f, (int)(sizeof n / sizeof *n)); } while (0)
static void build_vtables(void)
{
    { w64_fn f[] = { QI_, AddRef_, Release_, EnumAudioEndpoints_, GetDefaultAudioEndpoint_, GetDevice_, RegisterCb_, RegisterCb_ };
      VT(K_ENUM, "IMMDeviceEnumerator::EnumAudioEndpoints", "IMMDeviceEnumerator::GetDefaultAudioEndpoint", "IMMDeviceEnumerator::GetDevice",
         "IMMDeviceEnumerator::RegisterEndpointNotificationCallback", "IMMDeviceEnumerator::UnregisterEndpointNotificationCallback"); }
    { w64_fn f[] = { QI_, AddRef_, Release_, Activate_, OpenPropertyStore_, GetId_, GetState_ };
      VT(K_DEVICE, "IMMDevice::Activate", "IMMDevice::OpenPropertyStore", "IMMDevice::GetId", "IMMDevice::GetState"); }
    { w64_fn f[] = { QI_, AddRef_, Release_, CollGetCount_, CollItem_ };
      VT(K_COLL, "IMMDeviceCollection::GetCount", "IMMDeviceCollection::Item"); }
    { w64_fn f[] = { QI_, AddRef_, Release_, PropGetCount_, PropGetAt_, PropGetValue_, PropSetValue_, PropCommit_ };
      VT(K_PROPS, "IPropertyStore::GetCount", "IPropertyStore::GetAt", "IPropertyStore::GetValue", "IPropertyStore::SetValue", "IPropertyStore::Commit"); }
    { w64_fn f[] = { QI_, AddRef_, Release_, Initialize_, GetBufferSize_, GetStreamLatency_, GetCurrentPadding_, IsFormatSupported_,
                     GetMixFormat_, GetDevicePeriod_, Start_, Stop_, Reset_, SetEventHandle_, GetService_,
                     IsOffloadCapable_, SetClientProperties_, GetBufferSizeLimits_,
                     GetSharedModeEnginePeriod_, GetCurrentSharedModeEnginePeriod_, InitializeSharedAudioStream_ };
      VT(K_CLIENT, "IAudioClient::Initialize", "IAudioClient::GetBufferSize", "IAudioClient::GetStreamLatency", "IAudioClient::GetCurrentPadding",
         "IAudioClient::IsFormatSupported", "IAudioClient::GetMixFormat", "IAudioClient::GetDevicePeriod", "IAudioClient::Start",
         "IAudioClient::Stop", "IAudioClient::Reset", "IAudioClient::SetEventHandle", "IAudioClient::GetService",
         "IAudioClient2::IsOffloadCapable", "IAudioClient2::SetClientProperties", "IAudioClient2::GetBufferSizeLimits",
         "IAudioClient3::GetSharedModeEnginePeriod", "IAudioClient3::GetCurrentSharedModeEnginePeriod", "IAudioClient3::InitializeSharedAudioStream"); }
    { w64_fn f[] = { QI_, AddRef_, Release_, GetBuffer_, ReleaseBuffer_ };
      VT(K_RENDER, "IAudioRenderClient::GetBuffer", "IAudioRenderClient::ReleaseBuffer"); }
    { w64_fn f[] = { QI_, AddRef_, Release_, ClockGetFrequency_, ClockGetPosition_, ClockGetCharacteristics_ };
      VT(K_CLOCK, "IAudioClock::GetFrequency", "IAudioClock::GetPosition", "IAudioClock::GetCharacteristics"); }
    { w64_fn f[] = { QI_, AddRef_, Release_, GetDataFlow_ };
      VT(K_ENDPOINT, "IMMEndpoint::GetDataFlow"); }
    { w64_fn f[] = { QI_, AddRef_, Release_, SetMasterVolume_, GetMasterVolume_, SetMute_, GetMute_ };
      VT(K_VOLUME, "ISimpleAudioVolume::SetMasterVolume", "ISimpleAudioVolume::GetMasterVolume", "ISimpleAudioVolume::SetMute", "ISimpleAudioVolume::GetMute"); }
}

/* CoCreateInstance(clsid, outer, ctx, iid, void**) */
static void CoCreateInstance_(CPU *c)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, build_vtables);
    if (geq(A(0), &CLSID_MMDeviceEnumerator)) {
        obj_t *e = new_obj(K_ENUM, sizeof(obj_t));
        wr64(A(4), (uint64_t)(uintptr_t)e);
        w64_log("[wasapi] MMDeviceEnumerator created");
        RET(S_OK_);
    }
    w64_log("[com] CoCreateInstance(%s) refused", gstr(A(0)));
    if (A(4)) wr64(A(4), 0);
    RET(0x80040154u);                                   /* REGDB_E_CLASSNOTREG */
}

static const w64_export_t wasapi_exports[] = {
    { "ole32.dll", "CoCreateInstance", CoCreateInstance_ },
    { 0 }
};
/* the only CoCreateInstance: everything but the audio endpoint enumerator is refused */
__attribute__((constructor(103))) static void wasapi_init(void) { w64_register(wasapi_exports); }
