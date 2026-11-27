/*
 * win_extra.c - the rest of the Win32 surface Unity 2022 + IL2CPP reach outside windowing/graphics:
 * Winsock (no network: every socket call fails cleanly, byte-order helpers work), crypto-grade
 * random numbers, the COM basics, known folders, a small file-backed registry (Unity keeps
 * PlayerPrefs in HKCU\Software\<company>\<product>), and kernel32 queries.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <ctype.h>
#include <pthread.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void fill_random(void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = getrandom(b, n, 0);
        if (r <= 0) { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rand(); return; }
        b += r; n -= (size_t)r;
    }
}

/* ------------------------------------------------------------------ Winsock */
static __thread int t_wsa_err;
#define WSAENETDOWN 10050
#define SOCKET_ERROR 0xFFFFFFFFull
static void wsa_fail(CPU *c) { t_wsa_err = WSAENETDOWN; RET(SOCKET_ERROR); }
static void WSAStartup_(CPU *c)
{
    uint8_t *d = AP(1);
    if (d) {
        memset(d, 0, 408);
        uint16_t v = 0x0202; memcpy(d, &v, 2); memcpy(d + 2, &v, 2);
        strcpy((char *)d + 16, "WinSock 2.0");      /* x64 WSADATA: szDescription after the shorts/ptr */
    }
    RET(0);
}
static void WSACleanup_(CPU *c) { RET(0); }
static void socket_(CPU *c) { t_wsa_err = WSAENETDOWN; RET(~0ull); }       /* INVALID_SOCKET */
static void WSAGetLastError_(CPU *c) { RET((uint32_t)t_wsa_err); }
static void WSASetLastError_(CPU *c) { t_wsa_err = AI(0); RETV(); }
static void htonl_(CPU *c) { RET(__builtin_bswap32(AU(0))); }
static void htons_(CPU *c) { RET(__builtin_bswap16((uint16_t)A(0))); }
static void closesocket_(CPU *c) { RET(0); }
static void gethostname_(CPU *c) { if (A(0) && AI(1) > 8) strcpy((char *)AP(0), "android"); RET(0); }
static void getaddrinfo_(CPU *c) { if (A(3)) wr64(A(3), 0); RET(11001); }   /* WSAHOST_NOT_FOUND */
static void freeaddrinfo_(CPU *c) { RETV(); }
static void inet_addr_(CPU *c) { RET(0xFFFFFFFFu); }
static void WSAFDIsSet_(CPU *c) { RET(0); }
static void WSACreateEvent_(CPU *c) { RET(0); }

/* ------------------------------------------------------------------ random numbers */
static void CryptAcquireContextW_(CPU *c) { if (A(0)) wr64(A(0), 0xC0FFEE); RET(1); }
static void CryptReleaseContext_(CPU *c) { RET(1); }
static void CryptGenRandom_(CPU *c) { fill_random(AP(2), AU(1)); RET(1); }
static void RtlGenRandom_(CPU *c) { fill_random(AP(0), AU(1)); RET(1); }          /* SystemFunction036 */
static void BCryptGenRandom_(CPU *c) { fill_random(AP(1), AU(2)); RET(0); }       /* STATUS_SUCCESS */

/* ------------------------------------------------------------------ COM basics */
static void CoInitialize_(CPU *c) { RET(0); }
static void CoUninitialize_(CPU *c) { RETV(); }
static void CoTaskMemAlloc_(CPU *c) { RET((uintptr_t)malloc(A(0) ? A(0) : 1)); }
static void CoTaskMemFree_(CPU *c) { free(AP(0)); RETV(); }
static void CoCreateGuid_(CPU *c)
{
    uint8_t *g = AP(0);
    fill_random(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);         /* version 4 (Data3 high nibble, little endian) */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
    RET(0);
}
static void StringFromGUID2_(CPU *c)
{
    const uint8_t *g = AP(0);
    uint32_t d1; uint16_t d2, d3; memcpy(&d1, g, 4); memcpy(&d2, g + 4, 2); memcpy(&d3, g + 6, 2);
    char s[64];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3,
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    if (AI(2) < 39) RET(0);
    utf8_to_w16(s, (uint16_t *)AP(1), AI(2));
    RET(39);
}
static void PropVariantClear_(CPU *c) { if (A(0)) memset(AP(0), 0, 24); RET(0); }
static void CoNotImpl_(CPU *c) { RET(0x80004001u); }                               /* E_NOTIMPL */

/* ------------------------------------------------------------------ known folders */
static void SHGetKnownFolderPath_(CPU *c)
{
    const uint8_t *g = AP(0);
    uint32_t d1; memcpy(&d1, g, 4);
    const char *p = "C:\\Users\\Player";
    switch (d1) {
    case 0xA520A1A4: p = "C:\\Users\\Player\\AppData\\LocalLow"; break;       /* LocalAppDataLow */
    case 0xF1B32785: p = "C:\\Users\\Player\\AppData\\Local"; break;          /* LocalAppData */
    case 0x3EB685DB: p = "C:\\Users\\Player\\AppData\\Roaming"; break;        /* RoamingAppData */
    case 0xFDD39AD0: p = "C:\\Users\\Player\\Documents"; break;               /* Documents */
    case 0xB4BFCC3A: p = "C:\\Users\\Player\\Desktop"; break;                 /* Desktop */
    case 0x4C5C32FF: p = "C:\\Users\\Player\\Saved Games"; break;             /* SavedGames */
    case 0x33E28130: p = "C:\\Users\\Player\\Pictures"; break;                /* Pictures */
    case 0x18989B1D: p = "C:\\Users\\Player\\Videos"; break;                  /* Videos */
    case 0x4BD8D571: p = "C:\\Users\\Player\\Music"; break;                   /* Music */
    case 0x374DE290: p = "C:\\Users\\Player\\Downloads"; break;               /* Downloads */
    default: w64_log("[shell] SHGetKnownFolderPath %08X -> profile", d1);
    }
    char hp[1200]; w64_host_path(p, hp, sizeof hp);
    for (char *q = hp + strlen(w64_userdir); *q; q++) if (*q == '/') { *q = 0; mkdir(hp, 0777); *q = '/'; }
    mkdir(hp, 0777);
    int n = utf8_to_w16(p, NULL, 0);
    uint16_t *w = malloc((size_t)n * 2);
    utf8_to_w16(p, w, n);
    wr64(A(3), (uint64_t)(uintptr_t)w);
    RET(0);
}

/* ------------------------------------------------------------------ registry */
/* One flat table "key\value" -> (type, data), saved to <userdir>/registry.dat after every change. */
typedef struct { char *key; char *name; uint32_t type, size; uint8_t *data; } regval_t;
static regval_t *g_reg; static int g_nreg, g_capreg;
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { char path[512]; } regkey_t;

static void reg_path(char *out) { snprintf(out, 1100, "%s/registry.dat", w64_userdir); }
static void reg_save(void)
{
    char p[1100]; reg_path(p);
    char t[1110]; snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb");
    if (!f) return;
    for (int i = 0; i < g_nreg; i++) {
        uint32_t kl = (uint32_t)strlen(g_reg[i].key), nl = (uint32_t)strlen(g_reg[i].name);
        fwrite(&kl, 4, 1, f); fwrite(g_reg[i].key, 1, kl, f);
        fwrite(&nl, 4, 1, f); fwrite(g_reg[i].name, 1, nl, f);
        fwrite(&g_reg[i].type, 4, 1, f); fwrite(&g_reg[i].size, 4, 1, f); fwrite(g_reg[i].data, 1, g_reg[i].size, f);
    }
    fclose(f);
    rename(t, p);
}
static void reg_load(void)
{
    static int done;
    if (done) return;
    done = 1;
    char p[1100]; reg_path(p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    for (;;) {
        uint32_t kl, nl, ty, sz;
        if (fread(&kl, 4, 1, f) != 1 || kl > 4096) break;
        char *k = calloc(1, kl + 1); if (fread(k, 1, kl, f) != kl) break;
        if (fread(&nl, 4, 1, f) != 1 || nl > 4096) break;
        char *n = calloc(1, nl + 1); if (fread(n, 1, nl, f) != nl) break;
        if (fread(&ty, 4, 1, f) != 1 || fread(&sz, 4, 1, f) != 1 || sz > (64u << 20)) break;
        uint8_t *d = malloc(sz ? sz : 1); if (fread(d, 1, sz, f) != sz) break;
        if (g_nreg == g_capreg) { g_capreg = g_capreg ? g_capreg * 2 : 64; g_reg = realloc(g_reg, sizeof *g_reg * (size_t)g_capreg); }
        g_reg[g_nreg++] = (regval_t){ k, n, ty, sz, d };
    }
    fclose(f);
    w64_log("[reg] %d values from %s", g_nreg, p);
}
static const char *root_name(uint64_t h)
{
    switch ((uint32_t)h) {
    case 0x80000000u: return "HKCR"; case 0x80000001u: return "HKCU"; case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    }
    return NULL;
}
/* full path of (hkey, subkey), lower-cased for comparisons */
static int key_path(uint64_t h, const char *sub, char *out)
{
    const char *r = root_name(h);
    if (r) snprintf(out, 512, "%s", r);
    else { regkey_t *k = w64_handle_get(h, H_OTHER); if (!k) return -1; snprintf(out, 512, "%s", k->path); }
    if (sub && *sub) { size_t l = strlen(out); snprintf(out + l, 512 - l, "\\%s", sub); }
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    size_t l = strlen(out); while (l && out[l - 1] == '\\') out[--l] = 0;
    return 0;
}
static uint64_t key_open(const char *path) { regkey_t *k = calloc(1, sizeof *k); snprintf(k->path, sizeof k->path, "%s", path); return w64_handle_new(H_OTHER, k); }
static int key_exists(const char *path)
{
    size_t l = strlen(path);
    for (int i = 0; i < g_nreg; i++) if (!strncmp(g_reg[i].key, path, l) && (!g_reg[i].key[l] || g_reg[i].key[l] == '\\')) return 1;
    return 0;
}
static regval_t *val_find(const char *key, const char *name)
{
    for (int i = 0; i < g_nreg; i++) if (!strcmp(g_reg[i].key, key) && !strcasecmp(g_reg[i].name, name)) return &g_reg[i];
    return NULL;
}
static char *wname(uint64_t p) { return p ? w16_to_utf8((const uint16_t *)(uintptr_t)p, -1) : strdup(""); }

static void reg_create(CPU *c, int wide, uint64_t h, uint64_t sub, uint64_t out, uint64_t disp)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    char *s = wide ? wname(sub) : strdup(sub ? (const char *)(uintptr_t)sub : "");
    char path[512];
    if (key_path(h, s, path)) { free(s); pthread_mutex_unlock(&g_reg_mu); RET(6); }   /* ERROR_INVALID_HANDLE */
    if (disp) wr32(disp, key_exists(path) ? 2 : 1);
    wr64(out, key_open(path));
    free(s);
    pthread_mutex_unlock(&g_reg_mu);
    RET(0);
}
static void RegCreateKeyExW_(CPU *c) { reg_create(c, 1, A(0), A(1), w64_arg(c, 7), w64_arg(c, 8)); }
static void RegCreateKeyExA_(CPU *c) { reg_create(c, 0, A(0), A(1), w64_arg(c, 7), w64_arg(c, 8)); }
static void RegCreateKeyW_(CPU *c) { reg_create(c, 1, A(0), A(1), A(2), 0); }
static void reg_open(CPU *c, int wide, uint64_t h, uint64_t sub, uint64_t out)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    char *s = wide ? wname(sub) : strdup(sub ? (const char *)(uintptr_t)sub : "");
    char path[512];
    int bad = key_path(h, s, path);
    free(s);
    /* HKCU\Software\... opens even when empty (Unity reads PlayerPrefs before it ever wrote one) */
    if (bad || (!key_exists(path) && strncmp(path, "hkcu\\software", 13))) { if (out) wr64(out, 0); pthread_mutex_unlock(&g_reg_mu); RET(2); }
    if (out) wr64(out, key_open(path));
    pthread_mutex_unlock(&g_reg_mu);
    RET(0);
}
static void RegOpenKeyExW_(CPU *c) { reg_open(c, 1, A(0), A(1), A(4)); }
static void RegOpenKeyExA_(CPU *c) { reg_open(c, 0, A(0), A(1), A(4)); }
static void RegCloseKey_(CPU *c) { if (!root_name(A(0))) w64_handle_close(A(0)); RET(0); }
static void reg_query(CPU *c, int wide)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    char path[512];
    if (key_path(A(0), NULL, path)) { pthread_mutex_unlock(&g_reg_mu); RET(6); }
    char *n = wide ? wname(A(1)) : strdup(A(1) ? AS(1) : "");
    regval_t *v = val_find(path, n);
    free(n);
    uint64_t ty = A(3), data = w64_arg(c, 4), cb = w64_arg(c, 5);
    if (!v) { pthread_mutex_unlock(&g_reg_mu); RET(2); }                         /* ERROR_FILE_NOT_FOUND */
    if (ty) wr32(ty, v->type);
    uint32_t have = cb ? rd32(cb) : 0;
    if (cb) wr32(cb, v->size);
    if (data && have < v->size) { pthread_mutex_unlock(&g_reg_mu); RET(234); }    /* ERROR_MORE_DATA */
    if (data) memcpy((void *)(uintptr_t)data, v->data, v->size);
    pthread_mutex_unlock(&g_reg_mu);
    RET(0);
}
static void RegQueryValueExW_(CPU *c) { reg_query(c, 1); }
static void RegQueryValueExA_(CPU *c) { reg_query(c, 0); }
static void reg_set(CPU *c, int wide)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    char path[512];
    if (key_path(A(0), NULL, path)) { pthread_mutex_unlock(&g_reg_mu); RET(6); }
    char *n = wide ? wname(A(1)) : strdup(A(1) ? AS(1) : "");
    uint32_t ty = AU(3), sz = (uint32_t)w64_arg(c, 5);
    const uint8_t *d = (const uint8_t *)(uintptr_t)w64_arg(c, 4);
    regval_t *v = val_find(path, n);
    if (!v) {
        if (g_nreg == g_capreg) { g_capreg = g_capreg ? g_capreg * 2 : 64; g_reg = realloc(g_reg, sizeof *g_reg * (size_t)g_capreg); }
        v = &g_reg[g_nreg++];
        v->key = strdup(path); v->name = n; v->data = NULL;
    } else free(n);
    free(v->data);
    v->type = ty; v->size = sz; v->data = malloc(sz ? sz : 1);
    if (sz && d) memcpy(v->data, d, sz);
    reg_save();
    pthread_mutex_unlock(&g_reg_mu);
    RET(0);
}
/* A value the game finds when it has not stored one itself (platform defaults, e.g. graphics
 * settings for phones): set only if missing. key in the stored form (lower case, "hkcu\\..."). */
void w64_reg_default(const char *key, const char *name, uint32_t type, const void *data, uint32_t size)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    if (!val_find(key, name)) {
        if (g_nreg == g_capreg) { g_capreg = g_capreg ? g_capreg * 2 : 64; g_reg = realloc(g_reg, sizeof *g_reg * (size_t)g_capreg); }
        regval_t *v = &g_reg[g_nreg++];
        v->key = strdup(key); v->name = strdup(name); v->type = type; v->size = size;
        v->data = malloc(size ? size : 1); memcpy(v->data, data, size);
        reg_save();
        w64_log("[reg] default %s = %u bytes", name, size);
    }
    pthread_mutex_unlock(&g_reg_mu);
}
/* W64_REG_SET="name=int,name=int" (debug): overrides DWORD values of the game's key at start */
void w64_reg_force(const char *key, const char *name, uint32_t type, const void *data, uint32_t size)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    regval_t *v = val_find(key, name);
    if (!v) {
        if (g_nreg == g_capreg) { g_capreg = g_capreg ? g_capreg * 2 : 64; g_reg = realloc(g_reg, sizeof *g_reg * (size_t)g_capreg); }
        v = &g_reg[g_nreg++]; v->key = strdup(key); v->name = strdup(name); v->data = NULL;
    }
    free(v->data);
    v->type = type; v->size = size; v->data = malloc(size ? size : 1); memcpy(v->data, data, size);
    reg_save();
    pthread_mutex_unlock(&g_reg_mu);
    w64_log("[reg] forced %s", name);
}
static void RegSetValueExW_(CPU *c) { reg_set(c, 1); }
static void RegSetValueExA_(CPU *c) { reg_set(c, 0); }
static void reg_delete_value(CPU *c, int wide)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    char path[512];
    if (key_path(A(0), NULL, path)) { pthread_mutex_unlock(&g_reg_mu); RET(6); }
    char *n = wide ? wname(A(1)) : strdup(A(1) ? AS(1) : "");
    regval_t *v = val_find(path, n);
    free(n);
    if (!v) { pthread_mutex_unlock(&g_reg_mu); RET(2); }
    free(v->key); free(v->name); free(v->data);
    *v = g_reg[--g_nreg];
    reg_save();
    pthread_mutex_unlock(&g_reg_mu);
    RET(0);
}
static void RegDeleteValueW_(CPU *c) { reg_delete_value(c, 1); }
static void RegDeleteValueA_(CPU *c) { reg_delete_value(c, 0); }
/* (hkey, index, name, cchName*, reserved, type*, data, cbData*) */
static void RegEnumValueW_(CPU *c)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    char path[512];
    if (key_path(A(0), NULL, path)) { pthread_mutex_unlock(&g_reg_mu); RET(6); }
    uint32_t want = AU(1), k = 0;
    for (int i = 0; i < g_nreg; i++) {
        if (strcmp(g_reg[i].key, path)) continue;
        if (k++ != want) continue;
        regval_t *v = &g_reg[i];
        int need = utf8_to_w16(v->name, NULL, 0);
        uint32_t cap = rd32(A(3));
        if ((uint32_t)need > cap) { pthread_mutex_unlock(&g_reg_mu); RET(234); }
        utf8_to_w16(v->name, (uint16_t *)AP(2), (int)cap);
        wr32(A(3), (uint32_t)need - 1);
        if (w64_arg(c, 5)) wr32(w64_arg(c, 5), v->type);
        uint64_t cb = w64_arg(c, 7), data = w64_arg(c, 6);
        if (cb) { uint32_t have = rd32(cb); wr32(cb, v->size); if (data && have < v->size) { pthread_mutex_unlock(&g_reg_mu); RET(234); } }
        if (data) memcpy((void *)(uintptr_t)data, v->data, v->size);
        pthread_mutex_unlock(&g_reg_mu);
        RET(0);
    }
    pthread_mutex_unlock(&g_reg_mu);
    RET(259);                                                                    /* ERROR_NO_MORE_ITEMS */
}
static void reg_delete_tree(CPU *c, int wide, uint64_t h, uint64_t sub)
{
    pthread_mutex_lock(&g_reg_mu); reg_load();
    char *s = wide ? wname(sub) : strdup(sub ? (const char *)(uintptr_t)sub : "");
    char path[512];
    if (key_path(h, s, path)) { free(s); pthread_mutex_unlock(&g_reg_mu); RET(6); }
    free(s);
    size_t l = strlen(path);
    for (int i = 0; i < g_nreg;) {
        if (!strncmp(g_reg[i].key, path, l) && (!g_reg[i].key[l] || g_reg[i].key[l] == '\\')) {
            free(g_reg[i].key); free(g_reg[i].name); free(g_reg[i].data); g_reg[i] = g_reg[--g_nreg];
        } else i++;
    }
    reg_save();
    pthread_mutex_unlock(&g_reg_mu);
    RET(0);
}
static void SHDeleteKeyW_(CPU *c) { reg_delete_tree(c, 1, A(0), A(1)); }
static void RegDeleteKeyW_(CPU *c) { reg_delete_tree(c, 1, A(0), A(1)); }
static void RegDeleteTreeW_(CPU *c) { reg_delete_tree(c, 1, A(0), A(1)); }

/* ------------------------------------------------------------------ process token */
/* A normal user: not elevated, medium integrity (S-1-16-8192). */
static uint8_t g_sid_medium[12] = { 1, 1, 0, 0, 0, 0, 0, 16, 0x00, 0x20, 0, 0 };
static uint8_t g_sid_user[28] = { 1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 0xE9, 3, 0, 0 };
static void OpenProcessToken_(CPU *c) { if (A(2)) wr64(A(2), 0x70C0ull); RET(1); }
/* GetTokenInformation(token, class, buf, len, retlen) */
static void GetTokenInformation_(CPU *c)
{
    uint32_t cls = AU(1), len = AU(3);
    uint64_t buf = A(2), rl = w64_arg(c, 4);
    uint32_t need;
    switch (cls) {
    case 20: need = 4; if (rl) wr32(rl, need); if (len < need || !buf) { w64_set_last_error(122); RET(0); } wr32(buf, 0); RET(1);  /* TokenElevation */
    case 18: need = 4; if (rl) wr32(rl, need); if (len < need || !buf) { w64_set_last_error(122); RET(0); } wr32(buf, 1); RET(1);  /* TokenElevationType: default */
    case 25: case 1:                                       /* TokenIntegrityLevel / TokenUser: SID_AND_ATTRIBUTES */
        need = 16; if (rl) wr32(rl, need); if (len < need || !buf) { w64_set_last_error(122); RET(0); }
        wr64(buf, (uint64_t)(uintptr_t)(cls == 25 ? g_sid_medium : g_sid_user)); wr32(buf + 8, cls == 25 ? 0x20 : 0); RET(1);
    }
    w64_set_last_error(87); RET(0);
}
static void GetSidSubAuthority_(CPU *c) { RET(A(0) + 8 + 4 * (uint64_t)AU(1)); }
static void GetSidSubAuthorityCount_(CPU *c) { RET(A(0) + 1); }

/* ------------------------------------------------------------------ kernel32 queries */
static int ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 && n < 64 ? (int)n : 4; }
/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION: one RelationProcessorCore entry (32 bytes) per core */
static void GetLogicalProcessorInformation_(CPU *c)
{
    int n = ncpu();
    uint32_t need = (uint32_t)n * 32, have = rd32(A(1));
    wr32(A(1), need);
    if (!A(0) || have < need) { w64_set_last_error(122); RET(0); }
    uint8_t *b = AP(0);
    memset(b, 0, need);
    for (int i = 0; i < n; i++) { uint64_t m = 1ull << i; memcpy(b + 32 * i, &m, 8); }   /* Relationship 0 = core */
    RET(1);
}
/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX: cores (48 B), package (48 B), group (80 B) */
static void GetLogicalProcessorInformationEx_(CPU *c)
{
    uint32_t rel = AU(0);
    int n = ncpu();
    int want_core = rel == 0 || rel == 0xFFFF, want_pkg = rel == 3 || rel == 0xFFFF, want_grp = rel == 4 || rel == 0xFFFF;
    uint32_t need = (uint32_t)((want_core ? n : 0) + (want_pkg ? 1 : 0)) * 48u + (want_grp ? 80u : 0u), have = rd32(A(2));
    wr32(A(2), need);
    if (!A(1) || have < need) { w64_set_last_error(122); RET(0); }
    uint8_t *b = AP(1);
    memset(b, 0, need);
    uint8_t *p = b;
    for (int i = 0; want_core && i < n; i++, p += 48) {
        uint32_t r = 0, sz = 48; uint16_t gc = 1; uint64_t m = 1ull << i;
        memcpy(p, &r, 4); memcpy(p + 4, &sz, 4); memcpy(p + 8 + 22, &gc, 2); memcpy(p + 8 + 24, &m, 8);
    }
    if (want_pkg) {
        uint32_t r = 3, sz = 48; uint16_t gc = 1; uint64_t m = (n >= 64 ? ~0ull : (1ull << n) - 1);
        memcpy(p, &r, 4); memcpy(p + 4, &sz, 4); memcpy(p + 8 + 22, &gc, 2); memcpy(p + 8 + 24, &m, 8);
        p += 48;
    }
    if (want_grp) {
        uint32_t r = 4, sz = 80; uint16_t g1 = 1; uint8_t nb = (uint8_t)n;
        uint64_t m = (n >= 64 ? ~0ull : (1ull << n) - 1);
        memcpy(p, &r, 4); memcpy(p + 4, &sz, 4);
        memcpy(p + 8, &g1, 2); memcpy(p + 10, &g1, 2);
        p[0x20] = nb; p[0x21] = nb;
        memcpy(p + 0x48, &m, 8);
        p += 80;
    }
    RET(1);
}
static void GetProcessAffinityMask_(CPU *c)
{
    int n = ncpu(); uint64_t m = (1ull << n) - 1;
    if (A(1)) wr64(A(1), m);
    if (A(2)) wr64(A(2), m);
    RET(1);
}
static void GetSystemTimes_(CPU *c) { for (int i = 0; i < 3; i++) if (A(i)) wr64(A(i), 0); RET(1); }
static void GetThreadTimes_(CPU *c) { for (int i = 1; i < 5; i++) if (w64_arg(c, i)) wr64(w64_arg(c, i), 0); RET(1); }
static void GetThreadPriority_(CPU *c) { RET(0); }
static void GetProcessId_(CPU *c) { RET(0x100); }
static void GetVersionExW_(CPU *c)
{
    uint8_t *v = AP(0);
    uint32_t sz = rd32(A(0));
    memset(v + 4, 0, sz > 4 ? sz - 4 : 0);
    uint32_t maj = 10, min = 0, build = 19045, plat = 2;
    memcpy(v + 4, &maj, 4); memcpy(v + 8, &min, 4); memcpy(v + 12, &build, 4); memcpy(v + 16, &plat, 4);
    RET(1);
}
static void GetUserDefaultLocaleName_(CPU *c) { if (AI(1) < 6) RET(0); utf8_to_w16("en-US", (uint16_t *)AP(0), AI(1)); RET(6); }
static void GetThreadLocale_(CPU *c) { RET(0x409); }
static void GetDynamicTimeZoneInformation_(CPU *c) { memset(AP(0), 0, 432); RET(0); }
static void GetLocalTime_(CPU *c)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    uint16_t st[8] = { (uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday,
                       (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, (uint16_t)(ts.tv_nsec / 1000000) };
    memcpy(AP(0), st, 16);
    RETV();
}
static void wstr_ret(CPU *c, const char *s, uint64_t buf, uint32_t cap)
{
    int need = utf8_to_w16(s, NULL, 0);
    if (!buf || (uint32_t)need > cap) RET((uint32_t)need);
    utf8_to_w16(s, (uint16_t *)(uintptr_t)buf, (int)cap);
    RET((uint32_t)need - 1);
}
static void GetWindowsDirectoryW_(CPU *c) { wstr_ret(c, "C:\\Windows", A(0), AU(1)); }
static void GetSystemDirectoryW_(CPU *c) { wstr_ret(c, "C:\\Windows\\System32", A(0), AU(1)); }
static void GetSystemDirectoryA_(CPU *c)
{
    const char *s = "C:\\Windows\\System32";
    if (!A(0) || AU(1) <= strlen(s)) RET(strlen(s) + 1);
    strcpy((char *)AP(0), s); RET(strlen(s));
}
static void GetComputerNameW_(CPU *c)
{
    uint32_t cap = rd32(A(1));
    if (cap < 8) { wr32(A(1), 8); w64_set_last_error(111); RET(0); }
    utf8_to_w16("ANDROID", (uint16_t *)AP(0), (int)cap); wr32(A(1), 7); RET(1);
}
static void GetUserNameW_(CPU *c)
{
    uint32_t cap = rd32(A(1));
    if (cap < 7) { wr32(A(1), 7); w64_set_last_error(122); RET(0); }
    utf8_to_w16("Player", (uint16_t *)AP(0), (int)cap); wr32(A(1), 7); RET(1);
}
static void GetUserNameA_(CPU *c)
{
    uint32_t cap = rd32(A(1));
    if (cap < 7) { wr32(A(1), 7); w64_set_last_error(122); RET(0); }
    strcpy((char *)AP(0), "Player"); wr32(A(1), 7); RET(1);
}
static void ExpandEnvironmentStringsW_(CPU *c)
{
    char *s = w16_to_utf8(AW(0), -1);
    int need = utf8_to_w16(s, NULL, 0);
    if (A(1) && (uint32_t)need <= AU(2)) utf8_to_w16(s, (uint16_t *)AP(1), AI(2));
    free(s);
    RET((uint32_t)need);
}
static void GetLongPathNameW_(CPU *c)
{
    size_t n = w16len(AW(0)) + 1;
    if (!A(1) || AU(2) < n) RET(n);
    memmove(AP(1), AP(0), n * 2);
    RET(n - 1);
}
static void GetDiskFreeSpaceExW_(CPU *c)
{
    uint64_t big = 64ull << 30;
    for (int i = 1; i < 4; i++) if (w64_arg(c, i)) wr64(w64_arg(c, i), big);
    RET(1);
}
static void GetDriveTypeW_(CPU *c) { RET(3); }                       /* DRIVE_FIXED */
static void GetErrorMode_(CPU *c) { RET(0); }
static void GetConsoleCP_(CPU *c) { RET(65001); }
static void DebugBreak_(CPU *c) { w64_log("[k32] DebugBreak from %#llx (ignored)", (unsigned long long)rd64(c->rsp)); RETV(); }
static void HeapQueryInformation_(CPU *c) { RET(0); }
static void GetStartupInfoA_(CPU *c) { memset(AP(0), 0, 104); wr32(A(0), 104); RETV(); }
static void GetNumaHighestNodeNumber_(CPU *c) { if (A(0)) wr32(A(0), 0); RET(1); }

static const w64_export_t extra_exports[] = {
    { "WS2_32.dll", "#115", WSAStartup_ }, { "WS2_32.dll", "WSAStartup", WSAStartup_ },
    { "WS2_32.dll", "#116", WSACleanup_ }, { "WS2_32.dll", "WSACleanup", WSACleanup_ },
    { "WS2_32.dll", "#23", socket_ }, { "WS2_32.dll", "socket", socket_ },
    { "WS2_32.dll", "WSASocketW", socket_ }, { "WS2_32.dll", "WSASocketA", socket_ },
    { "WS2_32.dll", "#111", WSAGetLastError_ }, { "WS2_32.dll", "WSAGetLastError", WSAGetLastError_ },
    { "WS2_32.dll", "#112", WSASetLastError_ }, { "WS2_32.dll", "WSASetLastError", WSASetLastError_ },
    { "WS2_32.dll", "#8", htonl_ }, { "WS2_32.dll", "#14", htonl_ }, { "WS2_32.dll", "htonl", htonl_ }, { "WS2_32.dll", "ntohl", htonl_ },
    { "WS2_32.dll", "#9", htons_ }, { "WS2_32.dll", "#15", htons_ }, { "WS2_32.dll", "htons", htons_ }, { "WS2_32.dll", "ntohs", htons_ },
    { "WS2_32.dll", "#3", closesocket_ }, { "WS2_32.dll", "closesocket", closesocket_ },
    { "WS2_32.dll", "#57", gethostname_ }, { "WS2_32.dll", "gethostname", gethostname_ },
    { "WS2_32.dll", "getaddrinfo", getaddrinfo_ }, { "WS2_32.dll", "GetAddrInfoW", getaddrinfo_ },
    { "WS2_32.dll", "freeaddrinfo", freeaddrinfo_ }, { "WS2_32.dll", "FreeAddrInfoW", freeaddrinfo_ },
    { "WS2_32.dll", "#11", inet_addr_ }, { "WS2_32.dll", "#151", WSAFDIsSet_ },
    { "WS2_32.dll", "WSACreateEvent", WSACreateEvent_ },
    /* everything else that could pretend to work: SOCKET_ERROR + WSAENETDOWN */
    { "WS2_32.dll", "#1", wsa_fail }, { "WS2_32.dll", "#2", wsa_fail }, { "WS2_32.dll", "#4", wsa_fail },
    { "WS2_32.dll", "#5", wsa_fail }, { "WS2_32.dll", "#6", wsa_fail }, { "WS2_32.dll", "#7", wsa_fail },
    { "WS2_32.dll", "#10", wsa_fail }, { "WS2_32.dll", "#13", wsa_fail }, { "WS2_32.dll", "#16", wsa_fail },
    { "WS2_32.dll", "#17", wsa_fail }, { "WS2_32.dll", "#18", wsa_fail }, { "WS2_32.dll", "#19", wsa_fail },
    { "WS2_32.dll", "#20", wsa_fail }, { "WS2_32.dll", "#21", wsa_fail }, { "WS2_32.dll", "#22", wsa_fail },
    { "WS2_32.dll", "WSAIoctl", wsa_fail }, { "WS2_32.dll", "WSAPoll", wsa_fail }, { "WS2_32.dll", "WSARecv", wsa_fail },
    { "WS2_32.dll", "WSASend", wsa_fail }, { "WS2_32.dll", "WSAEventSelect", wsa_fail },
    { "WS2_32.dll", "WSAEnumNetworkEvents", wsa_fail }, { "WS2_32.dll", "WSASendDisconnect", wsa_fail },
    { "ADVAPI32.dll", "CryptAcquireContextW", CryptAcquireContextW_ },
    { "ADVAPI32.dll", "CryptReleaseContext", CryptReleaseContext_ },
    { "ADVAPI32.dll", "CryptGenRandom", CryptGenRandom_ },
    { "ADVAPI32.dll", "SystemFunction036", RtlGenRandom_ },
    { "bcrypt.dll", "BCryptGenRandom", BCryptGenRandom_ },
    { "ole32.dll", "CoInitialize", CoInitialize_ }, { "ole32.dll", "CoUninitialize", CoUninitialize_ },
    { "ole32.dll", "CoTaskMemAlloc", CoTaskMemAlloc_ }, { "ole32.dll", "CoTaskMemFree", CoTaskMemFree_ },
    { "ole32.dll", "CoCreateGuid", CoCreateGuid_ }, { "ole32.dll", "StringFromGUID2", StringFromGUID2_ },
    { "ole32.dll", "PropVariantClear", PropVariantClear_ },
    { "ole32.dll", "CoCreateFreeThreadedMarshaler", CoNotImpl_ }, { "ole32.dll", "CoGetContextToken", CoNotImpl_ },
    { "ole32.dll", "CoSetProxyBlanket", CoNotImpl_ },
    { "SHELL32.dll", "SHGetKnownFolderPath", SHGetKnownFolderPath_ },
    { "ADVAPI32.dll", "RegCreateKeyExW", RegCreateKeyExW_ }, { "ADVAPI32.dll", "RegCreateKeyExA", RegCreateKeyExA_ },
    { "ADVAPI32.dll", "RegCreateKeyW", RegCreateKeyW_ },
    { "ADVAPI32.dll", "RegOpenKeyExW", RegOpenKeyExW_ }, { "ADVAPI32.dll", "RegOpenKeyExA", RegOpenKeyExA_ },
    { "ADVAPI32.dll", "RegCloseKey", RegCloseKey_ },
    { "ADVAPI32.dll", "RegQueryValueExW", RegQueryValueExW_ }, { "ADVAPI32.dll", "RegQueryValueExA", RegQueryValueExA_ },
    { "ADVAPI32.dll", "RegSetValueExW", RegSetValueExW_ }, { "ADVAPI32.dll", "RegSetValueExA", RegSetValueExA_ },
    { "ADVAPI32.dll", "RegDeleteValueW", RegDeleteValueW_ }, { "ADVAPI32.dll", "RegDeleteValueA", RegDeleteValueA_ },
    { "ADVAPI32.dll", "RegEnumValueW", RegEnumValueW_ },
    { "ADVAPI32.dll", "RegDeleteKeyW", RegDeleteKeyW_ }, { "ADVAPI32.dll", "RegDeleteTreeW", RegDeleteTreeW_ },
    { "SHLWAPI.dll", "SHDeleteKeyW", SHDeleteKeyW_ },
    { "ADVAPI32.dll", "GetUserNameW", GetUserNameW_ },
    { "ADVAPI32.dll", "OpenProcessToken", OpenProcessToken_ },
    { "ADVAPI32.dll", "GetTokenInformation", GetTokenInformation_ },
    { "ADVAPI32.dll", "GetSidSubAuthority", GetSidSubAuthority_ },
    { "ADVAPI32.dll", "GetSidSubAuthorityCount", GetSidSubAuthorityCount_ }, { "ADVAPI32.dll", "GetUserNameA", GetUserNameA_ },
    { "KERNEL32.dll", "GetLogicalProcessorInformation", GetLogicalProcessorInformation_ },
    { "KERNEL32.dll", "GetLogicalProcessorInformationEx", GetLogicalProcessorInformationEx_ },
    { "KERNEL32.dll", "GetProcessAffinityMask", GetProcessAffinityMask_ },
    { "KERNEL32.dll", "GetSystemTimes", GetSystemTimes_ },
    { "KERNEL32.dll", "GetThreadTimes", GetThreadTimes_ },
    { "KERNEL32.dll", "GetThreadPriority", GetThreadPriority_ },
    { "KERNEL32.dll", "GetProcessId", GetProcessId_ },
    { "KERNEL32.dll", "GetVersionExW", GetVersionExW_ },
    { "KERNEL32.dll", "GetUserDefaultLocaleName", GetUserDefaultLocaleName_ },
    { "KERNEL32.dll", "GetThreadLocale", GetThreadLocale_ },
    { "KERNEL32.dll", "GetDynamicTimeZoneInformation", GetDynamicTimeZoneInformation_ },
    { "KERNEL32.dll", "GetLocalTime", GetLocalTime_ },
    { "KERNEL32.dll", "GetWindowsDirectoryW", GetWindowsDirectoryW_ },
    { "KERNEL32.dll", "GetSystemDirectoryW", GetSystemDirectoryW_ },
    { "KERNEL32.dll", "GetSystemDirectoryA", GetSystemDirectoryA_ },
    { "KERNEL32.dll", "GetComputerNameW", GetComputerNameW_ },
    { "KERNEL32.dll", "ExpandEnvironmentStringsW", ExpandEnvironmentStringsW_ },
    { "KERNEL32.dll", "GetLongPathNameW", GetLongPathNameW_ },
    { "KERNEL32.dll", "GetDiskFreeSpaceExW", GetDiskFreeSpaceExW_ },
    { "KERNEL32.dll", "GetDriveTypeW", GetDriveTypeW_ },
    { "KERNEL32.dll", "GetErrorMode", GetErrorMode_ },
    { "KERNEL32.dll", "GetConsoleCP", GetConsoleCP_ },
    { "KERNEL32.dll", "DebugBreak", DebugBreak_ },
    { "KERNEL32.dll", "HeapQueryInformation", HeapQueryInformation_ },
    { "KERNEL32.dll", "GetStartupInfoA", GetStartupInfoA_ },
    { "KERNEL32.dll", "GetNumaHighestNodeNumber", GetNumaHighestNodeNumber_ },
    { 0 }
};
/* registered before w32_stubs.c's placeholders so these win in w64_find */
__attribute__((constructor(102))) static void win_extra_init(void) { w64_register(extra_exports); }
