/*
 * w32_stubs.c - user32, gdi32, imm32, shell32, ole32, oleaut32, advapi32,
 * setupapi, version.
 *
 * Nearly all of these imports belong to SDL's Win32 backend, which never runs
 * (SDL is replaced at the dynapi table). What the game itself calls gets a
 * real answer below; everything else logs its name once and returns 0.
 */
#include "w64.h"
#include <pthread.h>

const char *w64_current_hostfn(void);

static void quiet0(CPU *c)
{
    static const char *seen[512];
    static int n;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    const char *nm = w64_current_hostfn();
    int dup = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) if (seen[i] == nm) { dup = 1; break; }
    if (!dup && n < 512) seen[n++] = nm;
    pthread_mutex_unlock(&mu);
    if (!dup) w64_log("[w32] %s -> 0 (stub)", nm);
    RET(0);
}

static void MessageBoxA_(CPU *c)
{
    w64_log("[msgbox] %s: %s", A(2) ? AS(2) : "", A(1) ? AS(1) : "");
    RET(1);   /* IDOK */
}
static void SHGetFolderPathW_(CPU *c)
{
    int csidl = AI(1) & 0xFF;
    const char *p = csidl == 0x1a ? "C:\\Users\\Player\\AppData\\Roaming" :
                    csidl == 0x1c ? "C:\\Users\\Player\\AppData\\Local" :
                    csidl == 0x05 ? "C:\\Users\\Player\\Documents" :
                    csidl == 0x28 ? "C:\\Users\\Player" : "C:\\Users\\Player\\AppData\\Roaming";
    utf8_to_w16(p, (uint16_t *)AP(4), 260);
    w64_log("[w32] SHGetFolderPathW(%#x) -> %s", csidl, p);
    RET(0);
}
static void ShellExecuteA_(CPU *c) { w64_log("[w32] ShellExecuteA %s %s (ignored)", A(1) ? AS(1) : "", A(2) ? AS(2) : ""); RET(42); }
static void ShellExecuteW_(CPU *c)
{
    char *a = A(1) ? w16_to_utf8(AW(1), -1) : strdup(""), *b = A(2) ? w16_to_utf8(AW(2), -1) : strdup("");
    w64_log("[w32] ShellExecuteW %s %s (ignored)", a, b);
    free(a); free(b);
    RET(42);
}
/* Windows rules: whitespace separates, "..." groups, 2n backslashes + quote -> n backslashes and a
 * quote toggle, 2n+1 -> n backslashes and a literal quote. One LocalAlloc block like the real one:
 * the caller LocalFree()s it. */
void *w64_local_alloc(size_t n);
static void CommandLineToArgvW_(CPU *c)
{
    const uint16_t *s = AW(0);
    size_t len = s ? w16len(s) : 0;
    uint16_t *buf = malloc((len + 2) * 2);
    size_t *starts = malloc((len + 2) * sizeof *starts);
    int argc = 0; size_t o = 0, i = 0;
    while (s && i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= len) break;
        starts[argc++] = o;
        int inq = 0;
        while (i < len && (inq || (s[i] != ' ' && s[i] != '\t'))) {
            if (s[i] == '\\') {
                size_t n = 0; while (i < len && s[i] == '\\') { n++; i++; }
                if (i < len && s[i] == '"') {
                    for (size_t k = 0; k < n / 2; k++) buf[o++] = '\\';
                    if (n & 1) { buf[o++] = '"'; i++; }
                } else for (size_t k = 0; k < n; k++) buf[o++] = '\\';
                continue;
            }
            if (s[i] == '"') { inq = !inq; i++; continue; }
            buf[o++] = s[i++];
        }
        buf[o++] = 0;
    }
    size_t tab = (size_t)(argc + 1) * 8;
    uint8_t *blk = w64_local_alloc(tab + o * 2 + 2);
    uint64_t *v = (uint64_t *)blk;
    uint16_t *str = (uint16_t *)(blk + tab);
    memcpy(str, buf, o * 2);
    for (int k = 0; k < argc; k++) v[k] = (uint64_t)(uintptr_t)(str + starts[k]);
    v[argc] = 0;
    free(buf); free(starts);
    if (A(1)) wr32(A(1), (uint32_t)argc);
    RET((uintptr_t)blk);
}
static void CoInitializeEx_(CPU *c) { RET(0); }
static void GetFileVersionInfoSizeA_(CPU *c) { RET(0); }

static const w64_export_t named[] = {
    { "USER32.dll", "MessageBoxA", MessageBoxA_ },
    { "SHELL32.dll", "SHGetFolderPathW", SHGetFolderPathW_ },
    { "SHELL32.dll", "ShellExecuteA", ShellExecuteA_ },
    { "SHELL32.dll", "ShellExecuteW", ShellExecuteW_ },
    { "SHELL32.dll", "CommandLineToArgvW", CommandLineToArgvW_ },
    { "ole32.dll", "CoInitializeEx", CoInitializeEx_ },
    { "ole32.dll", "OleInitialize", CoInitializeEx_ },
    { "VERSION.dll", "GetFileVersionInfoSizeA", GetFileVersionInfoSizeA_ },
    { 0 }
};

__attribute__((constructor)) static void w32_stubs_init(void) { w64_register(named); }
