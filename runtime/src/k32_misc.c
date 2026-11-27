/*
 * k32_misc.c - kernel32: modules, GetProcAddress, environment, command line,
 * process/system info, code pages and locale, exceptions and unwinding hooks.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <ctype.h>
#include <pthread.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>
#include <sys/uio.h>

int es3_eh_throw(CPU *c, uint64_t ret_pc, uint64_t obj, uint64_t throw_info);
void *w64_local_alloc(size_t n);
void *w64_local_alloc_raw(size_t n);

/* ------------------------------------------------------------------ modules */
/* Lifted modules (modules.c) are real images at their bases. DLLs the runtime implements itself
 * get fake handles; GetProcAddress maps those back to a DLL name. */
static const char *g_hostmods[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "ntdll.dll", "advapi32.dll", "shell32.dll", "ole32.dll",
    "winmm.dll", "imm32.dll", "version.dll", "kernelbase.dll", "oleaut32.dll", "setupapi.dll", "dwmapi.dll",
    "shcore.dll", "shlwapi.dll", "ws2_32.dll", "iphlpapi.dll", "dbghelp.dll", "winhttp.dll", "bcrypt.dll",
    "crypt32.dll", "hid.dll", "d3d11.dll", "dxgi.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
    "combase.dll", "api-ms-win-core-winrt-l1-1-0.dll", "api-ms-win-core-winrt-string-l1-1-0.dll",
    "mono.dll", "monoposixhelper.dll", "steam_api64.dll", "opengl32.dll", "dnsapi.dll",
    "msvcr120.dll", "ovrplugin.dll", "sixense.dll", NULL
};
#define MOD_BASE 0x00007FF700000000ull
#define MOD_STRIDE 0x100000ull

static int mod_index(const char *name)
{
    const char *b = name;
    for (const char *p = name; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    char n[128];
    snprintf(n, sizeof n, "%s", b);
    for (char *p = n; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strchr(n, '.')) strncat(n, ".dll", sizeof n - strlen(n) - 1);
    for (int i = 0; g_hostmods[i]; i++) if (!strcmp(g_hostmods[i], n)) return i;
    return -1;
}
static const char *host_dll(uint64_t h)
{
    if (h >= MOD_BASE && (h - MOD_BASE) % MOD_STRIDE == 0) {
        uint64_t i = (h - MOD_BASE) / MOD_STRIDE;
        if (i < sizeof g_hostmods / sizeof g_hostmods[0] - 1) return g_hostmods[i];
    }
    return NULL;
}
static uint64_t load_library(const char *name)
{
    w64_module_t *m = w64_module_find(name);
    if (m) {
        if (w64_module_attach(m)) { w64_set_last_error(1114 /* ERROR_DLL_INIT_FAILED */); return 0; }
        w64_log("[mod] LoadLibrary %s -> %#llx", name, (unsigned long long)m->base);
        return m->base;
    }
    int i = mod_index(name);
    /* API sets (api-ms-win-core-synch-l1-2-0, ...) are kernel32 for the functions we have */
    const char *bn = name; for (const char *p = name; *p; p++) if (*p == '\\' || *p == '/') bn = p + 1;
    if (i < 0 && (!strncasecmp(bn, "api-ms-win-", 11) || !strncasecmp(bn, "ext-ms-win-", 11))) i = mod_index("kernel32.dll");
    if (w64_trace || i < 0) w64_log("[mod] LoadLibrary %s -> %s", name, i < 0 ? "not available" : "host");
    if (i < 0) { w64_set_last_error(126 /* ERROR_MOD_NOT_FOUND */); return 0; }
    return MOD_BASE + MOD_STRIDE * (uint64_t)i;
}
uint64_t w64_load_library(const char *name) { return load_library(name); }
static void LoadLibraryA_(CPU *c) { RET(load_library(AS(0))); }
static void LoadLibraryW_(CPU *c) { char *u = w16_to_utf8(AW(0), -1); uint64_t h = load_library(u); free(u); RET(h); }
static void LoadLibraryExW_(CPU *c) { char *u = w16_to_utf8(AW(0), -1); uint64_t h = load_library(u); free(u); RET(h); }
static void LoadLibraryExA_(CPU *c) { RET(load_library(AS(0))); }
static void FreeLibrary_(CPU *c) { RET(1); }
static void DisableThreadLibraryCalls_(CPU *c) { w64_module_t *m = w64_module_by_base(A(0)); if (m) m->no_thread_calls = 1; RET(1); }

static uint64_t module_handle(const char *u)
{
    w64_module_t *m = w64_module_find(u);
    if (m) return m->attached || m == &g_mods[0] ? m->base : 0;
    int i = mod_index(u);
    return i >= 0 ? MOD_BASE + MOD_STRIDE * (uint64_t)i : 0;
}
static void GetModuleHandleW_(CPU *c)
{
    if (!A(0)) RET(w64_image_base);
    char *u = w16_to_utf8(AW(0), -1);
    uint64_t h = module_handle(u);
    if (!h) w64_set_last_error(126);
    free(u);
    RET(h);
}
static void GetModuleHandleA_(CPU *c)
{
    if (!A(0)) RET(w64_image_base);
    uint64_t h = module_handle(AS(0));
    if (!h) w64_set_last_error(126);
    RET(h);
}
static void GetModuleHandleExW_(CPU *c)
{
    uint32_t fl = AU(0);
    uint64_t h = w64_image_base;
    if (fl & 4) {                                   /* GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS */
        w64_module_t *m = w64_module_by_va(A(1));
        h = m ? m->base : 0;
    } else if (A(1)) {
        char *u = w16_to_utf8(AW(1), -1);
        h = module_handle(u);
        free(u);
    }
    if (A(2)) wr64(A(2), h);
    if (!h) w64_set_last_error(126);
    RET(h != 0);
}
static void GetModuleHandleExA_(CPU *c)
{
    uint32_t fl = AU(0);
    uint64_t h = w64_image_base;
    if (fl & 4) { w64_module_t *m = w64_module_by_va(A(1)); h = m ? m->base : 0; }
    else if (A(1)) h = module_handle(AS(1));
    if (A(2)) wr64(A(2), h);
    RET(h != 0);
}
/* C:\Game\<file> for a lifted module (the game directory maps to C:\Game), System32 for the rest */
static void module_path(uint64_t h, char *out, size_t cap)
{
    w64_module_t *m = h ? w64_module_by_base(h) : &g_mods[0];
    if (m) {
        snprintf(out, cap, "C:\\Game\\%s", m->spec->file);
        for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
        return;
    }
    const char *d = host_dll(h);
    snprintf(out, cap, "C:\\Windows\\System32\\%s", d ? d : "unknown.dll");
}
static void GetModuleFileNameW_(CPU *c)
{
    uint32_t cap = AU(2);
    char path[600];
    module_path(A(0), path, sizeof path);
    int need = utf8_to_w16(path, NULL, 0);
    if (!cap) RET(0);
    utf8_to_w16(path, (uint16_t *)AP(1), (int)cap);
    if ((uint32_t)need > cap) { ((uint16_t *)AP(1))[cap - 1] = 0; w64_set_last_error(122); RET(cap); }
    RET(need - 1);
}
static void GetModuleFileNameA_(CPU *c)
{
    uint32_t cap = AU(2);
    char path[600];
    module_path(A(0), path, sizeof path);
    size_t n = strlen(path);
    if (!cap) RET(0);
    snprintf((char *)AP(1), cap, "%s", path);
    RET(n + 1 > cap ? cap : n);
}

static void GetProcAddress_(CPU *c)
{
    uint64_t h = A(0), nm = A(1);
    w64_module_t *m = w64_module_by_base(h);
    if (m) {
        uint64_t r = w64_module_export(m, (const char *)(uintptr_t)nm);
        if (w64_trace || !r) w64_log("[mod] GetProcAddress(%s, %s) -> %#llx", m->spec->name,
                                     nm < 0x10000 ? "#ord" : (const char *)(uintptr_t)nm, (unsigned long long)r);
        if (!r) w64_set_last_error(127);
        RET(r);
    }
    if (nm < 0x10000) { w64_log("[mod] GetProcAddress by ordinal %llu", (unsigned long long)nm); RET(0); }
    const char *name = (const char *)(uintptr_t)nm;
    const char *dll = host_dll(h);
    uint64_t r = w64_hostfn_named(dll, name);
    if (!r && dll && (!strcmp(dll, "kernelbase.dll") || !strcmp(dll, "ntdll.dll"))) r = w64_hostfn_named("KERNEL32.dll", name);
    if (w64_trace || !r) w64_log("[mod] GetProcAddress(%s, %s) -> %s", dll ? dll : "?", name, r ? "ok" : "NULL");
    if (!r) w64_set_last_error(127);
    RET(r);
}

/* ------------------------------------------------------------------ environment / command line */
static pthread_mutex_t g_env_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_env[256];
static int g_nenv;
static void env_init(void)
{
    if (g_nenv) return;
    const char *defs[] = {
        "USERPROFILE=C:\\Users\\Player", "APPDATA=C:\\Users\\Player\\AppData\\Roaming",
        "LOCALAPPDATA=C:\\Users\\Player\\AppData\\Local", "TEMP=C:\\Temp", "TMP=C:\\Temp", "USERNAME=Player",
        "COMPUTERNAME=PLUM", "OS=Windows_NT", "SystemRoot=C:\\Windows", "windir=C:\\Windows",
        "PROCESSOR_ARCHITECTURE=AMD64", "NUMBER_OF_PROCESSORS=8", "HOMEDRIVE=C:", "HOMEPATH=\\Users\\Player", NULL
    };
    for (int i = 0; defs[i]; i++) g_env[g_nenv++] = strdup(defs[i]);
    const char *extra = getenv("W64_ENV");        /* "A=1;B=2" */
    if (extra) {
        char *d = strdup(extra), *save = NULL;
        for (char *t = strtok_r(d, ";", &save); t && g_nenv < 255; t = strtok_r(NULL, ";", &save)) g_env[g_nenv++] = strdup(t);
        free(d);
    }
}
static int env_find(const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < g_nenv; i++) if (!strncasecmp(g_env[i], name, n) && g_env[i][n] == '=') return i;
    return -1;
}
const char *w64_getenv(const char *name)
{
    pthread_mutex_lock(&g_env_mu);
    env_init();
    int i = env_find(name);
    const char *r = i >= 0 ? strchr(g_env[i], '=') + 1 : NULL;
    pthread_mutex_unlock(&g_env_mu);
    return r;
}
static void env_set(const char *name, const char *val)
{
    pthread_mutex_lock(&g_env_mu);
    env_init();
    int i = env_find(name);
    if (i >= 0) { free(g_env[i]); g_env[i] = g_env[--g_nenv]; }
    if (val && g_nenv < 255) {
        size_t n = strlen(name) + strlen(val) + 2;
        char *e = malloc(n);
        snprintf(e, n, "%s=%s", name, val);
        g_env[g_nenv++] = e;
    }
    pthread_mutex_unlock(&g_env_mu);
}
static void GetEnvironmentVariableA_(CPU *c)
{
    const char *v = w64_getenv(AS(0));
    if (w64_trace) w64_log("[env] %s -> %s", AS(0), v ? v : "(unset)");
    if (!v) { w64_set_last_error(203 /* ERROR_ENVVAR_NOT_FOUND */); RET(0); }
    size_t n = strlen(v);
    if (n + 1 > AU(2)) RET(n + 1);
    memcpy(AP(1), v, n + 1);
    RET(n);
}
static void SetEnvironmentVariableA_(CPU *c) { env_set(AS(0), A(1) ? AS(1) : NULL); RET(1); }
static void SetEnvironmentVariableW_(CPU *c)
{
    char *n = w16_to_utf8(AW(0), -1), *v = A(1) ? w16_to_utf8(AW(1), -1) : NULL;
    env_set(n, v);
    free(n); free(v);
    RET(1);
}
static void GetEnvironmentStringsW_(CPU *c)
{
    pthread_mutex_lock(&g_env_mu);
    env_init();
    size_t tot = 1;
    for (int i = 0; i < g_nenv; i++) tot += (size_t)utf8_to_w16(g_env[i], NULL, 0);
    uint16_t *b = malloc(tot * 2), *p = b;
    for (int i = 0; i < g_nenv; i++) { int n = utf8_to_w16(g_env[i], p, 1 << 20); p += n; }
    *p = 0;
    pthread_mutex_unlock(&g_env_mu);
    RET((uintptr_t)b);
}
static void FreeEnvironmentStringsW_(CPU *c) { free(AP(0)); RET(1); }

static const char *exe_win = "C:\\Game\\TheForest.exe";
static char *g_cmdline_a;
static uint16_t *g_cmdline_w;
static void cmdline_init(void)
{
    if (g_cmdline_a) return;
    const char *args = getenv("W64_ARGS");
    size_t n = strlen(exe_win) + (args ? strlen(args) : 0) + 8;
    g_cmdline_a = malloc(n);
    snprintf(g_cmdline_a, n, "\"%s\"%s%s", exe_win, args && *args ? " " : "", args ? args : "");
    g_cmdline_w = utf8_to_w16_dup(g_cmdline_a);
}
static void GetCommandLineA_(CPU *c) { cmdline_init(); RET((uintptr_t)g_cmdline_a); }
static void GetCommandLineW_(CPU *c) { cmdline_init(); RET((uintptr_t)g_cmdline_w); }
static void GetStartupInfoW_(CPU *c) { memset(AP(0), 0, 104); wr32(A(0), 104); RETV(); }

/* ------------------------------------------------------------------ process */
void w64_shutdown(int code);
static void ExitProcess_(CPU *c) { w64_log("[proc] ExitProcess(%d)", AI(0)); w64_shutdown(AI(0)); }
static void TerminateProcess_(CPU *c) { w64_log("[proc] TerminateProcess(%d)", AI(1)); w64_shutdown(AI(1)); }
static void GetExitCodeProcess_(CPU *c) { if (A(1)) wr32(A(1), 0); RET(1); }
static void CreateProcessW_(CPU *c) { char *u = A(1) ? w16_to_utf8(AW(1), -1) : strdup(""); w64_log("[proc] CreateProcessW %s (refused)", u); free(u); w64_set_last_error(2); RET(0); }
static void IsDebuggerPresent_(CPU *c) { RET(0); }
static void OutputDebugStringA_(CPU *c) { w64_log("[dbg] %s", AS(0)); RETV(); }
static void OutputDebugStringW_(CPU *c) { char *u = w16_to_utf8(AW(0), -1); w64_log("[dbg] %s", u); free(u); RETV(); }
static void SetErrorMode_(CPU *c) { RET(0); }
static void SetUnhandledExceptionFilter_(CPU *c) { RET(0); }
static void UnhandledExceptionFilter_(CPU *c) { RET(1); }
static void AddVectoredExceptionHandler_(CPU *c) { RET(0x1000); }
static void RemoveVectoredExceptionHandler_(CPU *c) { RET(1); }
static void GetLastError_(CPU *c) { RET(w64_get_last_error()); }
static void SetLastError_(CPU *c) { w64_set_last_error(AU(0)); RETV(); }
static void EncodePointer_(CPU *c) { RET(A(0)); }
static void DecodePointer_(CPU *c) { RET(A(0)); }
static void IsWow64Process_(CPU *c) { if (A(1)) wr32(A(1), 0); RET(1); }
static void SetThreadExecutionState_(CPU *c) { RET(0x80000000u); }
static void GetSystemPowerStatus_(CPU *c) { uint8_t *b = AP(0); memset(b, 0, 12); b[0] = 1; b[1] = 8; b[2] = 100; wr32(A(0) + 4, 0xFFFFFFFFu); wr32(A(0) + 8, 0xFFFFFFFFu); RET(1); }
static void SetHandleInformation_(CPU *c) { RET(1); }
static void CreatePipe_(CPU *c) { w64_set_last_error(5); RET(0); }
static void SetNamedPipeHandleState_(CPU *c) { RET(0); }
static void EnumResourceNamesW_(CPU *c) { RET(0); }

static void sysinfo(uint8_t *b)
{
    memset(b, 0, 48);
    uint16_t arch = 9; memcpy(b, &arch, 2);
    uint32_t page = 4096; memcpy(b + 4, &page, 4);
    uint64_t lo = 0x10000, hi = 0x00007FFFFFFEFFFFull; memcpy(b + 8, &lo, 8); memcpy(b + 16, &hi, 8);
    long n = sysconf(_SC_NPROCESSORS_ONLN); if (n < 1) n = 1; if (n > 32) n = 32;
    uint64_t mask = (n >= 64) ? ~0ull : ((1ull << n) - 1); memcpy(b + 24, &mask, 8);
    uint32_t np = (uint32_t)n; memcpy(b + 32, &np, 4);
    uint32_t pt = 8664; memcpy(b + 36, &pt, 4);
    uint32_t gran = 65536; memcpy(b + 40, &gran, 4);
    uint16_t lvl = 6; memcpy(b + 44, &lvl, 2);
}
static void GetSystemInfo_(CPU *c) { sysinfo(AP(0)); RETV(); }
static void GetNativeSystemInfo_(CPU *c) { sysinfo(AP(0)); RETV(); }
static void GlobalMemoryStatusEx_(CPU *c)
{
    uint8_t *b = AP(0);
    long pages = sysconf(_SC_PHYS_PAGES), avail = sysconf(_SC_AVPHYS_PAGES), ps = sysconf(_SC_PAGESIZE);
    uint64_t tot = (uint64_t)pages * (uint64_t)ps, av = (uint64_t)avail * (uint64_t)ps;
    uint32_t load = tot ? (uint32_t)(100 - av * 100 / tot) : 50;
    memcpy(b + 4, &load, 4);
    memcpy(b + 8, &tot, 8); memcpy(b + 16, &av, 8);
    memcpy(b + 24, &tot, 8); memcpy(b + 32, &av, 8);
    uint64_t tv = 0x7FFFFFFE0000ull; memcpy(b + 40, &tv, 8); memcpy(b + 48, &tv, 8);
    memset(b + 56, 0, 8);
    RET(1);
}
static void IsProcessorFeaturePresent_(CPU *c)
{
    uint32_t f = AU(0);
    /* SSE, SSE2, CMPXCHG16B(14)? no; NX(12); RDTSC(8); fastfail(23) no: int 29h is not lifted */
    RET(f == 6 || f == 10 || f == 8 || f == 12 || f == 3);
}
static void VerSetConditionMask_(CPU *c)
{
    uint64_t m = A(0);
    uint32_t type = AU(1);
    uint8_t cond = (uint8_t)A(2);
    for (int i = 0; i < 8; i++) if (type & (1u << i)) m |= (uint64_t)(cond & 7) << (i * 3);
    RET(m);
}
static void VerifyVersionInfoW_(CPU *c) { RET(1); }
static void MulDiv_(CPU *c)
{
    int32_t a = AI(0), b = AI(1), d = AI(2);
    if (!d) RET((uint32_t)-1);
    int64_t p = (int64_t)a * b;
    int64_t r = (p >= 0) == (d > 0) ? (p + d / 2) / d : (p - d / 2) / d;
    RET((uint32_t)(int32_t)r);
}

/* ------------------------------------------------------------------ code pages */
static uint32_t acp(void) { return 1252; }
static void GetACP_(CPU *c) { RET(acp()); }
static void GetOEMCP_(CPU *c) { RET(437); }
static void IsValidCodePage_(CPU *c) { uint32_t cp = AU(0); RET(cp == 65001 || cp == 1252 || cp == 437 || cp == 20127 || cp == 28591 || cp == 1200); }
static void GetCPInfo_(CPU *c)
{
    uint32_t cp = AU(0);
    uint8_t *b = AP(1);
    memset(b, 0, 20);
    uint32_t mx = cp == 65001 ? 4 : 1;
    memcpy(b, &mx, 4);
    b[4] = '?';
    RET(1);
}

/* MultiByteToWideChar(cp, flags, src, srclen, dst, dstlen) */
static void MultiByteToWideChar_(CPU *c)
{
    uint32_t cp = AU(0);
    const uint8_t *s = AP(2);
    int32_t n = AI(3), cap = AI(5);
    uint16_t *d = AP(4);
    if (n < 0) n = (int32_t)strlen((const char *)s) + 1;
    if (cp == 0 || cp == 1 || cp == 3) cp = acp();
    int out = 0;
    if (cp == 65001) {
        for (int i = 0; i < n; ) {
            uint32_t u = s[i++];
            if (u >= 0xF0 && i + 2 < n + 0) { u = ((u & 7) << 18) | ((s[i] & 63) << 12) | ((s[i + 1] & 63) << 6) | (s[i + 2] & 63); i += 3; }
            else if (u >= 0xE0 && i + 1 < n + 0) { u = ((u & 15) << 12) | ((s[i] & 63) << 6) | (s[i + 1] & 63); i += 2; }
            else if (u >= 0xC0 && i < n) { u = ((u & 31) << 6) | (s[i] & 63); i += 1; }
            else if (u >= 0x80) u = 0xFFFD;
            if (u >= 0x10000) {
                u -= 0x10000;
                if (cap) { if (out + 2 > cap) { w64_set_last_error(122); RET(0); } d[out] = (uint16_t)(0xD800 + (u >> 10)); d[out + 1] = (uint16_t)(0xDC00 + (u & 0x3FF)); }
                out += 2;
            } else {
                if (cap) { if (out + 1 > cap) { w64_set_last_error(122); RET(0); } d[out] = (uint16_t)u; }
                out++;
            }
        }
    } else {
        for (int i = 0; i < n; i++) {
            if (cap) { if (out + 1 > cap) { w64_set_last_error(122); RET(0); } d[out] = s[i]; }
            out++;
        }
    }
    RET(out);
}

/* WideCharToMultiByte(cp, flags, src, srclen, dst, dstlen, defchar, useddef) */
static void WideCharToMultiByte_(CPU *c)
{
    uint32_t cp = AU(0);
    const uint16_t *s = AP(2);
    int32_t n = AI(3), cap = AI(5);
    uint8_t *d = AP(4);
    if (n < 0) n = (int32_t)w16len(s) + 1;
    if (cp == 0 || cp == 1 || cp == 3) cp = acp();
    if (A(7)) wr32(A(7), 0);
    int out = 0;
    for (int i = 0; i < n; i++) {
        uint32_t u = s[i];
        uint8_t tmp[4]; int k;
        if (cp == 65001) {
            if (u >= 0xD800 && u < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
            if (u < 0x80) { tmp[0] = (uint8_t)u; k = 1; }
            else if (u < 0x800) { tmp[0] = (uint8_t)(0xC0 | (u >> 6)); tmp[1] = (uint8_t)(0x80 | (u & 63)); k = 2; }
            else if (u < 0x10000) { tmp[0] = (uint8_t)(0xE0 | (u >> 12)); tmp[1] = (uint8_t)(0x80 | ((u >> 6) & 63)); tmp[2] = (uint8_t)(0x80 | (u & 63)); k = 3; }
            else { tmp[0] = (uint8_t)(0xF0 | (u >> 18)); tmp[1] = (uint8_t)(0x80 | ((u >> 12) & 63)); tmp[2] = (uint8_t)(0x80 | ((u >> 6) & 63)); tmp[3] = (uint8_t)(0x80 | (u & 63)); k = 4; }
        } else {
            if (u > 0xFF) { tmp[0] = '?'; if (A(7)) wr32(A(7), 1); } else tmp[0] = (uint8_t)u;
            k = 1;
        }
        if (cap) { if (out + k > cap) { w64_set_last_error(122); RET(0); } memcpy(d + out, tmp, (size_t)k); }
        out += k;
    }
    RET(out);
}

/* ------------------------------------------------------------------ locale */
static const char *locale_str(uint32_t type)
{
    const char *lang = getenv("W64_LANG");      /* "en", "de", ... */
    if (!lang) lang = "en";
    switch (type & 0xFFFF) {
    case 0x0001: return "0409";                 /* LOCALE_ILANGUAGE */
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";
    case 0x0007: return "United States";
    case 0x000B: return "437";                  /* IDEFAULTCODEPAGE */
    case 0x000C: return ",";                    /* SLIST */
    case 0x000E: return ".";                    /* SDECIMAL */
    case 0x000F: return ",";                    /* STHOUSAND */
    case 0x0010: return "3;0";                  /* SGROUPING */
    case 0x0014: return "$";
    case 0x0016: return ".";
    case 0x0017: return ",";
    case 0x0018: return "3;0";
    case 0x0019: return "2";
    case 0x001D: return "/";
    case 0x001E: return ":";
    case 0x001F: return "M/d/yyyy";
    case 0x0020: return "dddd, MMMM d, yyyy";
    case 0x0028: return "AM";
    case 0x0029: return "PM";
    case 0x0059: return lang;                   /* SISO639LANGNAME */
    case 0x005A: return "US";                   /* SISO3166CTRYNAME */
    case 0x005C: return "en-US";                /* SNAME */
    case 0x1001: return "English";
    case 0x1002: return "United States";
    case 0x1004: return "1252";                 /* IDEFAULTANSICODEPAGE */
    case 0x1009: return "1";
    case 0x1014: return ".";
    }
    return "";
}
static int locale_out(uint32_t type, uint64_t buf, int32_t cap, int wide)
{
    if (type & 0x20000000) {                    /* LOCALE_RETURN_NUMBER */
        const char *s = locale_str(type & ~0x20000000u);
        uint32_t v = (uint32_t)strtoul(s, NULL, (type & 0xFFFF) == 1 ? 16 : 10);
        if (cap >= (wide ? 2 : 4)) wr32(buf, v);
        return wide ? 2 : 4;
    }
    const char *s = locale_str(type);
    int need = wide ? utf8_to_w16(s, NULL, 0) : (int)strlen(s) + 1;
    if (!cap) return need;
    if (need > cap) { w64_set_last_error(122); return 0; }
    if (wide) utf8_to_w16(s, (uint16_t *)(uintptr_t)buf, cap); else memcpy((void *)(uintptr_t)buf, s, (size_t)need);
    return need;
}
static void GetLocaleInfoW_(CPU *c) { RET(locale_out(AU(1), A(2), AI(3), 1)); }
static void GetLocaleInfoA_(CPU *c) { RET(locale_out(AU(1), A(2), AI(3), 0)); }
static void GetLocaleInfoEx_(CPU *c) { RET(locale_out(AU(1), A(2), AI(3), 1)); }
static void GetUserDefaultLCID_(CPU *c) { RET(0x409); }
static void IsValidLocale_(CPU *c) { RET(1); }
static void EnumSystemLocalesW_(CPU *c) { RET(1); }

static int cls_w(uint16_t ch)
{
    int t = 0x200;
    if (ch < 128) {
        if (isupper(ch)) t |= 0x101;
        if (islower(ch)) t |= 0x102;
        if (isdigit(ch)) t |= 0x4;
        if (isspace(ch)) t |= 0x8;
        if (ispunct(ch)) t |= 0x10;
        if (iscntrl(ch)) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit(ch)) t |= 0x80;
    } else if (ch >= 0xC0 && ch != 0xD7 && ch != 0xF7 && ch <= 0x24F) {
        t |= 0x100 | (ch < 0xDF ? 1 : 2);
    } else if (ch == 0xA0) t |= 0x48;
    else if (ch < 0xC0 && ch >= 0x80) t |= ch < 0xA0 ? 0x20 : 0x10;
    else t |= 0x100;
    return t;
}
static void GetStringTypeW_(CPU *c)
{
    const uint16_t *s = AP(1);
    int32_t n = AI(2);
    uint16_t *o = AP(3);
    if (n < 0) n = (int32_t)w16len(s) + 1;
    for (int i = 0; i < n; i++) o[i] = AU(0) == 1 ? (uint16_t)cls_w(s[i]) : 0;
    RET(1);
}
static uint16_t wlower(uint16_t ch) { if (ch < 128) return (uint16_t)tolower(ch); if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) return ch + 32; return (uint16_t)towlower(ch); }
static uint16_t wupper(uint16_t ch) { if (ch < 128) return (uint16_t)toupper(ch); if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) return ch - 32; return (uint16_t)towupper(ch); }
static int lcmap(uint32_t fl, const uint16_t *s, int32_t n, uint64_t dst, int32_t cap)
{
    if (n < 0) n = (int32_t)w16len(s) + 1;
    if (fl & 0x400) {                           /* LCMAP_SORTKEY: bytes */
        int need = n * 2 + 1;
        if (!cap) return need;
        uint8_t *d = (uint8_t *)(uintptr_t)dst;
        int k = 0;
        for (int i = 0; i < n && k + 2 < cap; i++) { uint16_t ch = (fl & 1) ? wlower(s[i]) : s[i]; d[k++] = (uint8_t)(ch >> 8); d[k++] = (uint8_t)ch; }
        d[k++] = 0;
        return k;
    }
    if (!cap) return n;
    if (cap < n) { w64_set_last_error(122); return 0; }
    uint16_t *d = (uint16_t *)(uintptr_t)dst;
    for (int i = 0; i < n; i++) d[i] = (fl & 0x100) ? wlower(s[i]) : (fl & 0x200) ? wupper(s[i]) : s[i];
    return n;
}
static void LCMapStringEx_(CPU *c) { RET(lcmap(AU(1), AP(2), AI(3), A(4), AI(5))); }
static void LCMapStringW_(CPU *c) { RET(lcmap(AU(1), AP(2), AI(3), A(4), AI(5))); }
static int cmpw(uint32_t fl, const uint16_t *a, int32_t na, const uint16_t *b, int32_t nb)
{
    if (na < 0) na = (int32_t)w16len(a);
    if (nb < 0) nb = (int32_t)w16len(b);
    for (int i = 0; i < na && i < nb; i++) {
        uint16_t x = a[i], y = b[i];
        if (fl & 1) { x = wlower(x); y = wlower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}
static void CompareStringEx_(CPU *c) { RET(cmpw(AU(1), AP(2), AI(3), AP(4), AI(5))); }
static void CompareStringW_(CPU *c) { RET(cmpw(AU(1), AP(2), AI(3), AP(4), AI(5))); }
static void CompareStringA_(CPU *c)
{
    const char *a = AS(2), *b = AS(4);
    int32_t na = AI(3), nb = AI(5);
    if (na < 0) na = (int32_t)strlen(a);
    if (nb < 0) nb = (int32_t)strlen(b);
    int r = (AU(1) & 1) ? strncasecmp(a, b, (size_t)(na < nb ? na : nb)) : strncmp(a, b, (size_t)(na < nb ? na : nb));
    if (!r) r = na - nb;
    RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}
static void GetDateFormatW_(CPU *c)
{
    char b[64];
    uint16_t *st = AP(2);
    if (st) snprintf(b, sizeof b, "%u/%u/%u", st[1], st[3], st[0]);
    else { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); snprintf(b, sizeof b, "%d/%d/%d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900); }
    int need = utf8_to_w16(b, NULL, 0);
    if (!AI(5)) RET(need);
    utf8_to_w16(b, (uint16_t *)AP(4), AI(5));
    RET(need);
}
static void GetTimeFormatW_(CPU *c)
{
    char b[64];
    uint16_t *st = AP(2);
    if (st) snprintf(b, sizeof b, "%02u:%02u:%02u", st[4], st[5], st[6]);
    else { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); snprintf(b, sizeof b, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec); }
    int need = utf8_to_w16(b, NULL, 0);
    if (!AI(5)) RET(need);
    utf8_to_w16(b, (uint16_t *)AP(4), AI(5));
    RET(need);
}

/* FormatMessage: a generic text; FORMAT_MESSAGE_ALLOCATE_BUFFER allocates with the heap */
static void format_message(CPU *c, int wide)
{
    uint32_t fl = AU(0), id = AU(2);
    char msg[64];
    snprintf(msg, sizeof msg, "Error %u\r\n", id);
    uint64_t buf = A(4);
    uint32_t cap = AU(5);
    if (fl & 0x100) {
        size_t n = wide ? (size_t)utf8_to_w16(msg, NULL, 0) * 2 : strlen(msg) + 1;
        void *p = w64_local_alloc_raw(n);
        if (wide) utf8_to_w16(msg, p, (int)n / 2); else memcpy(p, msg, n);
        wr64(buf, (uint64_t)(uintptr_t)p);
        RET(wide ? (uint64_t)(n / 2 - 1) : (uint64_t)(n - 1));
    }
    if (wide) { int need = utf8_to_w16(msg, NULL, 0); if ((uint32_t)need > cap) RET(0); utf8_to_w16(msg, (uint16_t *)(uintptr_t)buf, (int)cap); RET(need - 1); }
    if (strlen(msg) + 1 > cap) RET(0);
    memcpy((void *)(uintptr_t)buf, msg, strlen(msg) + 1);
    RET(strlen(msg));
}
static void FormatMessageW_(CPU *c) { format_message(c, 1); }
static void FormatMessageA_(CPU *c) { format_message(c, 0); }

/* ------------------------------------------------------------------ exceptions */
/* What the game throws (IL2CPP raises every C# exception as a C++ throw of an
 * Il2CppExceptionWrapper whose first field is the managed exception): the first 25 throws and
 * then every 500th go to the log with class name and message. Reads are checked
 * (process_vm_readv), a C++ exception of another kind just shows no name. */
static int safe_rd(uint64_t a, void *out, size_t n)
{
    if (a < 0x10000) return 0;
    struct iovec l = { out, n }, r = { (void *)(uintptr_t)a, n };
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
}
static void exc_log(uint64_t obj, uint64_t ret)
{
    static unsigned long n;
    unsigned long k = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (k > 25 && k % 500) return;
    uint64_t ex = 0, klass = 0, np = 0, nsp = 0, msg = 0;
    char name[96] = "?", ns[96] = "", m[160] = "";
    if (safe_rd(obj, &ex, 8) && safe_rd(ex, &klass, 8) && safe_rd(klass + 0x10, &np, 8) && safe_rd(klass + 0x18, &nsp, 8)) {
        if (!safe_rd(np, name, sizeof name - 1)) snprintf(name, sizeof name, "?");
        if (!safe_rd(nsp, ns, sizeof ns - 1)) ns[0] = 0;
        name[sizeof name - 1] = 0; ns[sizeof ns - 1] = 0;
        int32_t len = 0;
        if (safe_rd(ex + 0x18, &msg, 8) && msg && safe_rd(msg + 0x10, &len, 4) && len > 0) {
            uint16_t w[150]; int l = len < 150 ? len : 150;
            if (safe_rd(msg + 0x14, w, (size_t)l * 2)) { for (int i = 0; i < l; i++) m[i] = w[i] < 128 ? (char)w[i] : '?'; m[l < 159 ? l : 159] = 0; }
        }
    }
    w64_log("[exc] throw #%lu %s%s%s from %#llx: %s", k, ns, ns[0] ? "." : "", name, (unsigned long long)ret, m);
}
static void RaiseException_(CPU *c)
{
    uint32_t code = AU(0), nargs = AU(2);
    uint64_t args = A(3);
    uint64_t ret = rd64(c->rsp);
    if (code == 0xE06D7363u && nargs >= 3) {
        uint64_t obj = rd64(args + 8), ti = rd64(args + 16);
        exc_log(obj, ret);
        c->rsp += 8;                              /* consume the return address like a ret */
        if (es3_eh_throw(c, ret, obj, ti)) return; /* never returns when caught */
        c->rsp -= 8;
        w64_fatal(c, "uncaught C++ exception (throw info %#llx)", (unsigned long long)ti);
    }
    if (code == 0x406D1388u) RETV();             /* MS_VC_EXCEPTION: SetThreadName */
    w64_log("[exc] RaiseException %#x flags %#x from %#llx", code, AU(1), (unsigned long long)ret);
    if (AU(1) & 1) w64_fatal(c, "non-continuable exception %#x", code);
    RETV();
}
static void RtlCaptureContext_(CPU *c)
{
    uint8_t *ctx = AP(0);
    memset(ctx, 0, 0x4D0);
    uint64_t *g = (uint64_t *)(ctx + 0x78);       /* Rax.. in CONTEXT order: rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    uint64_t regs[16] = { c->rax, c->rcx, c->rdx, c->rbx, c->rsp + 8, c->rbp, c->rsi, c->rdi,
                          c->r8, c->r9, c->r10, c->r11, c->r12, c->r13, c->r14, c->r15 };
    memcpy(g, regs, sizeof regs);
    uint64_t rip = rd64(c->rsp);
    memcpy(ctx + 0xF8, &rip, 8);
    uint32_t flags = 0x10000B; memcpy(ctx + 0x30, &flags, 4);
    RETV();
}
static void RtlLookupFunctionEntry_(CPU *c)
{
    uint64_t pc = A(0);
    if (A(1)) wr64(A(1), 0);
    w64_module_t *mod = w64_module_by_va(pc);
    if (!mod || !mod->pdata_rva) RET(0);
    uint64_t tab = mod->base + mod->pdata_rva;
    uint32_t n = mod->pdata_size / 12, rva = (uint32_t)(pc - mod->base);
    int lo = 0, hi = (int)n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        uint64_t e = tab + 12 * (uint64_t)m;
        if (rva < rd32(e)) hi = m - 1;
        else if (rva >= rd32(e + 4)) lo = m + 1;
        else { if (A(1)) wr64(A(1), mod->base); RET(e); }
    }
    RET(0);
}
static void RtlVirtualUnwind_(CPU *c) { w64_log("[exc] RtlVirtualUnwind (stack walk) - not supported"); RET(0); }
static void RtlUnwind_(CPU *c) { w64_fatal(c, "RtlUnwind"); }
static void RtlUnwindEx_(CPU *c) { w64_fatal(c, "RtlUnwindEx"); }
static void RtlPcToFileHeader_(CPU *c)
{
    uint64_t pc = A(0);
    w64_module_t *mod = w64_module_by_va(pc);
    uint64_t b = mod ? mod->base : 0;
    if (A(1)) wr64(A(1), b);
    RET(b);
}
static void RtlCaptureStackBackTrace_(CPU *c) { RET(0); }

static const w64_export_t k32_misc_exports[] = {
    { "KERNEL32.dll", "LoadLibraryA", LoadLibraryA_ },
    { "KERNEL32.dll", "LoadLibraryW", LoadLibraryW_ },
    { "KERNEL32.dll", "LoadLibraryExW", LoadLibraryExW_ },
    { "KERNEL32.dll", "FreeLibrary", FreeLibrary_ },
    { "KERNEL32.dll", "GetModuleHandleW", GetModuleHandleW_ },
    { "KERNEL32.dll", "GetModuleHandleExW", GetModuleHandleExW_ },
    { "KERNEL32.dll", "GetModuleHandleA", GetModuleHandleA_ },
    { "KERNEL32.dll", "GetModuleHandleExA", GetModuleHandleExA_ },
    { "KERNEL32.dll", "LoadLibraryExA", LoadLibraryExA_ },
    { "KERNEL32.dll", "DisableThreadLibraryCalls", DisableThreadLibraryCalls_ },
    { "KERNEL32.dll", "GetModuleFileNameW", GetModuleFileNameW_ },
    { "KERNEL32.dll", "GetModuleFileNameA", GetModuleFileNameA_ },
    { "KERNEL32.dll", "GetProcAddress", GetProcAddress_ },
    { "KERNEL32.dll", "GetEnvironmentVariableA", GetEnvironmentVariableA_ },
    { "KERNEL32.dll", "SetEnvironmentVariableA", SetEnvironmentVariableA_ },
    { "KERNEL32.dll", "SetEnvironmentVariableW", SetEnvironmentVariableW_ },
    { "KERNEL32.dll", "GetEnvironmentStringsW", GetEnvironmentStringsW_ },
    { "KERNEL32.dll", "FreeEnvironmentStringsW", FreeEnvironmentStringsW_ },
    { "KERNEL32.dll", "GetCommandLineA", GetCommandLineA_ },
    { "KERNEL32.dll", "GetCommandLineW", GetCommandLineW_ },
    { "KERNEL32.dll", "GetStartupInfoW", GetStartupInfoW_ },
    { "KERNEL32.dll", "ExitProcess", ExitProcess_ },
    { "KERNEL32.dll", "TerminateProcess", TerminateProcess_ },
    { "KERNEL32.dll", "GetExitCodeProcess", GetExitCodeProcess_ },
    { "KERNEL32.dll", "CreateProcessW", CreateProcessW_ },
    { "KERNEL32.dll", "IsDebuggerPresent", IsDebuggerPresent_ },
    { "KERNEL32.dll", "OutputDebugStringA", OutputDebugStringA_ },
    { "KERNEL32.dll", "OutputDebugStringW", OutputDebugStringW_ },
    { "KERNEL32.dll", "SetErrorMode", SetErrorMode_ },
    { "KERNEL32.dll", "SetUnhandledExceptionFilter", SetUnhandledExceptionFilter_ },
    { "KERNEL32.dll", "UnhandledExceptionFilter", UnhandledExceptionFilter_ },
    { "KERNEL32.dll", "AddVectoredExceptionHandler", AddVectoredExceptionHandler_ },
    { "KERNEL32.dll", "RemoveVectoredExceptionHandler", RemoveVectoredExceptionHandler_ },
    { "KERNEL32.dll", "GetLastError", GetLastError_ },
    { "KERNEL32.dll", "SetLastError", SetLastError_ },
    { "KERNEL32.dll", "EncodePointer", EncodePointer_ },
    { "KERNEL32.dll", "DecodePointer", DecodePointer_ },
    { "KERNEL32.dll", "IsWow64Process", IsWow64Process_ },
    { "KERNEL32.dll", "SetThreadExecutionState", SetThreadExecutionState_ },
    { "KERNEL32.dll", "GetSystemPowerStatus", GetSystemPowerStatus_ },
    { "KERNEL32.dll", "SetHandleInformation", SetHandleInformation_ },
    { "KERNEL32.dll", "CreatePipe", CreatePipe_ },
    { "KERNEL32.dll", "SetNamedPipeHandleState", SetNamedPipeHandleState_ },
    { "KERNEL32.dll", "EnumResourceNamesW", EnumResourceNamesW_ },
    { "KERNEL32.dll", "GetSystemInfo", GetSystemInfo_ },
    { "KERNEL32.dll", "GetNativeSystemInfo", GetNativeSystemInfo_ },
    { "KERNEL32.dll", "GlobalMemoryStatusEx", GlobalMemoryStatusEx_ },
    { "KERNEL32.dll", "IsProcessorFeaturePresent", IsProcessorFeaturePresent_ },
    { "KERNEL32.dll", "VerSetConditionMask", VerSetConditionMask_ },
    { "KERNEL32.dll", "VerifyVersionInfoW", VerifyVersionInfoW_ },
    { "KERNEL32.dll", "MulDiv", MulDiv_ },
    { "KERNEL32.dll", "GetACP", GetACP_ },
    { "KERNEL32.dll", "GetOEMCP", GetOEMCP_ },
    { "KERNEL32.dll", "IsValidCodePage", IsValidCodePage_ },
    { "KERNEL32.dll", "GetCPInfo", GetCPInfo_ },
    { "KERNEL32.dll", "MultiByteToWideChar", MultiByteToWideChar_ },
    { "KERNEL32.dll", "WideCharToMultiByte", WideCharToMultiByte_ },
    { "KERNEL32.dll", "GetLocaleInfoW", GetLocaleInfoW_ },
    { "KERNEL32.dll", "GetLocaleInfoA", GetLocaleInfoA_ },
    { "KERNEL32.dll", "GetLocaleInfoEx", GetLocaleInfoEx_ },
    { "KERNEL32.dll", "GetUserDefaultLCID", GetUserDefaultLCID_ },
    { "KERNEL32.dll", "IsValidLocale", IsValidLocale_ },
    { "KERNEL32.dll", "EnumSystemLocalesW", EnumSystemLocalesW_ },
    { "KERNEL32.dll", "GetStringTypeW", GetStringTypeW_ },
    { "KERNEL32.dll", "LCMapStringEx", LCMapStringEx_ },
    { "KERNEL32.dll", "LCMapStringW", LCMapStringW_ },
    { "KERNEL32.dll", "CompareStringEx", CompareStringEx_ },
    { "KERNEL32.dll", "CompareStringW", CompareStringW_ },
    { "KERNEL32.dll", "CompareStringA", CompareStringA_ },
    { "KERNEL32.dll", "GetDateFormatW", GetDateFormatW_ },
    { "KERNEL32.dll", "GetTimeFormatW", GetTimeFormatW_ },
    { "KERNEL32.dll", "FormatMessageW", FormatMessageW_ },
    { "KERNEL32.dll", "FormatMessageA", FormatMessageA_ },
    { "KERNEL32.dll", "RaiseException", RaiseException_ },
    { "KERNEL32.dll", "RtlCaptureContext", RtlCaptureContext_ },
    { "KERNEL32.dll", "RtlLookupFunctionEntry", RtlLookupFunctionEntry_ },
    { "KERNEL32.dll", "RtlVirtualUnwind", RtlVirtualUnwind_ },
    { "KERNEL32.dll", "RtlUnwind", RtlUnwind_ },
    { "KERNEL32.dll", "RtlUnwindEx", RtlUnwindEx_ },
    { "KERNEL32.dll", "RtlPcToFileHeader", RtlPcToFileHeader_ },
    { "KERNEL32.dll", "RtlCaptureStackBackTrace", RtlCaptureStackBackTrace_ },
    { 0 }
};

__attribute__((constructor)) static void k32_misc_init(void) { w64_register(k32_misc_exports); }
