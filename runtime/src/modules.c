/*
 * modules.c - several lifted PE modules in one process.
 *
 * The Forest is TheForest.exe + fmod.dll + fmodstudio.dll + CSteamworks.dll + UnityImageNativePlugin.dll.
 * They all link at 0x180000000, so the build rebased each to its own base before lifting
 * (tools/rebase.py, ref/lift/layout.json) and the lifted C has those addresses baked in. Here the
 * player's original files are mapped at exactly those bases and relocated the same way, imports
 * between lifted modules are bound to the exporting module's code, everything else goes to host
 * functions (sentinels), and each module with a TLS directory gets its own TLS index.
 */
#include "w64.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

w64_module_t g_mods[W64_MAX_MODULES];
int g_nmods;
static pthread_mutex_t g_mod_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t u32(uint64_t a) { return rd32(a); }
static uint16_t u16(uint64_t a) { return rd16(a); }

static const char *base_name(const char *p)
{
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

/* "unityplayer", "UnityPlayer.dll", "C:\Game\UnityPlayer.dll" all name the same module */
static int name_eq(const char *a, const char *b)
{
    a = base_name(a); b = base_name(b);
    while (*a && *b && *a != '.' && *b != '.') {
        if ((*a | 0x20) != (*b | 0x20)) return 0;
        a++; b++;
    }
    return (!*a || *a == '.') && (!*b || *b == '.');
}

w64_module_t *w64_module_find(const char *name)
{
    for (int i = 0; i < g_nmods; i++) if (name_eq(g_mods[i].spec->name, name)) return &g_mods[i];
    return NULL;
}

w64_module_t *w64_module_by_va(uint64_t va)
{
    for (int i = 0; i < g_nmods; i++)
        if (va >= g_mods[i].base && va < g_mods[i].base + g_mods[i].size) return &g_mods[i];
    return NULL;
}

w64_module_t *w64_module_by_base(uint64_t base)
{
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].base == base) return &g_mods[i];
    return NULL;
}

/* ---------------------------------------------------------------- mapping */
static int map_module(w64_module_t *m, const char *gamedir)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", gamedir, m->spec->file);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { w64_log("[mod] cannot open %s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = malloc((size_t)st.st_size);
    if (!f || read(fd, f, (size_t)st.st_size) != st.st_size) { close(fd); free(f); return -1; }
    close(fd);
    uint32_t pe; memcpy(&pe, f + 0x3c, 4);
    uint16_t magic; memcpy(&magic, f + pe + 24, 2);
    if (memcmp(f + pe, "PE\0\0", 4) || magic != 0x20b) { w64_log("[mod] %s: not a PE32+ image", path); free(f); return -1; }
    uint16_t nsec, optsz; memcpy(&nsec, f + pe + 6, 2); memcpy(&optsz, f + pe + 20, 2);
    const uint8_t *opt = f + pe + 24;
    uint64_t linked; memcpy(&linked, opt + 24, 8);
    uint32_t soi, hdrs, entry; memcpy(&soi, opt + 56, 4); memcpy(&hdrs, opt + 60, 4); memcpy(&entry, opt + 16, 4);
    uint64_t base = m->spec->base;
    void *img = mmap((void *)(uintptr_t)base, soi, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (img == MAP_FAILED || (uint64_t)(uintptr_t)img != base) {
        w64_log("[mod] cannot map %s at %#llx: %s", m->spec->name, (unsigned long long)base, strerror(errno));
        free(f);
        return -1;
    }
    memcpy(img, f, hdrs);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vsz, va, rsz, raw;
        memcpy(&vsz, sh + 8, 4); memcpy(&va, sh + 12, 4); memcpy(&rsz, sh + 16, 4); memcpy(&raw, sh + 20, 4);
        uint32_t n = rsz < vsz || !vsz ? rsz : vsz;
        if (raw && n) memcpy((uint8_t *)img + va, f + raw, n);
    }
    free(f);
    uint64_t optva = base + pe + 24;
    /* relocate to the lift base, exactly as tools/rebase.py did to the lifted copy */
    int64_t delta = (int64_t)(base - linked);
    if (delta) {
        uint32_t rrva = u32(optva + 112 + 5 * 8), rsz = u32(optva + 112 + 5 * 8 + 4);
        if (!rrva) { w64_log("[mod] %s has no relocations", m->spec->name); return -1; }
        uint64_t p = base + rrva, end = p + rsz;
        uint64_t n64 = 0;
        while (p < end) {
            uint32_t page = u32(p), bsz = u32(p + 4);
            if (bsz < 8) break;
            for (uint32_t k = 8; k + 2 <= bsz; k += 2) {
                uint16_t e = u16(p + k);
                if ((e >> 12) == 10) { uint64_t a = base + page + (e & 0xFFF); wr64(a, rd64(a) + (uint64_t)delta); n64++; }
                else if ((e >> 12) == 3) { uint64_t a = base + page + (e & 0xFFF); wr32(a, rd32(a) + (uint32_t)delta); }
            }
            p += bsz;
        }
        wr64(optva + 24, base);
        w64_log("[mod] %s relocated %#llx -> %#llx (%llu fixups)", m->spec->name, (unsigned long long)linked,
                (unsigned long long)base, (unsigned long long)n64);
    }
    m->base = base;
    m->size = soi;
    m->entry = entry ? base + entry : 0;
    m->is_dll = (u16(base + pe + 22) & 0x2000) != 0;
    m->exp_rva = u32(optva + 112 + 0 * 8); m->exp_size = u32(optva + 112 + 0 * 8 + 4);
    m->pdata_rva = u32(optva + 112 + 3 * 8); m->pdata_size = u32(optva + 112 + 3 * 8 + 4);
    uint32_t tls_rva = u32(optva + 112 + 9 * 8);
    m->tls_index = -1;
    if (tls_rva) {
        uint64_t t = base + tls_rva;
        m->tls_start = rd64(t); m->tls_end = rd64(t + 8);
        m->tls_index_va = rd64(t + 16); m->tls_callbacks = rd64(t + 24); m->tls_zero = rd32(t + 32);
    }
    m->mapped = 1;
    return 0;
}

/* ---------------------------------------------------------------- exports */
uint64_t w64_module_export(w64_module_t *m, const char *name)
{
    if (!m || !m->exp_rva) return 0;
    uint64_t ed = m->base + m->exp_rva;
    uint32_t ordbase = u32(ed + 16), nfun = u32(ed + 20), nname = u32(ed + 24);
    uint64_t funcs = m->base + u32(ed + 28), names = m->base + u32(ed + 32), ords = m->base + u32(ed + 36);
    uint32_t idx = 0xFFFFFFFFu;
    if ((uintptr_t)name < 0x10000) idx = (uint32_t)(uintptr_t)name - ordbase;   /* by ordinal */
    else {
        uint32_t lo = 0, hi = nname;                                            /* names are sorted */
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int r = strcmp((const char *)(uintptr_t)(m->base + u32(names + 4 * mid)), name);
            if (!r) { idx = u16(ords + 2 * mid); break; }
            if (r < 0) lo = mid + 1; else hi = mid;
        }
    }
    if (idx >= nfun) return 0;
    uint32_t rva = u32(funcs + 4 * idx);
    if (!rva) return 0;
    if (rva >= m->exp_rva && rva < m->exp_rva + m->exp_size) {                  /* forwarder "dll.name" */
        const char *fw = (const char *)(uintptr_t)(m->base + rva);
        const char *dot = strchr(fw, '.');
        if (!dot) return 0;
        char dll[64]; snprintf(dll, sizeof dll, "%.*s", (int)(dot - fw), fw);
        w64_module_t *t = w64_module_find(dll);
        if (t) return w64_module_export(t, dot + 1);
        return w64_hostfn_named(dll, dot + 1);
    }
    return m->base + rva;
}

/* ---------------------------------------------------------------- init */
uint32_t w64_import_slot(const char *dll, const char *name);   /* core.c */

int w64_modules_init(const char *gamedir)
{
    for (int i = 0; i < w64_nmodspecs && i < W64_MAX_MODULES; i++) {
        w64_module_t *m = &g_mods[g_nmods];
        memset(m, 0, sizeof *m);
        m->spec = &w64_modspecs[i];
        if (map_module(m, gamedir) == 0) g_nmods++;
        else if (i == 0) return -1;              /* the executable itself is required */
    }
    /* TLS indices in load-table order */
    int next_tls = 0;
    for (int i = 0; i < g_nmods; i++) {
        w64_module_t *m = &g_mods[i];
        if (m->tls_index_va) { m->tls_index = next_tls++; wr32(m->tls_index_va, (uint32_t)m->tls_index); }
    }
    /* imports */
    int host = 0, inter = 0, missing = 0;
    for (int i = 0; i < g_nmods; i++) {
        w64_module_t *m = &g_mods[i];
        for (uint32_t k = 0; k < m->spec->nimps; k++) {
            const hle_import_t *im = &m->spec->imps[k];
            w64_module_t *t = w64_module_find(im->dll);
            uint64_t v = 0;
            if (t) {
                const char *nm = im->name;
                if (nm[0] == '#') nm = (const char *)(uintptr_t)strtoul(nm + 1, NULL, 10);
                v = w64_module_export(t, nm);
                if (v) inter++;
                else w64_log("[mod] %s: import %s!%s not exported", m->spec->name, im->dll, im->name);
            }
            if (!v) {
                uint32_t s = w64_import_slot(im->dll, im->name);
                v = HLE_SENTINEL(s);
                if (w64_hostfn_name(v) && !w64_find(im->dll, im->name)) missing++;
                host++;
            }
            wr64(im->slot, v);
        }
        w64_log("[mod] %-24s %#llx-%#llx entry %#llx tls %d imports %u",
                m->spec->name, (unsigned long long)m->base, (unsigned long long)(m->base + m->size),
                (unsigned long long)m->entry, m->tls_index, m->spec->nimps);
    }
    w64_log("[mod] %d modules, %d imports bound between modules, %d to host functions (%d not implemented)",
            g_nmods, inter, host, missing);
    return 0;
}

/* ---------------------------------------------------------------- TLS blocks */
/* ThreadLocalStoragePointer array for a new thread: one block per module with a TLS directory. */
uint64_t w64_tls_array_new(void)
{
    uint64_t *arr = calloc(W64_MAX_MODULES, sizeof *arr);
    for (int i = 0; i < g_nmods; i++) {
        w64_module_t *m = &g_mods[i];
        if (m->tls_index < 0) continue;
        size_t raw = (size_t)(m->tls_end - m->tls_start);
        uint8_t *blk = calloc(1, raw + m->tls_zero + 64);
        memcpy(blk, (void *)(uintptr_t)m->tls_start, raw);
        arr[m->tls_index] = (uint64_t)(uintptr_t)blk;
    }
    return (uint64_t)(uintptr_t)arr;
}

void w64_tls_array_free(uint64_t a)
{
    if (!a) return;
    uint64_t *arr = (uint64_t *)(uintptr_t)a;
    for (int i = 0; i < W64_MAX_MODULES; i++) free((void *)(uintptr_t)arr[i]);
    free(arr);
}

/* ---------------------------------------------------------------- attach (DllMain) */
static void tls_callbacks(CPU *c, w64_module_t *m, uint32_t reason)
{
    if (!m->tls_callbacks) return;
    for (uint64_t p = m->tls_callbacks; rd64(p); p += 8)
        w64_call(c, rd64(p), 3, m->base, (uint64_t)reason, 0ull);
}

/* Load-time initialisation of a module and the lifted modules it imports from (DLL_PROCESS_ATTACH).
 * Returns 0 on success. */
int w64_module_attach(w64_module_t *m)
{
    pthread_mutex_lock(&g_mod_mu);
    if (m->attached) { pthread_mutex_unlock(&g_mod_mu); return 0; }
    m->attached = 1;                             /* set first: import cycles terminate */
    m->load_order = 0;
    pthread_mutex_unlock(&g_mod_mu);
    for (uint32_t k = 0; k < m->spec->nimps; k++) {
        w64_module_t *t = w64_module_find(m->spec->imps[k].dll);
        if (t && t != m && !t->attached) w64_module_attach(t);
    }
    static int order;
    m->load_order = ++order;
    CPU *c = w64_cpu();
    tls_callbacks(c, m, 1);
    if (m->is_dll && m->entry) {
        w64_log("[mod] DllMain %s (PROCESS_ATTACH)", m->spec->name);
        uint64_t r = w64_call(c, m->entry, 3, m->base, 1ull, 0ull);
        if (!(uint32_t)r) { w64_log("[mod] DllMain %s failed", m->spec->name); return -1; }
    }
    return 0;
}

/* A new thread: DLL_THREAD_ATTACH for every attached module, in load order. */
void w64_modules_thread_attach(CPU *c)
{
    w64_module_t *ord[W64_MAX_MODULES]; int n = 0;
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].attached && g_mods[i].load_order) ord[n++] = &g_mods[i];
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && ord[j]->load_order < ord[j - 1]->load_order; j--) { w64_module_t *t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
    for (int i = 0; i < n; i++) {
        tls_callbacks(c, ord[i], 2);
        if (ord[i]->is_dll && ord[i]->entry && !ord[i]->no_thread_calls) w64_call(c, ord[i]->entry, 3, ord[i]->base, 2ull, 0ull);
    }
}

/* For mono_bridge.c P/Invoke resolution: look up an export in a lifted PE module (attaching it
 * if needed) or in the host HLE export registry. Returns guest VA or HLE sentinel VA, or 0. */
uint64_t w64_lookup_export_va(const char *dll, const char *name)
{
    if (!dll || !*dll || !name || !*name) return 0;
    w64_module_t *m = w64_module_find(dll);
    if (m) {
        if (!m->attached) w64_module_attach(m);
        uint64_t va = w64_module_export(m, name);
        if (va) return va;
    }
    return w64_hostfn_named(dll, name);
}

/* For eh64.c: the module holding va, and its .pdata. */
int w64_eh_module(uint64_t va, uint64_t *base, uint32_t *pdata_rva, uint32_t *pdata_n)
{
    w64_module_t *m = w64_module_by_va(va);
    if (!m) return 0;
    *base = m->base;
    *pdata_rva = m->pdata_rva;
    *pdata_n = m->pdata_size / 12;
    return 1;
}
