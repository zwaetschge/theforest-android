/*
 * memreport.c - W64_MEMMAP=<seconds>: where the process memory is.
 *
 * Every <seconds> the runtime reads /proc/self/smaps and logs the largest mappings by resident +
 * swapped size (an app's smaps can't be read from adb shell on a release build). Together with
 * W64_ALLOCLOG (VirtualAlloc regions with their guest callers) this says which allocator of the
 * game holds the memory.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <pthread.h>
#include <unistd.h>

typedef struct { unsigned long lo, hi; long kb; char name[64]; } mapsz_t;

static void report(void)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return;
    enum { TOP = 14 };
    mapsz_t top[TOP] = { 0 }, cur = { 0 };
    long tot_anon = 0, tot_file = 0, tot_gpu = 0;
    char line[512];
    int have = 0;
    #define FLUSH() do { if (have) { \
            int anon = !cur.name[0] || cur.name[0] == '['; \
            if (strstr(cur.name, "kgsl")) tot_gpu += cur.kb; else if (anon) tot_anon += cur.kb; else tot_file += cur.kb; \
            int k = TOP - 1; \
            if (cur.kb > top[k].kb) { while (k > 0 && cur.kb > top[k - 1].kb) { top[k] = top[k - 1]; k--; } top[k] = cur; } } } while (0)
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8];
        int off = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &off) >= 3 && strchr(line, '-') < strchr(line, ' ')) {
            FLUSH();
            have = 1;
            cur.lo = lo; cur.hi = hi; cur.kb = 0; cur.name[0] = 0;
            if (off > 0) { char *n = line + off; n[strcspn(n, "\n")] = 0; snprintf(cur.name, sizeof cur.name, "%s", n); }
            continue;
        }
        long v;
        if (sscanf(line, "Rss: %ld kB", &v) == 1) cur.kb += v;
        else if (sscanf(line, "Swap: %ld kB", &v) == 1) cur.kb += v;
    }
    FLUSH();
    #undef FLUSH
    fclose(f);
    w64_log("[mem] rss+swap: anon %ld MB, files %ld MB, kgsl %ld MB", tot_anon >> 10, tot_file >> 10, tot_gpu >> 10);
    for (int i = 0; i < TOP && top[i].kb; i++)
        w64_log("[mem]   %5ld MB  %lx-%lx (%lu MB) %s", top[i].kb >> 10, top[i].lo, top[i].hi, (top[i].hi - top[i].lo) >> 20, top[i].name);
}

__attribute__((weak)) void hp_reslog_report(void) {}
void w64_user32_post_input(uint32_t msg, uint64_t wp, uint64_t lp);
/* <user dir>/quit appearing (adb shell touch) closes the game window: Unity quits normally and
 * logs its own allocator statistics ("Memory Statistics:") */
static void quit_trigger(void)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/quit", w64_userdir);
    if (access(p, F_OK)) return;
    unlink(p);
    w64_log("[mem] quit requested: WM_CLOSE");
    w64_user32_post_input(0x0010 /* WM_CLOSE */, 0, 0);
}
/* W64_PEEK="<global va>+<offset>:<f|i>,...": each report logs the value at *(<global>) + offset
 * (a pointer global of the game, e.g. Unity's TimeManager) as float or int */
static void peek(void)
{
    const char *e = getenv("W64_PEEK");
    if (!e) return;
    char buf[512], out[2048];
    snprintf(buf, sizeof buf, "%s", e);
    int o = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long long g = 0, off = 0; char ty = 'f';
        if (sscanf(tok, "%llx+%llx:%c", &g, &off, &ty) < 2) continue;
        uint64_t base = 0; if (g) memcpy(&base, (void *)(uintptr_t)g, 8);
        if (!base) { o += snprintf(out + o, sizeof out - (size_t)o, " %s=null", tok); continue; }
        if (ty == 'd') {              /* "<g>+<off>:d<n>": n doubles */
            int n = atoi(strchr(tok, ':') + 2); if (n < 1) n = 1; if (n > 24) n = 24;
            o += snprintf(out + o, sizeof out - (size_t)o, " %llx:", off);
            for (int k = 0; k < n && o < (int)sizeof out - 40; k++) { double v; memcpy(&v, (void *)(uintptr_t)(base + off + 8 * k), 8); o += snprintf(out + o, sizeof out - (size_t)o, " %.6g", v); }
            continue;
        }
        if (ty == 'i') { int32_t v; memcpy(&v, (void *)(uintptr_t)(base + off), 4); o += snprintf(out + o, sizeof out - (size_t)o, " %s=%d", tok, v); }
        else { float v; memcpy(&v, (void *)(uintptr_t)(base + off), 4); o += snprintf(out + o, sizeof out - (size_t)o, " %s=%g", tok, v); }
        if (o > (int)sizeof out - 80) break;
    }
    w64_log("[peek]%s", out);
}
static void *memreport_thread(void *arg)
{
    int secs = (int)(intptr_t)arg;
    for (int t = 0;; t++) {
        sleep(1);
        quit_trigger();
        if (getenv("W64_PEEK") && t % 2 == 1) peek();
        if (t % secs == secs - 1) { report(); if (getenv("HP_RESLOG")) hp_reslog_report(); }
    }
    return NULL;
}

void w64_memreport_start(void)
{
    const char *e = getenv("W64_MEMMAP");
    int secs = e ? atoi(e) : 0;
    if (secs <= 0) return;
    pthread_t t;
    if (!pthread_create(&t, NULL, memreport_thread, (void *)(intptr_t)secs)) pthread_detach(t);
}
