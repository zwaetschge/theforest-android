/*
 * win_more.c - the long tail of Win32 imports Unity/IL2CPP reach: BSTR/VARIANT (oleaut32, used by
 * IL2CPP string marshaling), kernel32 odds and ends, and the libraries that have no counterpart on
 * Android (wave audio devices, HID, SetupAPI, IME, WinHTTP, certificates, adapters) - those report
 * "nothing there" in the way their callers handle.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <strings.h>
#include <time.h>
#include <unistd.h>

void *w64_local_alloc(size_t n);
const char *w64_getenv(const char *name);

/* ------------------------------------------------------------------ version / DPI */
/* shell32/comctl32 DllGetVersion: Unity refuses to start below shell 5. Report 6.0.19041. */
static void DllGetVersion_(CPU *c)
{
    if (!A(0) || rd32(A(0)) < 20) RET(0x80070057u);
    uint32_t v[4] = { 6, 0, 19041, 2 /* DLLVER_PLATFORM_NT */ };
    memcpy((uint8_t *)AP(0) + 4, v, 16);
    RET(0);
}
/* RTL_OSVERSIONINFOW: Windows 10 22H2 */
static void RtlGetVersion_(CPU *c)
{
    uint8_t *v = AP(0);
    uint32_t sz = rd32(A(0));
    memset(v + 4, 0, sz > 4 ? sz - 4 : 0);
    uint32_t maj = 10, min = 0, build = 19045, plat = 2;
    memcpy(v + 4, &maj, 4); memcpy(v + 8, &min, 4); memcpy(v + 12, &build, 4); memcpy(v + 16, &plat, 4);
    RET(0);
}
static void RtlVerifyVersionInfo_(CPU *c) { RET(0); }   /* STATUS_SUCCESS: we satisfy any minimum */
static void GetDpiForMonitor_(CPU *c)
{
    static int logged;
    if (!logged) { logged = 1; w64_log("[user32] GetDpiForMonitor(type %u) -> 96", AU(1)); }
    if (A(2)) wr32(A(2), 96);
    if (A(3)) wr32(A(3), 96);
    RET(0);
}
static void GetDpiForWindow_(CPU *c) { RET(96); }
static void AppPolicyGetProcessTerminationMethod_(CPU *c) { if (A(1)) wr32(A(1), 0); RET(0); }
static void AppPolicyGetThreadInitializationType_(CPU *c) { if (A(1)) wr32(A(1), 0); RET(0); }
static void SetProcessDpiAwareness_(CPU *c) { RET(0); }
static void Ok1b_(CPU *c) { RET(1); }

/* ------------------------------------------------------------------ BSTR / VARIANT */
/* A BSTR points at the characters; the byte length (excluding the NUL) is the 4 bytes before. */
static uint64_t bstr_alloc(const uint16_t *src, uint32_t nchars)
{
    uint8_t *b = malloc(4 + (size_t)nchars * 2 + 2);
    uint32_t bytes = nchars * 2;
    memcpy(b, &bytes, 4);
    if (src) memcpy(b + 4, src, bytes); else memset(b + 4, 0, bytes);
    b[4 + bytes] = b[5 + bytes] = 0;
    return (uint64_t)(uintptr_t)(b + 4);
}
static void SysAllocString_(CPU *c) { if (!A(0)) RET(0); RET(bstr_alloc(AW(0), (uint32_t)w16len(AW(0)))); }
static void SysAllocStringLen_(CPU *c) { RET(bstr_alloc(A(0) ? AW(0) : NULL, AU(1))); }
static void SysFreeString_(CPU *c) { if (A(0)) free((uint8_t *)AP(0) - 4); RETV(); }
static void SysStringLen_(CPU *c) { RET(A(0) ? rd32(A(0) - 4) / 2 : 0); }
static void VariantInit_(CPU *c) { memset(AP(0), 0, 24); RETV(); }
static void VariantClear_(CPU *c) { if (rd16(A(0)) == 8 /* VT_BSTR */ && rd64(A(0) + 8)) free((uint8_t *)(uintptr_t)rd64(A(0) + 8) - 4); memset(AP(0), 0, 24); RET(0); }
static void OleNotImpl_(CPU *c) { RET(0x80004001u); }
static void OleNull_(CPU *c) { RET(0); }

/* ------------------------------------------------------------------ kernel32 */
static void GetEnvironmentVariableW_(CPU *c)
{
    char *n = w16_to_utf8(AW(0), -1);
    const char *v = w64_getenv(n);
    free(n);
    if (!v) { w64_set_last_error(203); RET(0); }
    int need = utf8_to_w16(v, NULL, 0);
    if (!A(1) || (uint32_t)need > AU(2)) RET((uint32_t)need);
    utf8_to_w16(v, (uint16_t *)AP(1), AI(2));
    RET((uint32_t)need - 1);
}
static void K32GetModuleBaseNameW_(CPU *c)                /* (process, module, buf, cch) */
{
    w64_module_t *m = A(1) ? w64_module_by_base(A(1)) : &g_mods[0];
    const char *n = m ? m->spec->name : "unknown.dll";
    if (!AU(3)) RET(0);
    utf8_to_w16(n, (uint16_t *)AP(2), AI(3));
    RET((uint32_t)strlen(n) < AU(3) ? (uint32_t)strlen(n) : AU(3) - 1);
}
/* PROCESS_MEMORY_COUNTERS: cb, PageFaultCount, PeakWorkingSetSize 8, WorkingSetSize 16, ... PagefileUsage 56 */
static void K32GetProcessMemoryInfo_(CPU *c)
{
    uint8_t *p = AP(1);
    uint32_t cb = AU(2);
    memset(p + 4, 0, cb > 4 ? cb - 4 : 0);
    long pages = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { long a, b; if (fscanf(f, "%ld %ld", &a, &b) == 2) pages = b; fclose(f); }
    uint64_t rss = (uint64_t)pages * 4096;
    if (cb >= 24) { memcpy(p + 8, &rss, 8); memcpy(p + 16, &rss, 8); }
    if (cb >= 72) { memcpy(p + 56, &rss, 8); memcpy(p + 64, &rss, 8); }
    if (cb >= 80) memcpy(p + 72, &rss, 8);                /* PrivateUsage (..._EX) */
    RET(1);
}
static void ReadProcessMemory_(CPU *c)                    /* own process only: a copy */
{
    if (A(1) && A(2)) memcpy(AP(2), AP(1), (size_t)A(3));
    if (w64_arg(c, 4)) wr64(w64_arg(c, 4), A(3));
    RET(1);
}
static void FailInvalidHandle_(CPU *c) { w64_set_last_error(6); RET((uint64_t)-1); }
static void Fail0_(CPU *c) { w64_set_last_error(50); RET(0); }
static void Ok1_(CPU *c) { RET(1); }
static void CopyFileTime_(CPU *c) { if (A(0) && A(1)) memcpy(AP(1), AP(0), 8); RET(1); }   /* local == UTC here */
static void TzLocalToSystem_(CPU *c) { if (A(1) && A(2)) memcpy(AP(2), AP(1), 16); RET(1); }

/* ------------------------------------------------------------------ libraries with nothing behind them */
static void timeGetTime_(CPU *c) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); RET((uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000)); }
static void ret0(CPU *c) { RET(0); }
static void mmsys_err(CPU *c) { RET(2); }                 /* MMSYSERR_BADDEVICEID */
/* GetDeviceCaps: a 96-DPI display of the screen size (physical size in mm to match, so a DPI
 * computed from HORZSIZE comes out as 96 too) */
int w64_user32_mode(int i, int *w, int *h);
static void GetDeviceCaps_(CPU *c)
{
    int i = AI(1), w = 1920, h = 1080, n = 0;
    while (w64_user32_mode(n, &w, &h)) n++;          /* the last mode is the screen */
    if (n) w64_user32_mode(n - 1, &w, &h);
    int r = 0;
    switch (i) {
    case 88: case 90: r = 96; break;                 /* LOGPIXELSX/Y */
    case 12: r = 32; break;                          /* BITSPIXEL */
    case 14: r = 1; break;                           /* PLANES */
    case 116: r = 60; break;                         /* VREFRESH */
    case 8: case 118: r = w; break;                  /* HORZRES, DESKTOPHORZRES */
    case 10: case 117: r = h; break;                 /* VERTRES, DESKTOPVERTRES */
    case 4: r = (int)(w * 25.4 / 96 + 0.5); break;   /* HORZSIZE (mm) */
    case 6: r = (int)(h * 25.4 / 96 + 0.5); break;   /* VERTSIZE (mm) */
    }
    static uint64_t seen;
    if (i < 128 && !(seen >> (i / 2) & 1)) { seen |= 1ull << (i / 2); w64_log("[gdi] GetDeviceCaps(%d) -> %d", i, r); }
    RET(r);
}
static void iphlp_none(CPU *c) { RET(232); }              /* ERROR_NO_DATA */
static void HidD_GetHidGuid_(CPU *c) { memset(AP(0), 0, 16); RETV(); }
/* an empty device list: Unity re-polls its XInput pads after walking it (a failure skips that) */
static void SetupDiGetClassDevsA_(CPU *c) { RET(0x5E7D0001u); }
static void SetupDiEnum_(CPU *c) { w64_set_last_error(259 /* ERROR_NO_MORE_ITEMS */); RET(0); }
static void PathCanonicalizeW_(CPU *c) { size_t n = w16len(AW(1)) + 1; memmove(AP(0), AP(1), n * 2); RET(1); }
static void SHFileOperationW_(CPU *c) { RET(1); }         /* non-zero: failed */
static void PropVariantCopy_(CPU *c) { memcpy(AP(0), AP(1), 24); RET(0); }

static const w64_export_t more_exports[] = {
    { "OLEAUT32.dll", "#2", SysAllocString_ }, { "OLEAUT32.dll", "SysAllocString", SysAllocString_ },
    { "OLEAUT32.dll", "#4", SysAllocStringLen_ }, { "OLEAUT32.dll", "SysAllocStringLen", SysAllocStringLen_ },
    { "OLEAUT32.dll", "#6", SysFreeString_ }, { "OLEAUT32.dll", "SysFreeString", SysFreeString_ },
    { "OLEAUT32.dll", "#7", SysStringLen_ }, { "OLEAUT32.dll", "SysStringLen", SysStringLen_ },
    { "OLEAUT32.dll", "#8", VariantInit_ }, { "OLEAUT32.dll", "#9", VariantClear_ },
    { "OLEAUT32.dll", "#12", OleNotImpl_ }, { "OLEAUT32.dll", "#15", OleNull_ }, { "OLEAUT32.dll", "#16", OleNotImpl_ },
    { "OLEAUT32.dll", "#17", OleNull_ }, { "OLEAUT32.dll", "#19", OleNotImpl_ }, { "OLEAUT32.dll", "#20", OleNotImpl_ },
    { "OLEAUT32.dll", "#23", OleNotImpl_ }, { "OLEAUT32.dll", "#24", OleNotImpl_ }, { "OLEAUT32.dll", "#77", OleNotImpl_ },
    { "KERNEL32.dll", "GetEnvironmentVariableW", GetEnvironmentVariableW_ },
    { "KERNEL32.dll", "K32GetModuleBaseNameW", K32GetModuleBaseNameW_ },
    { "KERNEL32.dll", "K32GetProcessMemoryInfo", K32GetProcessMemoryInfo_ },
    { "KERNEL32.dll", "ReadProcessMemory", ReadProcessMemory_ },
    { "KERNEL32.dll", "CreateToolhelp32Snapshot", FailInvalidHandle_ },
    { "KERNEL32.dll", "Thread32First", Fail0_ }, { "KERNEL32.dll", "Thread32Next", Fail0_ },
    { "KERNEL32.dll", "InitializeProcThreadAttributeList", Fail0_ }, { "KERNEL32.dll", "UpdateProcThreadAttribute", Fail0_ },
    { "KERNEL32.dll", "DeleteProcThreadAttributeList", Ok1_ },
    { "KERNEL32.dll", "CancelIoEx", Fail0_ }, { "KERNEL32.dll", "PeekNamedPipe", Fail0_ },
    { "KERNEL32.dll", "OpenFileMappingW", Fail0_ }, { "KERNEL32.dll", "DeleteVolumeMountPointW", Fail0_ },
    { "KERNEL32.dll", "SetDllDirectoryW", Ok1_ },
    { "KERNEL32.dll", "LocalFileTimeToFileTime", CopyFileTime_ },
    { "KERNEL32.dll", "TzSpecificLocalTimeToSystemTime", TzLocalToSystem_ },
    { "WINMM.dll", "timeGetTime", timeGetTime_ },
    { "WINMM.dll", "waveOutGetNumDevs", ret0 }, { "WINMM.dll", "waveInGetNumDevs", ret0 },
    { "WINMM.dll", "waveOutOpen", mmsys_err }, { "WINMM.dll", "waveInOpen", mmsys_err },
    { "WINMM.dll", "waveOutGetDevCapsA", mmsys_err }, { "WINMM.dll", "waveOutGetDevCapsW", mmsys_err },
    { "WINMM.dll", "waveInGetDevCapsA", mmsys_err }, { "WINMM.dll", "waveInGetDevCapsW", mmsys_err },
    { "GDI32.dll", "GetDeviceCaps", GetDeviceCaps_ },
    { "GDI32.dll", "ChoosePixelFormat", ret0 }, { "GDI32.dll", "SetPixelFormat", ret0 }, { "GDI32.dll", "SwapBuffers", ret0 },
    { "IPHLPAPI.DLL", "GetAdaptersAddresses", iphlp_none }, { "IPHLPAPI.DLL", "GetNetworkParams", iphlp_none },
    { "IPHLPAPI.DLL", "GetIfEntry", iphlp_none },
    { "HID.DLL", "HidD_GetHidGuid", HidD_GetHidGuid_ },
    { "SETUPAPI.dll", "SetupDiGetClassDevsA", SetupDiGetClassDevsA_ }, { "SETUPAPI.dll", "SetupDiEnumDeviceInterfaces", SetupDiEnum_ },
    { "SETUPAPI.dll", "SetupDiEnumDeviceInfo", SetupDiEnum_ }, { "SETUPAPI.dll", "SetupDiDestroyDeviceInfoList", Ok1_ },
    { "IMM32.dll", "ImmGetContext", ret0 }, { "IMM32.dll", "ImmReleaseContext", Ok1_ },
    { "IMM32.dll", "ImmAssociateContext", ret0 }, { "IMM32.dll", "ImmAssociateContextEx", Ok1_ },
    { "IMM32.dll", "ImmGetCompositionStringW", ret0 }, { "IMM32.dll", "ImmGetConversionStatus", ret0 },
    { "IMM32.dll", "ImmNotifyIME", ret0 }, { "IMM32.dll", "ImmSetCompositionStringW", ret0 },
    { "WINHTTP.dll", "WinHttpOpen", ret0 }, { "WINHTTP.dll", "WinHttpCloseHandle", Ok1_ },
    { "WINHTTP.dll", "WinHttpGetIEProxyConfigForCurrentUser", ret0 }, { "WINHTTP.dll", "WinHttpGetProxyForUrl", ret0 },
    { "CRYPT32.dll", "CertOpenStore", ret0 }, { "CRYPT32.dll", "CertCloseStore", Ok1_ },
    { "CRYPT32.dll", "CertFreeCertificateContext", Ok1_ }, { "CRYPT32.dll", "CertFreeCertificateChain", ret0 },
    { "CRYPT32.dll", "CertGetCertificateChain", ret0 }, { "CRYPT32.dll", "CertVerifyCertificateChainPolicy", ret0 },
    { "CRYPT32.dll", "CertAddEncodedCertificateToStore", ret0 },
    { "ADVAPI32.dll", "CryptCreateHash", ret0 }, { "ADVAPI32.dll", "CryptHashData", ret0 },
    { "ADVAPI32.dll", "CryptGetHashParam", ret0 }, { "ADVAPI32.dll", "CryptDestroyHash", Ok1_ },
    { "VERSION.dll", "GetFileVersionInfoA", ret0 }, { "VERSION.dll", "VerQueryValueA", ret0 },
    { "SHLWAPI.dll", "PathCanonicalizeW", PathCanonicalizeW_ },
    { "SHELL32.dll", "SHFileOperationW", SHFileOperationW_ },
    { "ole32.dll", "PropVariantCopy", PropVariantCopy_ },
    { "dbghelp.dll", "SymInitialize", ret0 }, { "dbghelp.dll", "SymFromAddr", ret0 },
    { "SHELL32.dll", "DllGetVersion", DllGetVersion_ }, { "COMCTL32.dll", "DllGetVersion", DllGetVersion_ },
    { "ntdll.dll", "RtlGetVersion", RtlGetVersion_ }, { "ntdll.dll", "RtlVerifyVersionInfo", RtlVerifyVersionInfo_ },
    { "SHCORE.dll", "GetDpiForMonitor", GetDpiForMonitor_ }, { "SHCORE.dll", "SetProcessDpiAwareness", SetProcessDpiAwareness_ },
    { "USER32.dll", "GetDpiForWindow", GetDpiForWindow_ }, { "USER32.dll", "SetProcessDpiAwarenessContext", Ok1b_ },
    { "USER32.dll", "SetProcessDPIAware", Ok1b_ },
    { "KERNEL32.dll", "AppPolicyGetProcessTerminationMethod", AppPolicyGetProcessTerminationMethod_ },
    { "KERNEL32.dll", "AppPolicyGetThreadInitializationType", AppPolicyGetThreadInitializationType_ },
    { 0 }
};
__attribute__((constructor(102))) static void win_more_init(void) { w64_register(more_exports); }
