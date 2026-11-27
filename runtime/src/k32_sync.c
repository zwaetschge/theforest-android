/*
 * k32_sync.c - kernel32: heaps, virtual memory, critical sections, SRW locks,
 * condition variables, init-once, events/mutexes/semaphores, waits, threads,
 * TLS/FLS, SLists, thread pool work, time.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TRUE_ 1
#define WAIT_OBJECT_0   0u
#define WAIT_TIMEOUT    0x102u
#define WAIT_FAILED     0xFFFFFFFFu
#define INFINITE_       0xFFFFFFFFu
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_ALREADY_EXISTS 183

uint64_t w64_thread_id(void);

/* ------------------------------------------------------------------ heap */
/* 16-byte header holding the size, so HeapSize/HeapReAlloc are exact and the
 * returned block stays 16-aligned like the Win64 heap. */
#define HHDR 16
/* Tail slack: guest code reads a few bytes past its buffers (bit readers fetching whole words).
 * Harmless on the Windows heap; Android's allocator puts guard pages behind large blocks. */
#define HTAIL(n) ((n) >= 1024 ? 64 : 0)
/* ---- big blocks in a memfd arena (W64_LOWMEM; default on Android) ----
 * Samsung kills a foreground app above ~4 GB of *anonymous* memory; shared memory pages are
 * accounted separately, are still swappable, and never touch the flash like a file would.
 * Page runs of 64 KB granules, first fit, coalescing free list; freed runs are punched out so
 * the pages go back to the system at once. */
#include <sys/syscall.h>
#include <linux/falloc.h>
#include <linux/futex.h>
#include <fcntl.h>
#define BIG_MIN   (64u << 10)
#define GRAN      (64u << 10)
#define ARENA     (48ull << 30)
static uint8_t *g_arena;
static size_t g_arena_off;
static int g_arena_fd = -1, g_arena_state;      /* 0 = untried, 1 = on, -1 = off */
static int g_va_memfd_state;                    /* 0 = untried, 1 = on, -1 = off */
static struct run { uint64_t start, len; } *g_free;
static int g_nfree, g_capfree;
static pthread_mutex_t g_arena_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_arena_live;

static int arena_on(void)
{
    if (g_arena_state) return g_arena_state > 0;
    pthread_mutex_lock(&g_arena_mu);
    if (!g_arena_state) {
        const char *e = getenv("W64_LOWMEM");
#ifdef __ANDROID__
        int want = !e || atoi(e) != 0;
#else
        int want = e && atoi(e) != 0;
#endif
        g_arena_state = -1;
        if (want) {
            int fd = (int)syscall(__NR_memfd_create, "w64heap", 0u);
            if (fd >= 0 && ftruncate(fd, (off_t)(ARENA + GRAN)) == 0) {
                uint8_t *q = mmap(NULL, ARENA + GRAN, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd, 0);
                if (q != MAP_FAILED) {
                    /* Align base to 64 KB (GRAN) so VirtualAlloc regions in the arena keep Win64 granularity. */
                    uint8_t *a = (uint8_t *)(((uintptr_t)q + GRAN - 1) & ~(uintptr_t)(GRAN - 1));
                    size_t off = (size_t)(a - q);
                    if (off) munmap(q, off);
                    if (GRAN - off) munmap(a + ARENA, GRAN - off);
                    g_arena = a; g_arena_off = off; g_arena_fd = fd; g_arena_state = 1;
                    g_capfree = 1024; g_free = malloc(sizeof *g_free * (size_t)g_capfree);
                    g_free[0].start = 0; g_free[0].len = ARENA / GRAN; g_nfree = 1;
                    w64_log("[lowmem] big allocations (>= %u KB) in a %llu GB memfd arena", BIG_MIN >> 10, (unsigned long long)(ARENA >> 30));
                }
            }
            if (g_arena_state < 0) w64_log("[lowmem] memfd arena unavailable (%s)", strerror(errno));
        }
    }
    pthread_mutex_unlock(&g_arena_mu);
    return g_arena_state > 0;
}
static int va_memfd_on(void)
{
    if (g_va_memfd_state) return g_va_memfd_state > 0;
    const char *e = getenv("W64_LOWMEM_VA");
    uint64_t ram = (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE);
#ifdef __ANDROID__
    int want = e ? (atoi(e) != 0) : (ram > 0 && ram <= (9ull << 30));
#else
    int want = e && atoi(e) != 0;
#endif
    if (want && arena_on()) {
        g_va_memfd_state = 1;
        w64_log("[lowmem] VirtualAlloc (>= %u KB) backed by memfd arena (RAM %llu MB)", GRAN >> 10, (unsigned long long)(ram >> 20));
    } else {
        g_va_memfd_state = -1;
    }
    return g_va_memfd_state > 0;
}
static int in_arena(const void *p) { return g_arena && (const uint8_t *)p >= g_arena && (const uint8_t *)p < g_arena + ARENA; }
static void *arena_alloc(size_t bytes)
{
    uint64_t need = (bytes + GRAN - 1) / GRAN;
    pthread_mutex_lock(&g_arena_mu);
    for (int i = 0; i < g_nfree; i++)
        if (g_free[i].len >= need) {
            uint64_t st = g_free[i].start;
            g_free[i].start += need; g_free[i].len -= need;
            if (!g_free[i].len) { memmove(&g_free[i], &g_free[i + 1], sizeof *g_free * (size_t)(g_nfree - i - 1)); g_nfree--; }
            g_arena_live += need * GRAN;
            pthread_mutex_unlock(&g_arena_mu);
            return g_arena + st * GRAN;
        }
    pthread_mutex_unlock(&g_arena_mu);
    return NULL;
}
static void arena_free(void *p, size_t bytes)
{
    uint64_t st = (uint64_t)((uint8_t *)p - g_arena) / GRAN, len = (bytes + GRAN - 1) / GRAN;
    fallocate(g_arena_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)(g_arena_off + st * GRAN), (off_t)(len * GRAN));
    pthread_mutex_lock(&g_arena_mu);
    g_arena_live -= len * GRAN;
    int i = 0;
    while (i < g_nfree && g_free[i].start < st) i++;
    if (g_nfree + 1 > g_capfree) { g_capfree *= 2; g_free = realloc(g_free, sizeof *g_free * (size_t)g_capfree); }
    memmove(&g_free[i + 1], &g_free[i], sizeof *g_free * (size_t)(g_nfree - i));
    g_free[i].start = st; g_free[i].len = len; g_nfree++;
    if (i + 1 < g_nfree && g_free[i].start + g_free[i].len == g_free[i + 1].start) {
        g_free[i].len += g_free[i + 1].len;
        memmove(&g_free[i + 1], &g_free[i + 2], sizeof *g_free * (size_t)(g_nfree - i - 2)); g_nfree--;
    }
    if (i > 0 && g_free[i - 1].start + g_free[i - 1].len == g_free[i].start) {
        g_free[i - 1].len += g_free[i].len;
        memmove(&g_free[i], &g_free[i + 1], sizeof *g_free * (size_t)(g_nfree - i - 1)); g_nfree--;
    }
    pthread_mutex_unlock(&g_arena_mu);
}
uint64_t w64_arena_live(void) { return g_arena_live; }

static void *heap_alloc(size_t n, int zero)
{
    if (n >= BIG_MIN && arena_on()) {
        uint8_t *a = arena_alloc(n + HHDR + HTAIL(n));       /* fresh/punched pages read as zero */
        if (a) { memcpy(a, &n, sizeof n); memset(a + 8, 0, 8); return a + HHDR; }
    }
    uint8_t *p = zero ? calloc(1, n + HHDR + HTAIL(n)) : malloc(n + HHDR + HTAIL(n));
    if (!p) return NULL;
    memcpy(p, &n, sizeof n);
    memset(p + 8, 0, 8);
    return p + HHDR;
}
static size_t heap_size(void *q) { size_t n; memcpy(&n, (uint8_t *)q - HHDR, sizeof n); return n; }
/* live accounting per guest caller (W64_ALLOCLOG): callers in the header's spare 8 bytes */
#define LIVE_N 8192
static struct { uint64_t ra; int64_t bytes, count; } g_live[LIVE_N];
static pthread_mutex_t g_live_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_live_on = -1;
static void live_add(uint64_t ra, int64_t n)
{
    pthread_mutex_lock(&g_live_mu);
    uint32_t h = (uint32_t)((ra * 0x9E3779B97F4A7C15ull) >> 51) % LIVE_N;
    for (int k = 0; k < LIVE_N; k++, h = (h + 1) % LIVE_N)
        if (g_live[h].ra == ra || !g_live[h].ra) { g_live[h].ra = ra; g_live[h].bytes += n; g_live[h].count += n > 0 ? 1 : -1; break; }
    pthread_mutex_unlock(&g_live_mu);
}
void w64_live_dump(void)
{
    int64_t tot = 0;
    int idx[LIVE_N], m = 0;
    pthread_mutex_lock(&g_live_mu);
    for (int i = 0; i < LIVE_N; i++) if (g_live[i].ra) { idx[m++] = i; tot += g_live[i].bytes; }
    for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (g_live[idx[j]].bytes > g_live[idx[i]].bytes) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    extern uint64_t w64_vmem_live;
    w64_log("[live] heap total %lld MB in %d callers; VirtualAlloc live %llu MB; memfd arena %llu MB", (long long)(tot >> 20), m,
            (unsigned long long)(w64_vmem_live >> 20), (unsigned long long)(g_arena_live >> 20));
    for (int i = 0; i < m && i < 15; i++)
        w64_log("[live] %6lld MB %8lld blocks  caller %#llx", (long long)(g_live[idx[i]].bytes >> 20), (long long)g_live[idx[i]].count, (unsigned long long)g_live[idx[i]].ra);
    pthread_mutex_unlock(&g_live_mu);
}
static void heap_free(void *q)
{
    if (!q) return;
    if (g_live_on > 0) { uint64_t ra; memcpy(&ra, (uint8_t *)q - 8, 8); if (ra) live_add(ra, -(int64_t)heap_size(q)); }
    if (in_arena(q)) { size_t n = heap_size(q); arena_free((uint8_t *)q - HHDR, n + HHDR + HTAIL(n)); return; }
    free((uint8_t *)q - HHDR);
}
static void *heap_realloc(void *q, size_t n, int zero)
{
    if (!q) return heap_alloc(n, zero);
    size_t old = heap_size(q);
    if (in_arena(q) || (n >= BIG_MIN && arena_on())) {
        uint8_t *nq = heap_alloc(n, zero);
        if (!nq) return NULL;
        memcpy(nq - 8, (uint8_t *)q - 8, 8);                    /* keep the caller tag */
        memcpy(nq, q, old < n ? old : n);
        if (zero && n > old) memset(nq + old, 0, n - old);
        uint64_t ra = 0;
        memcpy((uint8_t *)q - 8, &ra, 8);                       /* moved: no live accounting on the old block */
        heap_free(q);
        return nq;
    }
    uint8_t *p = realloc((uint8_t *)q - HHDR, n + HHDR + HTAIL(n));
    if (!p) return NULL;
    memcpy(p, &n, sizeof n);
    if (zero && n > old) memset(p + HHDR + old, 0, n - old);
    return p + HHDR;
}
void *w64_heap_alloc(size_t n) { return heap_alloc(n, 0); }
void  w64_heap_free(void *p) { heap_free(p); }

#define PROCESS_HEAP 0x0000000000150000ull
static void GetProcessHeap_(CPU *c) { RET(PROCESS_HEAP); }
static void HeapCreate_(CPU *c) { RET(w64_handle_new(H_HEAP, NULL)); }
static void HeapDestroy_(CPU *c) { w64_handle_close(A(0)); RET(1); }
static size_t g_alloclog = (size_t)-1;
static uint64_t g_big_total;
static void alloclog(CPU *c, const char *fn, size_t n)
{
    if (g_alloclog == (size_t)-1) g_alloclog = getenv("W64_ALLOCLOG") ? (size_t)atoll(getenv("W64_ALLOCLOG")) : 0;
    if (!g_alloclog || n < g_alloclog) return;
    g_big_total += n;
    /* guest return address chain: [rsp] is the caller of HeapAlloc (usually malloc), walk a little */
    uint64_t ra[4] = { 0 };
    int k = 0;
    uint64_t top = w64_teb ? rd64((uint64_t)(uintptr_t)w64_teb + TEB_STACK_BASE) : 0;
    for (uint64_t p = c->rsp; k < 4 && p < c->rsp + 0x2000 && p + 8 <= top; p += 8) {
        uint64_t v = rd64(p);
        w64_module_t *m = w64_module_by_va(v);
        if (m && v > m->base + 0x1000) ra[k++] = v;
    }
    w64_log("[alloc] %s %zu MB (total big %llu MB) from %llx %llx %llx %llx", fn, n >> 20, (unsigned long long)(g_big_total >> 20),
            (unsigned long long)ra[0], (unsigned long long)ra[1], (unsigned long long)ra[2], (unsigned long long)ra[3]);
}
/* the guest code that asked: first image return address above the CRT allocator frames */
static uint64_t stack_top(void) { return w64_teb ? rd64((uint64_t)(uintptr_t)w64_teb + TEB_STACK_BASE) : 0; }
static uint64_t guest_caller(CPU *c)
{
    int k = 0;
    uint64_t top = stack_top();
    for (uint64_t p = c->rsp; p < c->rsp + 0x300 && p + 8 <= top; p += 8) {
        uint64_t v = rd64(p);
        w64_module_t *m = w64_module_by_va(v);
        if (m && v > m->base + 0x1000) return v;
        if (++k > 96) break;
    }
    return rd64(c->rsp);
}
static void HeapAlloc_(CPU *c)
{
    alloclog(c, "HeapAlloc", (size_t)A(2));
    if (g_live_on < 0) g_live_on = getenv("W64_ALLOCLOG") != NULL;
    void *p = heap_alloc((size_t)A(2), (AU(1) & 8) != 0);
    if (p && g_live_on > 0) { uint64_t ra = guest_caller(c); memcpy((uint8_t *)p - 8, &ra, 8); live_add(ra, (int64_t)A(2)); }
    if (!p) w64_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    RET((uintptr_t)p);
}
static void HeapFree_(CPU *c) { heap_free(AP(2)); RET(1); }
static void HeapReAlloc_(CPU *c)
{
    uint32_t fl = AU(1);
    void *q = AP(2);
    size_t n = (size_t)A(3);
    if ((fl & 0x10) && q && n > heap_size(q)) RET(0);     /* HEAP_REALLOC_IN_PLACE_ONLY */
    if (g_live_on > 0 && q) { uint64_t ra; memcpy(&ra, (uint8_t *)q - 8, 8); if (ra) live_add(ra, (int64_t)n - (int64_t)heap_size(q)); }
    RET((uintptr_t)heap_realloc(q, n, (fl & 8) != 0));
}
static void HeapSize_(CPU *c) { void *q = AP(2); RET(q ? heap_size(q) : (uint64_t)-1); }
static void HeapValidate_(CPU *c) { RET(1); }
static void HeapCompact_(CPU *c) { RET(1 << 20); }

static void *g_raw_local[128];
static pthread_mutex_t g_raw_mu = PTHREAD_MUTEX_INITIALIZER;
void *w64_local_alloc_raw(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) return NULL;
    pthread_mutex_lock(&g_raw_mu);
    for (int i = 0; i < 128; i++) if (!g_raw_local[i]) { g_raw_local[i] = p; break; }
    pthread_mutex_unlock(&g_raw_mu);
    return p;
}
static int free_if_raw_local(void *p)
{
    if (!p) return 1;
    pthread_mutex_lock(&g_raw_mu);
    for (int i = 0; i < 128; i++) {
        if (g_raw_local[i] == p) { g_raw_local[i] = NULL; pthread_mutex_unlock(&g_raw_mu); free(p); return 1; }
    }
    pthread_mutex_unlock(&g_raw_mu);
    return 0;
}
static void GlobalAlloc_(CPU *c) { RET((uintptr_t)heap_alloc((size_t)A(1), (AU(0) & 0x40) != 0)); }
static void GlobalFree_(CPU *c) { void *p = AP(0); if (!free_if_raw_local(p)) heap_free(p); RET(0); }
static void GlobalLock_(CPU *c) { RET(A(0)); }
static void GlobalUnlock_(CPU *c) { RET(1); }
static void GlobalSize_(CPU *c) { RET(A(0) ? heap_size(AP(0)) : 0); }
static void LocalFree_(CPU *c) { void *p = AP(0); if (!free_if_raw_local(p)) heap_free(p); RET(0); }
void *w64_local_alloc(size_t n) { return heap_alloc(n, 1); }

/* ------------------------------------------------------------------ virtual memory */
typedef struct vreg { uint64_t base, size; struct vreg *next; } vreg_t;
uint64_t w64_vmem_live;
static vreg_t *g_vregs;
static pthread_mutex_t g_vmu = PTHREAD_MUTEX_INITIALIZER;

static void VirtualAlloc_(CPU *c)
{
    uint64_t addr = A(0), size = (A(1) + 0xFFF) & ~0xFFFull;
    uint32_t type = AU(2);
    if (addr) {
        /* commit inside an existing reservation: already readable/writable */
        pthread_mutex_lock(&g_vmu);
        for (vreg_t *r = g_vregs; r; r = r->next)
            if (addr >= r->base && addr < r->base + r->size) { pthread_mutex_unlock(&g_vmu); RET(addr & ~0xFFFull); }
        pthread_mutex_unlock(&g_vmu);
    }
    if (!(type & 0x3000)) RET(0);
    alloclog(c, "VirtualAlloc", (size_t)size);
    void *p;
    if (addr) {
        p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    } else if (size >= GRAN && va_memfd_on()) {
        p = arena_alloc((size_t)size);
        if (!p) p = MAP_FAILED;
    } else {
        /* Windows hands out regions on the 64 KB allocation granularity; Unity's allocators find
         * their block headers by masking pointers, so a 4 KB-aligned mmap makes regions overlap. */
        const uint64_t G = 0x10000;
        uint8_t *q = mmap(NULL, size + G, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        p = q;
        if (q != MAP_FAILED) {
            uint8_t *a = (uint8_t *)(((uintptr_t)q + G - 1) & ~(uintptr_t)(G - 1));
            if (a > q) munmap(q, (size_t)(a - q));
            if (a + size < q + size + G) munmap(a + size, (size_t)(q + size + G - (a + size)));
            p = a;
        }
    }
    if (p == MAP_FAILED) { w64_set_last_error(ERROR_NOT_ENOUGH_MEMORY); RET(0); }
    if (g_alloclog && g_alloclog != (size_t)-1 && size >= g_alloclog)
        w64_log("[alloc]   -> %p..%p type %#x", p, (void *)((uint8_t *)p + size), type);
    vreg_t *r = malloc(sizeof *r);
    r->base = (uint64_t)(uintptr_t)p; r->size = size;
    w64_vmem_live += size;
    pthread_mutex_lock(&g_vmu);
    r->next = g_vregs; g_vregs = r;
    pthread_mutex_unlock(&g_vmu);
    RET((uintptr_t)p);
}

static void VirtualFree_(CPU *c)
{
    uint64_t addr = A(0);
    uint32_t type = AU(2);
    if (!(type & 0x8000)) {                 /* MEM_DECOMMIT: zero the pages */
        uint64_t a0 = addr & ~0xFFFull;
        uint64_t sz = (A(1) + (addr - a0) + 0xFFF) & ~0xFFFull;
        if (sz) {
            if (in_arena((void *)(uintptr_t)a0)) {
                uint64_t off = g_arena_off + (a0 - (uint64_t)(uintptr_t)g_arena);
                fallocate(g_arena_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)sz);
            } else {
                madvise((void *)(uintptr_t)a0, sz, MADV_DONTNEED);
            }
        }
        RET(1);
    }
    pthread_mutex_lock(&g_vmu);
    for (vreg_t **pp = &g_vregs; *pp; pp = &(*pp)->next)
        if ((*pp)->base == addr) {
            vreg_t *r = *pp; *pp = r->next;
            pthread_mutex_unlock(&g_vmu);
            if (in_arena((void *)(uintptr_t)r->base)) arena_free((void *)(uintptr_t)r->base, r->size);
            else munmap((void *)(uintptr_t)r->base, r->size);
            w64_vmem_live -= r->size;
            free(r);
            RET(1);
        }
    pthread_mutex_unlock(&g_vmu);
    RET(0);
}

/* All guest memory stays readable and writable (lifted code never executes guest pages), so a
 * protection change only has to report the previous protection. */
static void VirtualProtect_(CPU *c) { if (A(3)) wr32(A(3), 0x04 /* PAGE_READWRITE */); RET(1); }
/* VirtualQuery: fast path checks loaded modules and VirtualAlloc regions; fallback parses
 * /proc/self/maps (stack-bottom searches by Boehm GC need gaps to show up as MEM_FREE). */
static const char *parse_hex(const char *p, uint64_t *out)
{
    uint64_t v = 0;
    int any = 0;
    for (; *p; p++) {
        unsigned d;
        if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
        else if (*p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A' + 10);
        else break;
        v = (v << 4) | d;
        any = 1;
    }
    if (!any) return NULL;
    *out = v;
    return p;
}
static int maps_find(uint64_t a, uint64_t *lo, uint64_t *hi, int *rw)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    uint64_t prev_hi = 0, l, h;
    int r = 0;
    while (fgets(line, sizeof line, f)) {
        const char *p = parse_hex(line, &l);
        if (!p || *p != '-') continue;
        p = parse_hex(p + 1, &h);
        if (!p || *p != ' ') continue;
        p++;
        if (a < l) { *lo = prev_hi; *hi = l; r = 0; goto done; }     /* in the gap before this mapping */
        if (a < h) { *lo = l; *hi = h; *rw = (p[0] == 'r') | ((p[1] == 'w') << 1); r = 1; goto done; }
        prev_hi = h;
    }
    *lo = prev_hi; *hi = 0x800000000000ull; r = 0;
done:
    fclose(f);
    return r;
}
static void VirtualQuery_(CPU *c)                       /* MEMORY_BASIC_INFORMATION (48 bytes) */
{
    uint64_t a = A(0) & ~0xFFFull;
    uint8_t *mbi = AP(1);
    if (AU(2) < 48) RET(0);
    memset(mbi, 0, 48);
    uint64_t lo = 0, hi = 0, size, base;
    uint32_t state, prot, type;
    w64_module_t *m = w64_module_by_va(a);
    if (m) {
        state = 0x1000; prot = 0x04; type = 0x1000000;
        base = m->base; size = (m->base + m->size) - a;
    } else {
        int hit = 0;
        pthread_mutex_lock(&g_vmu);
        for (vreg_t *r = g_vregs; r; r = r->next)
            if (a >= r->base && a < r->base + r->size) { lo = r->base; hi = r->base + r->size; hit = 1; break; }
        pthread_mutex_unlock(&g_vmu);
        if (hit) {
            state = 0x1000; prot = 0x04; type = 0x20000;
            base = lo; size = hi - a;
        } else {
            int rw = 3, found = maps_find(a, &lo, &hi, &rw);
            if (found == 1) {
                state = 0x1000;                                   /* MEM_COMMIT */
                prot = (rw & 2) ? 0x04 : (rw & 1) ? 0x02 : 0x01;  /* READWRITE / READONLY / NOACCESS */
                type = 0x20000;                                   /* MEM_PRIVATE */
                base = lo;
                size = hi - a;
            } else {
                state = 0x10000; prot = 0x01; type = 0;           /* MEM_FREE */
                base = 0;
                size = hi > a ? hi - a : 0x1000;
            }
        }
    }
    memcpy(mbi, &a, 8); memcpy(mbi + 8, &base, 8); memcpy(mbi + 16, &prot, 4);
    memcpy(mbi + 24, &size, 8); memcpy(mbi + 32, &state, 4); memcpy(mbi + 36, &prot, 4); memcpy(mbi + 40, &type, 4);
    RET(48);
}

/* ------------------------------------------------------------------ critical sections */
/* CRITICAL_SECTION (40 bytes): DebugInfo slot holds the host mutex,
 * RecursionCount (+0x8 LockCount is left alone, +0xC RecursionCount),
 * OwningThread (+0x10) mirrored for code that peeks. */
static pthread_mutex_t g_cs_init_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_mutex(uint64_t cs)
{
    pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)rd64(cs);
    if (m && (uint64_t)(uintptr_t)m != ~0ull) return m;
    pthread_mutex_lock(&g_cs_init_mu);
    m = (pthread_mutex_t *)(uintptr_t)rd64(cs);
    if (!m || (uint64_t)(uintptr_t)m == ~0ull) {
        m = malloc(sizeof *m);
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &a);
        wr64(cs, (uint64_t)(uintptr_t)m);
    }
    pthread_mutex_unlock(&g_cs_init_mu);
    return m;
}
static void InitializeCriticalSection_(CPU *c) { memset(AP(0), 0, 40); cs_mutex(A(0)); RETV(); }
static void InitializeCriticalSectionAndSpinCount_(CPU *c) { memset(AP(0), 0, 40); cs_mutex(A(0)); RET(1); }
static void InitializeCriticalSectionEx_(CPU *c) { memset(AP(0), 0, 40); cs_mutex(A(0)); RET(1); }
static void EnterCriticalSection_(CPU *c)
{
    uint64_t cs = A(0);
    pthread_mutex_lock(cs_mutex(cs));
    wr32(cs + 0xC, rd32(cs + 0xC) + 1);
    wr64(cs + 0x10, w64_thread_id());
    RETV();
}
static void TryEnterCriticalSection_(CPU *c)
{
    uint64_t cs = A(0);
    if (pthread_mutex_trylock(cs_mutex(cs))) RET(0);
    wr32(cs + 0xC, rd32(cs + 0xC) + 1);
    wr64(cs + 0x10, w64_thread_id());
    RET(1);
}
static void LeaveCriticalSection_(CPU *c)
{
    uint64_t cs = A(0);
    uint32_t r = rd32(cs + 0xC);
    if (r) wr32(cs + 0xC, r - 1);
    if (r <= 1) wr64(cs + 0x10, 0);
    pthread_mutex_unlock(cs_mutex(cs));
    RETV();
}
static void DeleteCriticalSection_(CPU *c)
{
    pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)rd64(A(0));
    if (m && (uint64_t)(uintptr_t)m != ~0ull) { pthread_mutex_destroy(m); free(m); }
    wr64(A(0), 0);
    RETV();
}

/* ------------------------------------------------------------------ SRW locks + condition variables */
typedef struct { pthread_mutex_t m; pthread_cond_t cv; int readers, writer, wwait; } srw_t;
typedef struct { pthread_mutex_t m; pthread_cond_t cv; uint64_t seq; } cond_t;
static pthread_mutex_t g_lazy_mu = PTHREAD_MUTEX_INITIALIZER;

static srw_t *srw_of(uint64_t p)
{
    srw_t *s = (srw_t *)(uintptr_t)__atomic_load_n((uint64_t *)(uintptr_t)p, __ATOMIC_ACQUIRE);
    if (s) return s;
    pthread_mutex_lock(&g_lazy_mu);
    s = (srw_t *)(uintptr_t)rd64(p);
    if (!s) {
        s = calloc(1, sizeof *s);
        pthread_mutex_init(&s->m, NULL);
        pthread_cond_init(&s->cv, NULL);
        __atomic_store_n((uint64_t *)(uintptr_t)p, (uint64_t)(uintptr_t)s, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_lazy_mu);
    return s;
}
static cond_t *cond_of(uint64_t p)
{
    cond_t *s = (cond_t *)(uintptr_t)__atomic_load_n((uint64_t *)(uintptr_t)p, __ATOMIC_ACQUIRE);
    if (s) return s;
    pthread_mutex_lock(&g_lazy_mu);
    s = (cond_t *)(uintptr_t)rd64(p);
    if (!s) {
        s = calloc(1, sizeof *s);
        pthread_mutex_init(&s->m, NULL);
        pthread_cond_init(&s->cv, NULL);
        __atomic_store_n((uint64_t *)(uintptr_t)p, (uint64_t)(uintptr_t)s, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_lazy_mu);
    return s;
}
static void srw_lock_x(srw_t *s)
{
    pthread_mutex_lock(&s->m);
    s->wwait++;
    while (s->writer || s->readers) pthread_cond_wait(&s->cv, &s->m);
    s->wwait--;
    s->writer = 1;
    pthread_mutex_unlock(&s->m);
}
static void srw_lock_s(srw_t *s)
{
    pthread_mutex_lock(&s->m);
    while (s->writer || s->wwait) pthread_cond_wait(&s->cv, &s->m);
    s->readers++;
    pthread_mutex_unlock(&s->m);
}
static void srw_unlock_x(srw_t *s)
{
    pthread_mutex_lock(&s->m);
    s->writer = 0;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
}
static void srw_unlock_s(srw_t *s)
{
    pthread_mutex_lock(&s->m);
    if (s->readers > 0) s->readers--;
    if (!s->readers) pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
}
static void AcquireSRWLockExclusive_(CPU *c) { srw_lock_x(srw_of(A(0))); RETV(); }
static void AcquireSRWLockShared_(CPU *c) { srw_lock_s(srw_of(A(0))); RETV(); }
static void ReleaseSRWLockExclusive_(CPU *c) { srw_unlock_x(srw_of(A(0))); RETV(); }
static void ReleaseSRWLockShared_(CPU *c) { srw_unlock_s(srw_of(A(0))); RETV(); }
static void TryAcquireSRWLockExclusive_(CPU *c)
{
    srw_t *s = srw_of(A(0));
    pthread_mutex_lock(&s->m);
    int ok = !s->writer && !s->readers;
    if (ok) s->writer = 1;
    pthread_mutex_unlock(&s->m);
    RET(ok);
}

static int timed_wait(pthread_cond_t *cv, pthread_mutex_t *m, uint32_t ms)
{
    if (ms == INFINITE_) return pthread_cond_wait(cv, m);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cv, m, &ts);
}

static void SleepConditionVariableSRW_(CPU *c)
{
    cond_t *cv = cond_of(A(0));
    srw_t *s = srw_of(A(1));
    uint32_t ms = AU(2), shared = AU(3) & 1;
    pthread_mutex_lock(&cv->m);
    uint64_t seq = cv->seq;
    if (shared) srw_unlock_s(s); else srw_unlock_x(s);
    int to = 0;
    while (cv->seq == seq && !to) to = timed_wait(&cv->cv, &cv->m, ms) == ETIMEDOUT;
    pthread_mutex_unlock(&cv->m);
    if (shared) srw_lock_s(s); else srw_lock_x(s);
    if (to) { w64_set_last_error(1460 /* ERROR_TIMEOUT */); RET(0); }
    RET(1);
}
static void WakeConditionVariable_(CPU *c)
{
    cond_t *cv = cond_of(A(0));
    pthread_mutex_lock(&cv->m); cv->seq++; pthread_cond_signal(&cv->cv); pthread_mutex_unlock(&cv->m);
    RETV();
}
static void WakeAllConditionVariable_(CPU *c)
{
    cond_t *cv = cond_of(A(0));
    pthread_mutex_lock(&cv->m); cv->seq++; pthread_cond_broadcast(&cv->cv); pthread_mutex_unlock(&cv->m);
    RETV();
}

/* CONDITION_VARIABLE with a CRITICAL_SECTION: the section may be held recursively - release it
 * completely while waiting and restore the count. */
static void SleepConditionVariableCS_(CPU *c)
{
    cond_t *cv = cond_of(A(0));
    uint64_t cs = A(1);
    uint32_t ms = AU(2);
    pthread_mutex_t *m = cs_mutex(cs);
    uint32_t rec = rd32(cs + 0xC); uint64_t owner = rd64(cs + 0x10);
    pthread_mutex_lock(&cv->m);
    uint64_t seq = cv->seq;
    wr32(cs + 0xC, 0); wr64(cs + 0x10, 0);
    for (uint32_t i = 0; i < (rec ? rec : 1); i++) pthread_mutex_unlock(m);
    int to = 0;
    while (cv->seq == seq && !to) to = timed_wait(&cv->cv, &cv->m, ms) == ETIMEDOUT;
    pthread_mutex_unlock(&cv->m);
    for (uint32_t i = 0; i < (rec ? rec : 1); i++) pthread_mutex_lock(m);
    wr32(cs + 0xC, rec ? rec : 1); wr64(cs + 0x10, owner);
    if (to) { w64_set_last_error(1460 /* ERROR_TIMEOUT */); RET(0); }
    RET(1);
}
static void InitializeConditionVariable_(CPU *c) { wr64(A(0), 0); RETV(); }
static void InitializeSRWLock_(CPU *c) { wr64(A(0), 0); RETV(); }

/* INIT_ONCE: 0 = fresh, 1 = running, 2 = done (low bits; context pointers are not stored) */
static void InitOnceExecuteOnce_(CPU *c)                 /* (initonce, fn, param, *context) */
{
    uint64_t *p = (uint64_t *)AP(0);
    for (;;) {
        uint64_t v = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        if (v == 2) { if (A(3)) wr64(A(3), 0); RET(1); }
        uint64_t z = 0;
        if (v == 0 && __atomic_compare_exchange_n(p, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            uint64_t fn = A(1), io = A(0), prm = A(2), ctx = A(3);
            uint32_t ok = (uint32_t)w64_call(c, fn, 3, io, prm, ctx);
            __atomic_store_n(p, ok ? 2 : 0, __ATOMIC_RELEASE);
            RET(ok);
        }
        usleep(50);
    }
}
static void InitOnceBeginInitialize_(CPU *c)
{
    uint64_t *p = (uint64_t *)AP(0);
    uint32_t fl = AU(1);
    uint64_t pend = A(2);
    for (;;) {
        uint64_t v = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        if (v == 2) { if (pend) wr32(pend, 0); if (A(3)) wr64(A(3), 0); RET(1); }
        if (fl & 1) { w64_set_last_error(31); RET(0); }     /* INIT_ONCE_CHECK_ONLY, not done */
        uint64_t z = 0;
        if (v == 0 && __atomic_compare_exchange_n(p, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (pend) wr32(pend, 1);
            RET(1);
        }
        usleep(50);
    }
}
static void InitOnceComplete_(CPU *c)
{
    uint64_t *p = (uint64_t *)AP(0);
    __atomic_store_n(p, (AU(1) & 4) ? 0 : 2, __ATOMIC_RELEASE);   /* INIT_ONCE_INIT_FAILED */
    RET(1);
}

/* ------------------------------------------------------------------ waitable objects */
typedef struct {
    int kind;                    /* H_EVENT, H_MUTEX, H_SEM, H_THREAD */
    int manual, signaled;        /* event */
    _Atomic int32_t count;       /* semaphore (lock-free atomic + futex!) */
    int32_t max;                 /* semaphore max */
    uint64_t owner; int rec;     /* mutex */
    int exited; uint32_t code;   /* thread */
    pthread_t th;
    uint64_t tid;                /* thread: guest thread id */
    int start_hold;              /* thread: CREATE_SUSPENDED count not yet resumed */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    _Atomic uint32_t waiters;       /* WaitForSingleObject / CREATE_SUSPENDED waiters on THIS object */
    _Atomic uint32_t multi_waiters; /* WaitForMultipleObjects waiters watching THIS object */
} wobj_t;
/* Strict lock hierarchy: g_multi_mu (only held by WaitForMultipleObjects or multi-wake)
 * -> o->mu (in ascending pointer order if locking >1). Wakers ALWAYS release o->mu BEFORE
 * signaling o->cv or acquiring g_multi_mu (wobj_t is never freed), eliminating lock inversion
 * AND Bionic hurry-up-and-wait futex ping-pong! */
static pthread_mutex_t g_multi_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wcv = PTHREAD_COND_INITIALIZER;

static wobj_t *wobj_alloc(int kind)
{
    wobj_t *o = calloc(1, sizeof *o);
    o->kind = kind;
    pthread_mutex_init(&o->mu, NULL);
    pthread_cond_init(&o->cv, NULL);
    return o;
}
/* Release o->mu and wake waiters on `o` (caller holds o->mu). Safe to signal o->cv after unlock
 * because wobj_t objects live for the lifetime of the process. */
static inline void wobj_wake_unlock(wobj_t *o, int32_t count, int all)
{
    uint32_t w = atomic_load_explicit(&o->waiters, memory_order_relaxed);
    uint32_t mw = atomic_load_explicit(&o->multi_waiters, memory_order_relaxed);
    pthread_mutex_unlock(&o->mu);
    if (w) {
        if (all || (count > 1 && (uint32_t)count >= w)) pthread_cond_broadcast(&o->cv);
        else if (count <= 1) pthread_cond_signal(&o->cv);
        else { for (int32_t i = 0; i < count; i++) pthread_cond_signal(&o->cv); }
    }
    if (__builtin_expect(mw != 0, 0)) {
        pthread_mutex_lock(&g_multi_mu);
        pthread_cond_broadcast(&g_wcv);
        pthread_mutex_unlock(&g_multi_mu);
    }
}

static inline int32_t sem_post_fast(wobj_t *o, int32_t add)
{
    int32_t prev, next;
    do {
        prev = atomic_load_explicit(&o->count, memory_order_relaxed);
        next = prev + add;
        if (o->max && next > o->max) next = o->max;
    } while (!atomic_compare_exchange_weak_explicit(&o->count, &prev, next,
                                                    memory_order_acq_rel, memory_order_relaxed));
    atomic_thread_fence(memory_order_seq_cst);
    if (__builtin_expect(atomic_load_explicit(&o->waiters, memory_order_relaxed) != 0, 0))
        syscall(SYS_futex, (int32_t *)&o->count, FUTEX_WAKE_PRIVATE, add, NULL, NULL, 0);
    if (__builtin_expect(atomic_load_explicit(&o->multi_waiters, memory_order_relaxed) != 0, 0)) {
        pthread_mutex_lock(&g_multi_mu);
        pthread_cond_broadcast(&g_wcv);
        pthread_mutex_unlock(&g_multi_mu);
    }
    return prev;
}

static inline uint32_t sem_wait_fast(wobj_t *x, uint32_t ms)
{
    /* Fast lock-free acquire + brief spin (only if no other waiter is already sleeping)
     * so Unity JobQueue micro-job bursts hand off in ~100ns without futex syscalls! */
    int max_spin = (atomic_load_explicit(&x->waiters, memory_order_relaxed) == 0) ? 512 : 64;
    for (int spin = 0; ; spin++) {
        int32_t v = atomic_load_explicit(&x->count, memory_order_acquire);
        while (v > 0) {
            if (atomic_compare_exchange_weak_explicit(&x->count, &v, v - 1,
                                                      memory_order_acq_rel, memory_order_acquire))
                return WAIT_OBJECT_0;
        }
        if (ms == 0) return WAIT_TIMEOUT;
        if (spin < max_spin) {
#if defined(__aarch64__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
            continue;
        }
        break;
    }
    uint64_t deadline = (ms == INFINITE_) ? 0 : (w64_now_ns() + (uint64_t)ms * 1000000ull);
    atomic_fetch_add_explicit(&x->waiters, 1, memory_order_seq_cst);
    for (;;) {
        int32_t v = atomic_load_explicit(&x->count, memory_order_acquire);
        while (v > 0) {
            if (atomic_compare_exchange_weak_explicit(&x->count, &v, v - 1,
                                                      memory_order_acq_rel, memory_order_acquire)) {
                atomic_fetch_sub_explicit(&x->waiters, 1, memory_order_relaxed);
                return WAIT_OBJECT_0;
            }
        }
        struct timespec ts, *tsp = NULL;
        if (ms != INFINITE_) {
            uint64_t now = w64_now_ns();
            if (now >= deadline) {
                atomic_fetch_sub_explicit(&x->waiters, 1, memory_order_relaxed);
                return WAIT_TIMEOUT;
            }
            uint64_t rem = deadline - now;
            ts.tv_sec = (time_t)(rem / 1000000000ull);
            ts.tv_nsec = (long)(rem % 1000000000ull);
            tsp = &ts;
        }
        syscall(SYS_futex, (int32_t *)&x->count, FUTEX_WAIT_PRIVATE, 0, tsp, NULL, 0);
    }
}

static wobj_t *wobj(uint64_t h)
{
    int t = w64_handle_type(h);
    if (t == H_EVENT || t == H_MUTEX || t == H_SEM || t == H_THREAD) return w64_handle_get(h, t);
    return NULL;
}

static void CreateEventW_(CPU *c)
{
    wobj_t *o = wobj_alloc(H_EVENT);
    o->manual = AI(1) != 0; o->signaled = AI(2) != 0;
    RET(w64_handle_new(H_EVENT, o));
}
static void CreateEventExW_(CPU *c)     /* (sa, name, flags, access): 1 = manual reset, 2 = initially set */
{
    wobj_t *o = wobj_alloc(H_EVENT);
    o->manual = (AU(2) & 1) != 0; o->signaled = (AU(2) & 2) != 0;
    RET(w64_handle_new(H_EVENT, o));
}
static void SetEvent_(CPU *c)
{
    wobj_t *o = wobj(A(0));
    if (!o) RET(0);
    pthread_mutex_lock(&o->mu);
    o->signaled = 1;
    wobj_wake_unlock(o, 1, o->manual);
    RET(1);
}
static void ResetEvent_(CPU *c)
{
    wobj_t *o = wobj(A(0));
    if (!o) RET(0);
    pthread_mutex_lock(&o->mu); o->signaled = 0; pthread_mutex_unlock(&o->mu);
    RET(1);
}
static void CreateMutexW_(CPU *c)
{
    wobj_t *o = wobj_alloc(H_MUTEX);
    if (AI(1)) { o->owner = w64_thread_id(); o->rec = 1; }
    RET(w64_handle_new(H_MUTEX, o));
}
static void CreateSemaphoreW_(CPU *c)
{
    wobj_t *o = wobj_alloc(H_SEM);
    atomic_store_explicit(&o->count, AI(1), memory_order_relaxed);
    o->max = AI(2);
    RET(w64_handle_new(H_SEM, o));
}
static void CreateSemaphoreExW_(CPU *c)  /* (sa, initial, max, name, flags, access) */
{
    wobj_t *o = wobj_alloc(H_SEM);
    atomic_store_explicit(&o->count, AI(1), memory_order_relaxed);
    o->max = AI(2);
    RET(w64_handle_new(H_SEM, o));
}
static void ReleaseSemaphore_(CPU *c)
{
    wobj_t *o = wobj(A(0));
    if (!o || o->kind != H_SEM) RET(0);
    int32_t add = AI(1);
    if (add <= 0) RET(0);
    int32_t prev = sem_post_fast(o, add);
    if (A(2)) wr32(A(2), (uint32_t)prev);
    RET(1);
}

/* Host-side waitable objects (DXVK's frame latency semaphore, Flush1 events): guest handles. */
uint64_t w64_sem_create(int32_t initial, int32_t max)
{
    wobj_t *o = wobj_alloc(H_SEM);
    atomic_store_explicit(&o->count, initial, memory_order_relaxed);
    o->max = max;
    return w64_handle_new(H_SEM, o);
}
void w64_sem_release(uint64_t h)
{
    wobj_t *o = wobj(h);
    if (!o) return;
    if (o->kind == H_SEM) { sem_post_fast(o, 1); return; }
    pthread_mutex_lock(&o->mu);
    if (o->kind == H_EVENT) o->signaled = 1;
    wobj_wake_unlock(o, 1, o->kind == H_EVENT && o->manual);
}

static int w_ready(wobj_t *o, uint64_t me)
{
    switch (o->kind) {
    case H_EVENT: return o->signaled;
    case H_SEM: return atomic_load_explicit(&o->count, memory_order_acquire) > 0;
    case H_MUTEX: return !o->rec || o->owner == me;
    case H_THREAD: return o->exited;
    }
    return 1;
}
static int w_try_take(wobj_t *o, uint64_t me)
{
    switch (o->kind) {
    case H_EVENT: if (!o->signaled) return 0; if (!o->manual) o->signaled = 0; return 1;
    case H_SEM: {
        int32_t v = atomic_load_explicit(&o->count, memory_order_acquire);
        while (v > 0) {
            if (atomic_compare_exchange_weak_explicit(&o->count, &v, v - 1,
                                                      memory_order_acq_rel, memory_order_acquire))
                return 1;
        }
        return 0;
    }
    case H_MUTEX: if (o->rec && o->owner != me) return 0; o->owner = me; o->rec++; return 1;
    case H_THREAD: return o->exited != 0;
    }
    return 1;
}

uint32_t w64_wait_objs(int n, const uint64_t *hs, int all, uint32_t ms)
{
    wobj_t *o[64];
    if (n <= 0 || n > 64) return WAIT_FAILED;
    for (int i = 0; i < n; i++) {
        o[i] = wobj(hs[i]);
        if (!o[i]) {
            if (hs[i] == (uint64_t)-1 || hs[i] == (uint64_t)-2) { if (ms == INFINITE_) for (;;) sleep(1000); usleep(ms * 1000); return WAIT_TIMEOUT; }
            w64_set_last_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }
    /* Fast lock-free atomic + futex path for single-semaphore WaitForSingleObject */
    if (n == 1 && o[0]->kind == H_SEM) return sem_wait_fast(o[0], ms);
    uint64_t me = w64_thread_id();
    /* Per-object mutex + condvar for WaitForSingleObject (n == 1): zero global lock contention! */
    if (n == 1) {
        wobj_t *x = o[0];
        pthread_mutex_lock(&x->mu);
        for (;;) {
            if (w_try_take(x, me)) { pthread_mutex_unlock(&x->mu); return WAIT_OBJECT_0; }
            if (ms == 0) break;
            atomic_fetch_add_explicit(&x->waiters, 1, memory_order_relaxed);
            int r = timed_wait(&x->cv, &x->mu, ms);
            atomic_fetch_sub_explicit(&x->waiters, 1, memory_order_relaxed);
            if (r == ETIMEDOUT) {
                if (w_try_take(x, me)) { pthread_mutex_unlock(&x->mu); return WAIT_OBJECT_0; }
                break;
            }
        }
        pthread_mutex_unlock(&x->mu);
        return WAIT_TIMEOUT;
    }
    /* Multi-object path (WaitForMultipleObjects, n > 1): lock g_multi_mu then unique o[] sorted by addr. */
    wobj_t *u[64];
    int nu = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < nu; j++) if (u[j] == o[i]) { dup = 1; break; }
        if (!dup) u[nu++] = o[i];
    }
    for (int i = 1; i < nu; i++) {
        wobj_t *key = u[i]; int j = i - 1;
        while (j >= 0 && (uintptr_t)u[j] > (uintptr_t)key) { u[j + 1] = u[j]; j--; }
        u[j + 1] = key;
    }
    pthread_mutex_lock(&g_multi_mu);
    for (int i = 0; i < nu; i++) {
        pthread_mutex_lock(&u[i]->mu);
        atomic_fetch_add_explicit(&u[i]->multi_waiters, 1, memory_order_seq_cst);
    }
    uint32_t res = WAIT_TIMEOUT;
    for (;;) {
        if (all) {
            int ok = 1;
            for (int i = 0; i < n; i++) if (!w_ready(o[i], me)) { ok = 0; break; }
            if (ok) {
                int taken = 0;
                for (int i = 0; i < n; i++) {
                    if (!w_try_take(o[i], me)) {
                        for (int j = 0; j < taken; j++)
                            if (o[j]->kind == H_SEM) sem_post_fast(o[j], 1);
                        ok = 0;
                        break;
                    }
                    taken++;
                }
                if (ok) { res = WAIT_OBJECT_0; break; }
            }
        } else {
            int hit = -1;
            for (int i = 0; i < n; i++)
                if (w_try_take(o[i], me)) { hit = i; break; }
            if (hit >= 0) { res = WAIT_OBJECT_0 + (uint32_t)hit; break; }
        }
        if (ms == 0) break;
        for (int i = nu - 1; i >= 0; i--) pthread_mutex_unlock(&u[i]->mu);
        int r = timed_wait(&g_wcv, &g_multi_mu, ms);
        for (int i = 0; i < nu; i++) pthread_mutex_lock(&u[i]->mu);
        if (r == ETIMEDOUT) {
            for (int i = 0; i < n && !all; i++)
                if (w_try_take(o[i], me)) { res = WAIT_OBJECT_0 + (uint32_t)i; break; }
            break;
        }
    }
    for (int i = nu - 1; i >= 0; i--) {
        atomic_fetch_sub_explicit(&u[i]->multi_waiters, 1, memory_order_relaxed);
        pthread_mutex_unlock(&u[i]->mu);
    }
    pthread_mutex_unlock(&g_multi_mu);
    return res;
}
static void WaitForSingleObject_(CPU *c) { uint64_t h = A(0); RET(w64_wait_objs(1, &h, 0, AU(1))); }
static void WaitForSingleObjectEx_(CPU *c) { uint64_t h = A(0); RET(w64_wait_objs(1, &h, 0, AU(1))); }
static void WaitForMultipleObjects_(CPU *c)
{
    uint32_t n = AU(0);
    uint64_t hs[64];
    if (n > 64) RET(WAIT_FAILED);
    for (uint32_t i = 0; i < n; i++) hs[i] = rd64(A(1) + 8 * i);
    RET(w64_wait_objs((int)n, hs, AI(2) != 0, AU(3)));
}
int w64_close_waitable(uint64_t h)
{
    wobj_t *o = wobj(h);
    if (!o) return 0;
    w64_handle_close(h);     /* the object leaks deliberately: threads may still signal it */
    return 1;
}

/* ------------------------------------------------------------------ threads */
extern __thread uint64_t w64_preset_tid;
uint64_t w64_alloc_tid(void);
typedef struct {
    uint64_t start, param, tid;
    wobj_t *obj;
    jmp_buf exit_jb;
    uint32_t exit_code;
} thr_t;
static __thread thr_t *t_thr;

/* FLS: index space shared with TLS bookkeeping but values kept per thread here */
#define FLS_MAX 128
static uint64_t g_fls_cb[FLS_MAX];
static int g_fls_used[FLS_MAX];
static __thread uint64_t t_fls[FLS_MAX];
static pthread_mutex_t g_fls_mu = PTHREAD_MUTEX_INITIALIZER;

static void fls_run_callbacks(CPU *c)
{
    for (int i = 0; i < FLS_MAX; i++)
        if (g_fls_used[i] && g_fls_cb[i] && t_fls[i]) {
            uint64_t v = t_fls[i];
            t_fls[i] = 0;
            w64_call(c, g_fls_cb[i], 1, v);
        }
}

static void *thread_main(void *arg)
{
    thr_t *t = arg;
    pthread_mutex_lock(&t->obj->mu);
    while (t->obj->start_hold > 0) {
        t->obj->waiters++;
        pthread_cond_wait(&t->obj->cv, &t->obj->mu);   /* CREATE_SUSPENDED */
        t->obj->waiters--;
    }
    pthread_mutex_unlock(&t->obj->mu);
    t_thr = t;
    w64_preset_tid = t->tid;
    CPU *c = w64_cpu();          /* guest stack, TEB, TLS, DLL_THREAD_ATTACH callbacks */
    if (!setjmp(t->exit_jb)) {
        uint64_t r = w64_call(c, t->start, 1, t->param);
        t->exit_code = (uint32_t)r;
    }
    fls_run_callbacks(c);
    w64_thread_detach();
    pthread_mutex_lock(&t->obj->mu);
    t->obj->exited = 1;
    t->obj->code = t->exit_code;
    wobj_wake_unlock(t->obj, 1, 1);
    t_thr = NULL;
    free(t);
    return NULL;
}

static void CreateThread_(CPU *c)
{
    thr_t *t = calloc(1, sizeof *t);
    t->start = A(2); t->param = A(3); t->tid = w64_alloc_tid();
    t->obj = wobj_alloc(H_THREAD);
    t->obj->tid = t->tid;
    if (AU(4) & 4) t->obj->start_hold = 1;          /* CREATE_SUSPENDED */
    uint64_t h = w64_handle_new(H_THREAD, t->obj);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);      /* lifted frames live on the host stack */
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->obj->th, &at, thread_main, t)) { w64_handle_close(h); RET(0); }
    if (A(5)) wr32(A(5), (uint32_t)t->tid);
    if (w64_trace) w64_log("[thread] start %#llx param %#llx", (unsigned long long)t->start, (unsigned long long)t->param);
    RET(h);
}
/* ---- thread pool timers: a host thread per timer calls fn(instance, context, timer) ---- */
typedef struct { uint64_t fn, ctx; pthread_mutex_t m; pthread_cond_t cv; uint64_t due_ns; uint32_t period; int armed, closing, running; pthread_t th; } tptimer_t;
static void *tptimer_main(void *arg)
{
    tptimer_t *t = arg;
    pthread_mutex_lock(&t->m);
    while (!t->closing) {
        if (!t->armed) { pthread_cond_wait(&t->cv, &t->m); continue; }
        uint64_t now = w64_now_ns();
        if (now < t->due_ns) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t d = t->due_ns - now;
            ts.tv_sec += (time_t)(d / 1000000000ull); ts.tv_nsec += (long)(d % 1000000000ull);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&t->cv, &t->m, &ts);
            continue;
        }
        if (t->period) t->due_ns += (uint64_t)t->period * 1000000ull; else t->armed = 0;
        t->running = 1;
        pthread_mutex_unlock(&t->m);
        CPU *c = w64_cpu();
        w64_call(c, t->fn, 3, 0ull, t->ctx, (uint64_t)(uintptr_t)t);
        pthread_mutex_lock(&t->m);
        t->running = 0;
        pthread_cond_broadcast(&t->cv);
    }
    pthread_mutex_unlock(&t->m);
    return NULL;
}
static void CreateThreadpoolTimer_(CPU *c)
{
    tptimer_t *t = calloc(1, sizeof *t);
    t->fn = A(0); t->ctx = A(1);
    pthread_mutex_init(&t->m, NULL); pthread_cond_init(&t->cv, NULL);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_create(&t->th, &at, tptimer_main, t);
    RET((uintptr_t)t);
}
static void SetThreadpoolTimer_(CPU *c)                  /* (timer, FILETIME *due, period ms, window) */
{
    tptimer_t *t = AP(0);
    pthread_mutex_lock(&t->m);
    if (!A(1)) t->armed = 0;
    else {
        int64_t due = (int64_t)rd64(A(1));
        uint64_t now = w64_now_ns();
        if (due < 0) t->due_ns = now + (uint64_t)(-due) * 100ull;
        else {                                            /* absolute FILETIME */
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            int64_t ft_now = 116444736000000000ll + (int64_t)ts.tv_sec * 10000000ll + ts.tv_nsec / 100;
            t->due_ns = now + (due > ft_now ? (uint64_t)(due - ft_now) * 100ull : 0);
        }
        t->period = AU(2); t->armed = 1;
    }
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->m);
    RETV();
}
static void WaitForThreadpoolTimerCallbacks_(CPU *c)
{
    tptimer_t *t = AP(0);
    pthread_mutex_lock(&t->m);
    if (AU(1)) t->armed = 0;
    while (t->running) pthread_cond_wait(&t->cv, &t->m);
    pthread_mutex_unlock(&t->m);
    RETV();
}
static void CloseThreadpoolTimer_(CPU *c)
{
    tptimer_t *t = AP(0);
    pthread_mutex_lock(&t->m); t->closing = 1; t->armed = 0; pthread_cond_broadcast(&t->cv); pthread_mutex_unlock(&t->m);
    RETV();                                               /* the thread exits on its own; t is leaked */
}
static void GetTickCount64_(CPU *c) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); RET((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000); }
static void GetCurrentProcessorNumber_(CPU *c) { int n = sched_getcpu(); RET(n < 0 ? 0 : (uint32_t)n); }
static void FlushProcessWriteBuffers_(CPU *c) { __atomic_thread_fence(__ATOMIC_SEQ_CST); RETV(); }
static void GetCurrentPackageId_(CPU *c) { RET(15700); }            /* APPMODEL_ERROR_NO_PACKAGE */
static void CreateSymbolicLinkW_(CPU *c) { w64_set_last_error(1314); RET(0); }

/* ---- WaitOnAddress / WakeByAddress* ----
 * Unity's baselib futex (job system workers, semaphores) and the MSVC STL atomic waits. 4- and
 * 8-byte waits are Linux futexes on the (identity-mapped) address itself - an 8-byte wait sleeps
 * on its low word; every value change is followed by a wake on the same address, so a change of
 * the high word alone still wakes it. Other sizes share one condition variable. One condition
 * for everything woke all ~20 job workers for every single wake (sys time, latency). */
#include <linux/futex.h>
static pthread_mutex_t g_woa_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_woa_cv = PTHREAD_COND_INITIALIZER;
static int g_woa_slow_waiters;
static long futex_op(void *addr, int op, uint32_t val, const struct timespec *ts)
{
    return syscall(SYS_futex, addr, op | FUTEX_PRIVATE_FLAG, val, ts, NULL, 0);
}
static void WaitOnAddress_(CPU *c)
{
    const void *addr = AP(0), *cmp = AP(1);
    size_t n = (size_t)A(2);
    uint32_t ms = AU(3);
    if (n > 8) n = 8;
    if ((n == 4 || n == 8) && !((uintptr_t)addr & (n - 1))) {
        if (memcmp(addr, cmp, n)) RET(1);
        uint32_t expect; memcpy(&expect, cmp, 4);
        struct timespec ts, *tp = NULL;
        if (ms != 0xFFFFFFFFu) { ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000; tp = &ts; }
        long r = futex_op((void *)addr, FUTEX_WAIT, expect, tp);
        if (r < 0 && errno == ETIMEDOUT) { w64_set_last_error(1460 /* ERROR_TIMEOUT */); RET(0); }
        RET(1);                                   /* woken, value changed (EAGAIN) or signal */
    }
    pthread_mutex_lock(&g_woa_mu);
    if (memcmp(addr, cmp, n)) { pthread_mutex_unlock(&g_woa_mu); RET(1); }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t add = ms == 0xFFFFFFFFu ? 50ull * 1000000 : (uint64_t)ms * 1000000;   /* re-check at least every 50 ms */
    ts.tv_sec += (time_t)(add / 1000000000); ts.tv_nsec += (long)(add % 1000000000);
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    g_woa_slow_waiters++;
    int r = pthread_cond_timedwait(&g_woa_cv, &g_woa_mu, &ts);
    g_woa_slow_waiters--;
    pthread_mutex_unlock(&g_woa_mu);
    if (r && ms != 0xFFFFFFFFu) { w64_set_last_error(1460 /* ERROR_TIMEOUT */); RET(0); }
    RET(1);
}
static void wake_by_address(const void *addr, int all)
{
    if (!((uintptr_t)addr & 3)) futex_op((void *)addr, FUTEX_WAKE, all ? 0x7FFFFFFF : 1, NULL);
    if (__atomic_load_n(&g_woa_slow_waiters, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&g_woa_mu); pthread_cond_broadcast(&g_woa_cv); pthread_mutex_unlock(&g_woa_mu);
    }
}
static void WakeByAddressSingle_(CPU *c) { wake_by_address(AP(0), 0); RETV(); }
static void WakeByAddressAll_(CPU *c) { wake_by_address(AP(0), 1); RETV(); }

/* ---- SuspendThread / ResumeThread / GetThreadContext (core.c parks threads at import calls) ---- */
int w64_thread_suspend(uint64_t tid);
int w64_thread_resume(uint64_t tid);
CPU *w64_thread_cpu(uint64_t tid);
static uint64_t thread_tid(uint64_t h)
{
    if (h == (uint64_t)-2) return w64_thread_id();
    wobj_t *o = wobj(h);
    return o && o->kind == H_THREAD ? o->tid : 0;
}
static void SuspendThread_(CPU *c)
{
    wobj_t *o = A(0) == (uint64_t)-2 ? NULL : wobj(A(0));
    if (o && o->kind == H_THREAD && o->start_hold > 0) { pthread_mutex_lock(&o->mu); int p = o->start_hold++; pthread_mutex_unlock(&o->mu); RET((uint32_t)p); }
    int r = w64_thread_suspend(thread_tid(A(0)));
    RET(r < 0 ? 0xFFFFFFFFu : (uint32_t)r);
}
static void ResumeThread_(CPU *c)
{
    wobj_t *o = A(0) == (uint64_t)-2 ? NULL : wobj(A(0));
    if (o && o->kind == H_THREAD && o->start_hold > 0) {
        pthread_mutex_lock(&o->mu);
        int p = o->start_hold--;
        wobj_wake_unlock(o, 1, 1);
        RET((uint32_t)p);
    }
    int r = w64_thread_resume(thread_tid(A(0)));
    RET(r < 0 ? 0xFFFFFFFFu : (uint32_t)r);
}
/* CONTEXT (x64): Rax at 0x78 ... R15 at 0xF0, Rip 0xF8, MxCsr 0x34, EFlags 0x44, Xmm0 at 0x1A0 */
static void GetThreadContext_(CPU *c)
{
    CPU *t = w64_thread_cpu(thread_tid(A(0)));
    if (!t) { w64_set_last_error(6); RET(0); }
    uint8_t *ctx = AP(1);
    uint64_t regs[16] = { t->rax, t->rcx, t->rdx, t->rbx, t->rsp, t->rbp, t->rsi, t->rdi,
                          t->r8, t->r9, t->r10, t->r11, t->r12, t->r13, t->r14, t->r15 };
    memcpy(ctx + 0x78, regs, sizeof regs);
    memcpy(ctx + 0xF8, &t->rip, 8);
    memcpy(ctx + 0x34, &t->mxcsr, 4);
    for (int i = 0; i < 16; i++) memcpy(ctx + 0x1A0 + 16 * i, &t->xmm[i], 16);
    RET(1);
}
static void GetThreadId_(CPU *c) { RET(thread_tid(A(0))); }
/* DuplicateHandle(GetCurrentThread()) is how threads hand out a real handle to themselves. */
static void DuplicateHandle_(CPU *c)
{
    uint64_t src = A(1), out = A(3);
    if (src == (uint64_t)-2) {
        wobj_t *o = wobj_alloc(H_THREAD);
        o->tid = w64_thread_id();
        if (out) wr64(out, w64_handle_new(H_THREAD, o));
        RET(1);
    }
    if (out) wr64(out, src);
    RET(1);
}

static void ExitThread_(CPU *c)
{
    if (t_thr) { t_thr->exit_code = AU(0); longjmp(t_thr->exit_jb, 1); }
    w64_log("ExitThread on a thread not created by CreateThread");
    pthread_exit(NULL);
}
static void FreeLibraryAndExitThread_(CPU *c)
{
    if (t_thr) { t_thr->exit_code = AU(1); longjmp(t_thr->exit_jb, 1); }
    pthread_exit(NULL);
}
static void GetExitCodeThread_(CPU *c)
{
    wobj_t *o = wobj(A(0));
    if (!o) RET(0);
    wr32(A(1), o->exited ? o->code : 259 /* STILL_ACTIVE */);
    RET(1);
}
static void GetCurrentThread_(CPU *c) { RET((uint64_t)-2); }
static void GetCurrentThreadId_(CPU *c) { RET((uint32_t)w64_thread_id()); }
static void GetCurrentProcess_(CPU *c) { RET((uint64_t)-1); }
static void GetCurrentProcessId_(CPU *c) { RET(0x100); }
static void SetThreadPriority_(CPU *c) { RET(1); }
static void Sleep_(CPU *c)
{
    uint32_t ms = AU(0);
    if (ms == 0) sched_yield(); else usleep((useconds_t)ms * 1000);
    RETV();
}

/* ------------------------------------------------------------------ TLS / FLS */
static int g_tls_used[1088];
static pthread_mutex_t g_tls_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tls_slot_addr(uint32_t i)
{
    uint64_t teb = (uint64_t)(uintptr_t)w64_teb;
    if (i < 64) return teb + TEB_TLS_SLOTS + 8 * i;
    uint64_t ex = rd64(teb + TEB_TLS_EXP);
    if (!ex) { ex = (uint64_t)(uintptr_t)calloc(1024, 8); wr64(teb + TEB_TLS_EXP, ex); }
    return ex + 8 * (uint64_t)(i - 64);
}
static void TlsAlloc_(CPU *c)
{
    pthread_mutex_lock(&g_tls_mu);
    for (uint32_t i = 0; i < 1088; i++)
        if (!g_tls_used[i]) { g_tls_used[i] = 1; pthread_mutex_unlock(&g_tls_mu); RET(i); }
    pthread_mutex_unlock(&g_tls_mu);
    RET(0xFFFFFFFFu);
}
static void TlsFree_(CPU *c) { uint32_t i = AU(0); if (i < 1088) g_tls_used[i] = 0; RET(1); }
uint64_t w64_tls_get(uint32_t i)
{
    w64_set_last_error(0);
    return i < 1088 ? rd64(tls_slot_addr(i)) : 0;
}
static void TlsGetValue_(CPU *c) { RET(w64_tls_get(AU(0))); }
static void TlsSetValue_(CPU *c) { uint32_t i = AU(0); if (i >= 1088) RET(0); wr64(tls_slot_addr(i), A(1)); RET(1); }

static void FlsAlloc_(CPU *c)
{
    pthread_mutex_lock(&g_fls_mu);
    for (uint32_t i = 1; i < FLS_MAX; i++)
        if (!g_fls_used[i]) { g_fls_used[i] = 1; g_fls_cb[i] = A(0); pthread_mutex_unlock(&g_fls_mu); RET(i); }
    pthread_mutex_unlock(&g_fls_mu);
    RET(0xFFFFFFFFu);
}
static void FlsFree_(CPU *c) { uint32_t i = AU(0); if (i < FLS_MAX) { g_fls_used[i] = 0; g_fls_cb[i] = 0; } RET(1); }
static void FlsGetValue_(CPU *c) { uint32_t i = AU(0); RET(i < FLS_MAX ? t_fls[i] : 0); }
static void FlsSetValue_(CPU *c) { uint32_t i = AU(0); if (i >= FLS_MAX) RET(0); t_fls[i] = A(1); RET(1); }

/* ------------------------------------------------------------------ SList (global lock is enough) */
static pthread_mutex_t g_sl_mu = PTHREAD_MUTEX_INITIALIZER;
static void InitializeSListHead_(CPU *c) { memset(AP(0), 0, 16); RETV(); }
static void InterlockedPushEntrySList_(CPU *c)
{
    pthread_mutex_lock(&g_sl_mu);
    uint64_t head = A(0), e = A(1), old = rd64(head);
    wr64(e, old);
    wr64(head, e);
    wr16(head + 8, (uint16_t)(rd16(head + 8) + 1));
    pthread_mutex_unlock(&g_sl_mu);
    RET(old);
}

static void InterlockedPopEntrySList_(CPU *c)
{
    pthread_mutex_lock(&g_sl_mu);
    uint64_t head = A(0), e = rd64(head);
    if (e) { wr64(head, rd64(e)); wr16(head + 8, (uint16_t)(rd16(head + 8) - 1)); }
    pthread_mutex_unlock(&g_sl_mu);
    RET(e);
}
static void InterlockedFlushSList_(CPU *c)
{
    pthread_mutex_lock(&g_sl_mu);
    uint64_t head = A(0), e = rd64(head);
    wr64(head, 0); wr16(head + 8, 0);
    pthread_mutex_unlock(&g_sl_mu);
    RET(e);
}
static void QueryDepthSList_(CPU *c) { RET(rd16(A(0) + 8)); }

/* ------------------------------------------------------------------ more waiting */
static void SleepEx_(CPU *c) { uint32_t ms = AU(0); if (ms == INFINITE_) for (;;) sleep(1000); usleep(ms ? ms * 1000u : 0); if (!ms) sched_yield(); RET(0); }
static void SwitchToThread_(CPU *c) { sched_yield(); RET(1); }
static void WaitForMultipleObjectsEx_(CPU *c)
{
    uint32_t n = AU(0);
    uint64_t hs[64];
    if (n > 64) RET(WAIT_FAILED);
    for (uint32_t i = 0; i < n; i++) hs[i] = rd64(A(1) + 8 * i);
    RET(w64_wait_objs((int)n, hs, AI(2) != 0, AU(3)));
}
static void SetEvent_(CPU *c);
static void ReleaseSemaphore_(CPU *c);
static void SignalObjectAndWait_(CPU *c)                  /* (signal, wait, ms, alertable) */
{
    wobj_t *o = wobj(A(0));
    if (o) {
        pthread_mutex_lock(&o->mu);
        if (o->kind == H_EVENT) o->signaled = 1;
        else if (o->kind == H_SEM && (!o->max || o->count < o->max)) o->count++;
        else if (o->kind == H_MUTEX && o->rec > 0 && --o->rec == 0) o->owner = 0;
        wobj_wake_unlock(o, 1, o->kind == H_EVENT && o->manual);
    }
    uint64_t h = A(1);
    RET(w64_wait_objs(1, &h, 0, AU(2)));
}
static void OpenEventA_(CPU *c) { w64_set_last_error(2); RET(0); }

/* waitable timer = auto/manual event driven by one persistent host thread per timer */
typedef struct {
    uint64_t h, due_ns;
    uint32_t period;
    int armed, started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} wtim_t;
static void *wtimer_main(void *arg)
{
    wtim_t *t = arg;
    pthread_mutex_lock(&t->mu);
    for (;;) {
        while (!t->armed) pthread_cond_wait(&t->cv, &t->mu);
        uint64_t due = t->due_ns;
        uint64_t now = w64_now_ns();
        if (due > now) {
            struct timespec ts = { (time_t)(due / 1000000000ull), (long)(due % 1000000000ull) };
            pthread_cond_timedwait(&t->cv, &t->mu, &ts);
            if (!t->armed || t->due_ns != due) continue;
            if (w64_now_ns() < due) continue;
        }
        uint32_t period = t->period;
        if (period) t->due_ns = due + (uint64_t)period * 1000000ull;
        else t->armed = 0;
        uint64_t h = t->h;
        pthread_mutex_unlock(&t->mu);
        wobj_t *o = wobj(h);
        if (!o) return NULL;
        pthread_mutex_lock(&o->mu);
        o->signaled = 1;
        wobj_wake_unlock(o, 1, o->manual);
        pthread_mutex_lock(&t->mu);
    }
    return NULL;
}
static wtim_t *g_wtims[256];
static void create_wtimer(CPU *c, int manual)
{
    wobj_t *o = wobj_alloc(H_EVENT);
    o->manual = manual;
    uint64_t h = w64_handle_new(H_EVENT, o);
    wtim_t *t = calloc(1, sizeof *t); t->h = h;
    pthread_mutex_init(&t->mu, NULL);
    pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&t->cv, &ca); pthread_condattr_destroy(&ca);
    g_wtims[(h / 4) & 255] = t;
    RET(h);
}
static void CreateWaitableTimerExW_(CPU *c) { create_wtimer(c, (AU(2) & 1) != 0); }   /* CREATE_WAITABLE_TIMER_MANUAL_RESET */
static void CreateWaitableTimerA_(CPU *c) { create_wtimer(c, AI(1) != 0); }
/* SetWaitableTimer(h, LARGE_INTEGER *due (100 ns, < 0 relative), period ms, apc, arg, resume) */
static void SetWaitableTimer_(CPU *c)
{
    wtim_t *t = g_wtims[(A(0) / 4) & 255];
    wobj_t *o = wobj(A(0));
    if (!t || t->h != A(0) || !o) RET(0);
    int64_t due = (int64_t)rd64(A(1));
    pthread_mutex_lock(&o->mu); o->signaled = 0; pthread_mutex_unlock(&o->mu);
    uint64_t now = w64_now_ns(), target;
    if (due < 0) target = now + (uint64_t)(-due) * 100ull;
    else {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        int64_t ft_now = 116444736000000000ll + (int64_t)ts.tv_sec * 10000000ll + ts.tv_nsec / 100;
        target = now + (due > ft_now ? (uint64_t)(due - ft_now) * 100ull : 0);
    }
    pthread_mutex_lock(&t->mu);
    t->due_ns = target;
    t->period = AU(2);
    t->armed = 1;
    if (!t->started) {
        t->started = 1;
        pthread_t th; pthread_create(&th, NULL, wtimer_main, t); pthread_detach(th);
    }
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mu);
    RET(1);
}

/* RegisterWaitForSingleObject(*out, handle, cb, ctx, ms, flags): cb(ctx, timedOut) on a host thread */
typedef struct { uint64_t h, cb, ctx; uint32_t ms, flags; volatile int stop; } rwait_t;
static void *rwait_main(void *arg)
{
    rwait_t *w = arg;
    CPU *c = w64_cpu();
    while (!w->stop) {
        uint32_t r = w64_wait_objs(1, &w->h, 0, w->ms == INFINITE_ ? 100 : w->ms);
        if (w->stop) break;
        if (r == WAIT_TIMEOUT && w->ms == INFINITE_) continue;
        w64_call(c, w->cb, 2, w->ctx, (uint64_t)(r == WAIT_TIMEOUT));
        if (w->flags & 8) break;                          /* WT_EXECUTEONLYONCE */
    }
    return NULL;
}
static void RegisterWaitForSingleObject_(CPU *c)
{
    rwait_t *w = calloc(1, sizeof *w);
    w->h = A(1); w->cb = A(2); w->ctx = A(3); w->ms = AU(4); w->flags = (uint32_t)w64_arg(c, 5);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th; pthread_create(&th, &at, rwait_main, w); pthread_detach(th);
    wr64(A(0), (uint64_t)(uintptr_t)w);
    RET(1);
}
static void UnregisterWait_(CPU *c) { rwait_t *w = AP(0); if (w) w->stop = 1; RET(1); }

/* timer queues: CreateTimerQueueTimer(*out, queue, cb, param, due ms, period ms, flags) -> cb(param, TRUE) */
typedef struct { uint64_t cb, prm; uint32_t due, period; volatile int stop; } tqt_t;
static void *tqt_main(void *arg)
{
    tqt_t *t = arg;
    CPU *c = w64_cpu();
    usleep(t->due * 1000u);
    while (!t->stop) {
        w64_call(c, t->cb, 2, t->prm, 1ull);
        if (!t->period) break;
        usleep(t->period * 1000u);
    }
    return NULL;
}
static void CreateTimerQueue_(CPU *c) { RET(0x7E0Eull); }
static void CreateTimerQueueTimer_(CPU *c)
{
    tqt_t *t = calloc(1, sizeof *t);
    t->cb = A(2); t->prm = A(3); t->due = (uint32_t)w64_arg(c, 4); t->period = (uint32_t)w64_arg(c, 5);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th; pthread_create(&th, &at, tqt_main, t); pthread_detach(th);
    wr64(A(0), (uint64_t)(uintptr_t)t);
    RET(1);
}
static void ChangeTimerQueueTimer_(CPU *c) { tqt_t *t = AP(1); if (t) t->period = AU(3); RET(1); }
static void DeleteTimerQueueTimer_(CPU *c) { tqt_t *t = AP(1); if (t) t->stop = 1; RET(1); }

static void OpenThread_(CPU *c)                           /* (access, inherit, tid) */
{
    wobj_t *o = wobj_alloc(H_THREAD);
    o->tid = AU(2);
    RET(w64_handle_new(H_THREAD, o));
}
static void SetThreadAffinityMask_(CPU *c) { RET(0xFF); }
static void SetThreadErrorMode_(CPU *c) { if (A(1)) wr32(A(1), 0); RET(1); }
static void LocalAlloc_(CPU *c) { RET((uintptr_t)heap_alloc(A(1) ? A(1) : 1, (AU(0) & 0x40) != 0)); }

/* ------------------------------------------------------------------ thread pool work */
typedef struct { uint64_t cb, ctx; int pending; pthread_mutex_t m; pthread_cond_t cv; } tpw_t;
static void *tp_main(void *arg)
{
    tpw_t *w = arg;
    CPU *c = w64_cpu();
    w64_call(c, w->cb, 3, 0ull, w->ctx, (uint64_t)(uintptr_t)w);
    fls_run_callbacks(c);
    w64_thread_detach();
    pthread_mutex_lock(&w->m); w->pending--; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m);
    return NULL;
}
static void CreateThreadpoolWork_(CPU *c)
{
    tpw_t *w = calloc(1, sizeof *w);
    w->cb = A(0); w->ctx = A(1);
    pthread_mutex_init(&w->m, NULL); pthread_cond_init(&w->cv, NULL);
    RET((uintptr_t)w);
}
static void SubmitThreadpoolWork_(CPU *c)
{
    tpw_t *w = AP(0);
    pthread_mutex_lock(&w->m); w->pending++; pthread_mutex_unlock(&w->m);
    pthread_t th; pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, tp_main, w);
    RETV();
}
static void WaitForThreadpoolWorkCallbacks_(CPU *c)
{
    tpw_t *w = AP(0);
    pthread_mutex_lock(&w->m); while (w->pending) pthread_cond_wait(&w->cv, &w->m); pthread_mutex_unlock(&w->m);
    RETV();
}
static void CloseThreadpoolWork_(CPU *c) { RETV(); }
static void FreeLibraryWhenCallbackReturns_(CPU *c) { RETV(); }

/* ------------------------------------------------------------------ time */
#define EPOCH_DIFF_100NS 116444736000000000ull
static uint64_t filetime_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return EPOCH_DIFF_100NS + (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}
/* 10 MHz ticks since process start, offset by one hour like a freshly booted PC */
uint64_t w64_qpc(void)
{
    static uint64_t t0;
    uint64_t n = w64_now_ns();
    if (!t0) t0 = n;
    return (n - t0) / 100 + 36000000000ull;
}
static void QueryPerformanceCounter_(CPU *c) { wr64(A(0), w64_qpc()); RET(1); }
static void QueryPerformanceFrequency_(CPU *c) { wr64(A(0), 10000000ull); RET(1); }
static void GetTickCount_(CPU *c) { RET((uint32_t)(w64_now_ns() / 1000000ull)); }
static void GetSystemTimeAsFileTime_(CPU *c) { wr64(A(0), filetime_now()); RETV(); }

static void ft_to_st(uint64_t ft, uint64_t st)
{
    time_t t = (time_t)((ft - EPOCH_DIFF_100NS) / 10000000ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    uint16_t v[8] = { (uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday,
                      (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, (uint16_t)((ft / 10000) % 1000) };
    memcpy((void *)(uintptr_t)st, v, 16);
}
static uint64_t st_to_ft(uint64_t st)
{
    uint16_t v[8];
    memcpy(v, (void *)(uintptr_t)st, 16);
    struct tm tm = { 0 };
    tm.tm_year = v[0] - 1900; tm.tm_mon = v[1] - 1; tm.tm_mday = v[3];
    tm.tm_hour = v[4]; tm.tm_min = v[5]; tm.tm_sec = v[6];
    time_t t = timegm(&tm);
    return EPOCH_DIFF_100NS + (uint64_t)t * 10000000ull + (uint64_t)v[7] * 10000ull;
}
static void GetSystemTime_(CPU *c) { ft_to_st(filetime_now(), A(0)); RETV(); }
static void FileTimeToSystemTime_(CPU *c) { ft_to_st(rd64(A(0)), A(1)); RET(1); }
static void SystemTimeToFileTime_(CPU *c) { wr64(A(1), st_to_ft(A(0))); RET(1); }
static void SystemTimeToTzSpecificLocalTime_(CPU *c) { memcpy(AP(2), AP(1), 16); RET(1); }
static void GetTimeZoneInformation_(CPU *c) { memset(AP(0), 0, 172); RET(0); }
static void timeBeginPeriod_(CPU *c) { RET(0); }
static void timeEndPeriod_(CPU *c) { RET(0); }

static const w64_export_t k32_sync_exports[] = {
    { "KERNEL32.dll", "GetProcessHeap", GetProcessHeap_ },
    { "KERNEL32.dll", "HeapCreate", HeapCreate_ },
    { "KERNEL32.dll", "HeapDestroy", HeapDestroy_ },
    { "KERNEL32.dll", "HeapAlloc", HeapAlloc_ },
    { "KERNEL32.dll", "HeapFree", HeapFree_ },
    { "KERNEL32.dll", "HeapReAlloc", HeapReAlloc_ },
    { "KERNEL32.dll", "HeapSize", HeapSize_ },
    { "KERNEL32.dll", "HeapValidate", HeapValidate_ },
    { "KERNEL32.dll", "HeapCompact", HeapCompact_ },
    { "KERNEL32.dll", "GlobalAlloc", GlobalAlloc_ },
    { "KERNEL32.dll", "GlobalFree", GlobalFree_ },
    { "KERNEL32.dll", "GlobalLock", GlobalLock_ },
    { "KERNEL32.dll", "GlobalUnlock", GlobalUnlock_ },
    { "KERNEL32.dll", "GlobalSize", GlobalSize_ },
    { "KERNEL32.dll", "LocalFree", LocalFree_ },
    { "KERNEL32.dll", "VirtualAlloc", VirtualAlloc_ },
    { "KERNEL32.dll", "VirtualFree", VirtualFree_ },
    { "KERNEL32.dll", "InitializeCriticalSection", InitializeCriticalSection_ },
    { "KERNEL32.dll", "InitializeCriticalSectionAndSpinCount", InitializeCriticalSectionAndSpinCount_ },
    { "KERNEL32.dll", "InitializeCriticalSectionEx", InitializeCriticalSectionEx_ },
    { "KERNEL32.dll", "EnterCriticalSection", EnterCriticalSection_ },
    { "KERNEL32.dll", "TryEnterCriticalSection", TryEnterCriticalSection_ },
    { "KERNEL32.dll", "LeaveCriticalSection", LeaveCriticalSection_ },
    { "KERNEL32.dll", "DeleteCriticalSection", DeleteCriticalSection_ },
    { "KERNEL32.dll", "AcquireSRWLockExclusive", AcquireSRWLockExclusive_ },
    { "KERNEL32.dll", "AcquireSRWLockShared", AcquireSRWLockShared_ },
    { "KERNEL32.dll", "ReleaseSRWLockExclusive", ReleaseSRWLockExclusive_ },
    { "KERNEL32.dll", "ReleaseSRWLockShared", ReleaseSRWLockShared_ },
    { "KERNEL32.dll", "TryAcquireSRWLockExclusive", TryAcquireSRWLockExclusive_ },
    { "KERNEL32.dll", "SleepConditionVariableSRW", SleepConditionVariableSRW_ },
    { "KERNEL32.dll", "WakeConditionVariable", WakeConditionVariable_ },
    { "KERNEL32.dll", "WakeAllConditionVariable", WakeAllConditionVariable_ },
    { "KERNEL32.dll", "InitOnceBeginInitialize", InitOnceBeginInitialize_ },
    { "KERNEL32.dll", "InitOnceComplete", InitOnceComplete_ },
    { "KERNEL32.dll", "CreateEventW", CreateEventW_ },
    { "KERNEL32.dll", "CreateEventA", CreateEventW_ },
    { "KERNEL32.dll", "CreateEventExW", CreateEventExW_ },
    { "KERNEL32.dll", "CreateMutexA", CreateMutexW_ },
    { "KERNEL32.dll", "CreateSemaphoreExW", CreateSemaphoreExW_ },
    { "KERNEL32.dll", "SetEvent", SetEvent_ },
    { "KERNEL32.dll", "ResetEvent", ResetEvent_ },
    { "KERNEL32.dll", "CreateMutexW", CreateMutexW_ },
    { "KERNEL32.dll", "CreateSemaphoreW", CreateSemaphoreW_ },
    { "KERNEL32.dll", "ReleaseSemaphore", ReleaseSemaphore_ },
    { "KERNEL32.dll", "WaitForSingleObject", WaitForSingleObject_ },
    { "KERNEL32.dll", "WaitForSingleObjectEx", WaitForSingleObjectEx_ },
    { "KERNEL32.dll", "WaitForMultipleObjects", WaitForMultipleObjects_ },
    { "KERNEL32.dll", "CreateThread", CreateThread_ },
    { "KERNEL32.dll", "ExitThread", ExitThread_ },
    { "KERNEL32.dll", "FreeLibraryAndExitThread", FreeLibraryAndExitThread_ },
    { "KERNEL32.dll", "GetExitCodeThread", GetExitCodeThread_ },
    { "KERNEL32.dll", "GetCurrentThread", GetCurrentThread_ },
    { "KERNEL32.dll", "SuspendThread", SuspendThread_ },
    { "KERNEL32.dll", "InterlockedPopEntrySList", InterlockedPopEntrySList_ },
    { "KERNEL32.dll", "InterlockedFlushSList", InterlockedFlushSList_ },
    { "KERNEL32.dll", "QueryDepthSList", QueryDepthSList_ },
    { "KERNEL32.dll", "SleepEx", SleepEx_ },
    { "KERNEL32.dll", "SwitchToThread", SwitchToThread_ },
    { "KERNEL32.dll", "WaitForMultipleObjectsEx", WaitForMultipleObjectsEx_ },
    { "KERNEL32.dll", "SignalObjectAndWait", SignalObjectAndWait_ },
    { "KERNEL32.dll", "OpenEventA", OpenEventA_ },
    { "KERNEL32.dll", "CreateWaitableTimerExW", CreateWaitableTimerExW_ },
    { "KERNEL32.dll", "CreateWaitableTimerA", CreateWaitableTimerA_ },
    { "KERNEL32.dll", "SetWaitableTimer", SetWaitableTimer_ },
    { "KERNEL32.dll", "RegisterWaitForSingleObject", RegisterWaitForSingleObject_ },
    { "KERNEL32.dll", "UnregisterWait", UnregisterWait_ },
    { "KERNEL32.dll", "UnregisterWaitEx", UnregisterWait_ },
    { "KERNEL32.dll", "CreateTimerQueue", CreateTimerQueue_ },
    { "KERNEL32.dll", "CreateTimerQueueTimer", CreateTimerQueueTimer_ },
    { "KERNEL32.dll", "ChangeTimerQueueTimer", ChangeTimerQueueTimer_ },
    { "KERNEL32.dll", "DeleteTimerQueueTimer", DeleteTimerQueueTimer_ },
    { "KERNEL32.dll", "OpenThread", OpenThread_ },
    { "KERNEL32.dll", "SetThreadAffinityMask", SetThreadAffinityMask_ },
    { "KERNEL32.dll", "SetThreadErrorMode", SetThreadErrorMode_ },
    { "KERNEL32.dll", "LocalAlloc", LocalAlloc_ },
    { "KERNEL32.dll", "VirtualProtect", VirtualProtect_ },
    { "KERNEL32.dll", "VirtualQuery", VirtualQuery_ },
    { "KERNEL32.dll", "SleepConditionVariableCS", SleepConditionVariableCS_ },
    { "KERNEL32.dll", "InitializeConditionVariable", InitializeConditionVariable_ },
    { "KERNEL32.dll", "InitializeSRWLock", InitializeSRWLock_ },
    { "KERNEL32.dll", "InitOnceExecuteOnce", InitOnceExecuteOnce_ },
    { "KERNEL32.dll", "CreateThreadpoolTimer", CreateThreadpoolTimer_ },
    { "KERNEL32.dll", "SetThreadpoolTimer", SetThreadpoolTimer_ },
    { "KERNEL32.dll", "WaitForThreadpoolTimerCallbacks", WaitForThreadpoolTimerCallbacks_ },
    { "KERNEL32.dll", "CloseThreadpoolTimer", CloseThreadpoolTimer_ },
    { "KERNEL32.dll", "GetTickCount64", GetTickCount64_ },
    { "KERNEL32.dll", "GetCurrentProcessorNumber", GetCurrentProcessorNumber_ },
    { "KERNEL32.dll", "FlushProcessWriteBuffers", FlushProcessWriteBuffers_ },
    { "KERNEL32.dll", "GetCurrentPackageId", GetCurrentPackageId_ },
    { "KERNEL32.dll", "CreateSymbolicLinkW", CreateSymbolicLinkW_ },
    { "KERNEL32.dll", "WaitOnAddress", WaitOnAddress_ },
    { "KERNEL32.dll", "WakeByAddressSingle", WakeByAddressSingle_ },
    { "KERNEL32.dll", "WakeByAddressAll", WakeByAddressAll_ },
    { "KERNEL32.dll", "ResumeThread", ResumeThread_ },
    { "KERNEL32.dll", "GetThreadContext", GetThreadContext_ },
    { "KERNEL32.dll", "GetThreadId", GetThreadId_ },
    { "KERNEL32.dll", "DuplicateHandle", DuplicateHandle_ },
    { "KERNEL32.dll", "GetCurrentThreadId", GetCurrentThreadId_ },
    { "KERNEL32.dll", "GetCurrentProcess", GetCurrentProcess_ },
    { "KERNEL32.dll", "GetCurrentProcessId", GetCurrentProcessId_ },
    { "KERNEL32.dll", "SetThreadPriority", SetThreadPriority_ },
    { "KERNEL32.dll", "Sleep", Sleep_ },
    { "KERNEL32.dll", "TlsAlloc", TlsAlloc_ },
    { "KERNEL32.dll", "TlsFree", TlsFree_ },
    { "KERNEL32.dll", "TlsGetValue", TlsGetValue_ },
    { "KERNEL32.dll", "TlsSetValue", TlsSetValue_ },
    { "KERNEL32.dll", "FlsAlloc", FlsAlloc_ },
    { "KERNEL32.dll", "FlsFree", FlsFree_ },
    { "KERNEL32.dll", "FlsGetValue", FlsGetValue_ },
    { "KERNEL32.dll", "FlsSetValue", FlsSetValue_ },
    { "KERNEL32.dll", "InitializeSListHead", InitializeSListHead_ },
    { "KERNEL32.dll", "InterlockedPushEntrySList", InterlockedPushEntrySList_ },
    { "KERNEL32.dll", "CreateThreadpoolWork", CreateThreadpoolWork_ },
    { "KERNEL32.dll", "SubmitThreadpoolWork", SubmitThreadpoolWork_ },
    { "KERNEL32.dll", "WaitForThreadpoolWorkCallbacks", WaitForThreadpoolWorkCallbacks_ },
    { "KERNEL32.dll", "CloseThreadpoolWork", CloseThreadpoolWork_ },
    { "KERNEL32.dll", "FreeLibraryWhenCallbackReturns", FreeLibraryWhenCallbackReturns_ },
    { "KERNEL32.dll", "QueryPerformanceCounter", QueryPerformanceCounter_ },
    { "KERNEL32.dll", "QueryPerformanceFrequency", QueryPerformanceFrequency_ },
    { "KERNEL32.dll", "GetTickCount", GetTickCount_ },
    { "KERNEL32.dll", "GetSystemTimeAsFileTime", GetSystemTimeAsFileTime_ },
    { "KERNEL32.dll", "GetSystemTimePreciseAsFileTime", GetSystemTimeAsFileTime_ },
    { "KERNEL32.dll", "GetSystemTime", GetSystemTime_ },
    { "KERNEL32.dll", "FileTimeToSystemTime", FileTimeToSystemTime_ },
    { "KERNEL32.dll", "SystemTimeToFileTime", SystemTimeToFileTime_ },
    { "KERNEL32.dll", "SystemTimeToTzSpecificLocalTime", SystemTimeToTzSpecificLocalTime_ },
    { "KERNEL32.dll", "GetTimeZoneInformation", GetTimeZoneInformation_ },
    { "WINMM.dll", "timeBeginPeriod", timeBeginPeriod_ },
    { "WINMM.dll", "timeEndPeriod", timeEndPeriod_ },
    { 0 }
};

__attribute__((constructor)) static void k32_sync_init(void) { w64_register(k32_sync_exports); }
