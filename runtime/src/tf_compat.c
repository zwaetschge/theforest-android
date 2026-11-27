/*
 * tf_compat.c - Additional Win32 / UCRT / VCRUNTIME140 / WS2_32 ordinal compatibility imports
 * required by Unity 5.6.5p4 (TheForest.exe) and its native plugins (fmod, fmodstudio, CSteamworks,
 * UnityImageNativePlugin).
 */
#define _GNU_SOURCE
#include "w64.h"
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

extern int es3_eh_throw(CPU *c, uint64_t ret_pc, uint64_t obj, uint64_t throw_info);

/* ---- KERNEL32 ---- */
static void GetVersion_(CPU *c) { RET(0x4A65000Au); /* Windows 10.0 build 19045 */ }
static void GetVersionExA_(CPU *c)
{
    uint8_t *p = AP(0);
    if (!p) RET(0);
    uint32_t sz = rd32(A(0));
    if (sz >= 148) {
        memset(p + 4, 0, sz - 4);
        wr32(A(0) + 4, 10);    /* Major */
        wr32(A(0) + 8, 0);     /* Minor */
        wr32(A(0) + 12, 19045);/* Build */
        wr32(A(0) + 16, 2);    /* VER_PLATFORM_WIN32_NT */
    }
    RET(1);
}
static void VerifyVersionInfoA_(CPU *c) { RET(1); }
static void HeapSetInformation_(CPU *c) { RET(1); }
static void SetHandleCount_(CPU *c) { RET(AU(0)); }
static void GetUserDefaultUILanguage_(CPU *c) { RET(0x0409); /* en-US */ }
static void FlushInstructionCache_(CPU *c) { RET(1); }
static void SetConsoleMode_(CPU *c) { RET(1); }
static void FlushConsoleInputBuffer_(CPU *c) { RET(1); }
static void ReadConsoleInputA_(CPU *c) { if (A(3)) wr32(A(3), 0); RET(1); }
static void GetDriveTypeA_(CPU *c) { RET(3); /* DRIVE_FIXED */ }

static void GlobalMemoryStatus_(CPU *c)
{
    uint8_t *p = AP(0);
    if (!p) RETV();
    memset(p, 0, 56);
    uint64_t ram = (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE);
    if (!ram) ram = 8ull << 30;
    uint32_t cap = ram > 0x7FFFFFFFull ? 0x7FFFFFFFu : (uint32_t)ram;
    wr32(A(0) + 0, 56);
    wr32(A(0) + 4, 30);
    wr64(A(0) + 8, ram);
    wr64(A(0) + 16, ram / 2);
    wr64(A(0) + 24, ram);
    wr64(A(0) + 32, ram / 2);
    wr64(A(0) + 40, 0x7FFFFFFFFFFFull);
    wr64(A(0) + 48, 0x7FFFFFFFFFFFull);
    (void)cap;
    RETV();
}

static void GetDiskFreeSpaceExA_(CPU *c)
{
    char hp[1024];
    w64_host_path(A(0) ? AS(0) : "C:\\", hp, sizeof hp);
    struct statvfs sv;
    uint64_t avail = 16ull << 30, total = 64ull << 30;
    if (statvfs(hp, &sv) == 0) {
        avail = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
        total = (uint64_t)sv.f_blocks * (uint64_t)sv.f_frsize;
    }
    if (A(1)) wr64(A(1), avail);
    if (A(2)) wr64(A(2), total);
    if (A(3)) wr64(A(3), avail);
    RET(1);
}

static void ExpandEnvironmentStringsA_(CPU *c)
{
    const char *src = AS(0);
    char *dst = AP(1);
    uint32_t cap = AU(2);
    if (!src) RET(0);
    size_t len = strlen(src) + 1;
    if (dst && cap > 0) snprintf(dst, cap, "%s", src);
    RET((uint32_t)len);
}

static void FileTimeToLocalFileTime_(CPU *c)
{
    if (A(0) && A(1)) wr64(A(1), rd64(A(0)));
    RET(1);
}
static void FileTimeToDosDateTime_(CPU *c)
{
    if (A(1)) wr16(A(1), (uint16_t)((46 << 9) | (1 << 5) | 1));
    if (A(2)) wr16(A(2), 0);
    RET(1);
}

static void lstrlenA_(CPU *c) { const char *s = AS(0); RET(s ? strlen(s) : 0); }
static void lstrcpyA_(CPU *c) { char *d = AP(0); const char *s = AS(1); if (d && s) strcpy(d, s); RET(A(0)); }
static void lstrcpynA_(CPU *c)
{
    char *d = AP(0); const char *s = AS(1); int n = AI(2);
    if (d && s && n > 0) { strncpy(d, s, (size_t)(n - 1)); d[n - 1] = 0; }
    RET(A(0));
}
static void lstrcpynW_(CPU *c)
{
    uint16_t *d = AP(0); const uint16_t *s = AW(1); int n = AI(2);
    if (d && s && n > 0) {
        int i = 0;
        for (; i < n - 1 && s[i]; i++) d[i] = s[i];
        d[i] = 0;
    }
    RET(A(0));
}

static void GetDateFormatA_(CPU *c)
{
    char *buf = AP(4); int cap = AI(5);
    const char *s = "11/26/2026";
    if (buf && cap > 0) snprintf(buf, (size_t)cap, "%s", s);
    RET((uint32_t)(strlen(s) + 1));
}
static void GetTimeFormatA_(CPU *c)
{
    char *buf = AP(4); int cap = AI(5);
    const char *s = "12:00:00";
    if (buf && cap > 0) snprintf(buf, (size_t)cap, "%s", s);
    RET((uint32_t)(strlen(s) + 1));
}
static void EnumSystemLocalesA_(CPU *c) { RET(1); }

/* Forward to existing W implementations */
static void CreateSemaphoreA_(CPU *c)
{
    w64_fn fn = w64_find("kernel32.dll", "CreateSemaphoreW");
    if (fn) { c->r9 = 0; fn(c); return; }
    RET(0);
}
static void CreateFileMappingA_(CPU *c)
{
    w64_fn fn = w64_find("kernel32.dll", "CreateFileMappingW");
    if (fn) {
        uint64_t save5 = rd64(c->rsp + 8 + 8 * 5);
        wr64(c->rsp + 8 + 8 * 5, 0);
        fn(c);
        wr64(c->rsp + 8 + 8 * 5, save5);
        return;
    }
    RET(0);
}
static void OpenEventW_(CPU *c) { w64_set_last_error(2); RET(0); }
static void ReleaseMutex_(CPU *c) { RET(1); }
static void CreateIoCompletionPort_(CPU *c) { RET(0x42420000ull); }
static void GetQueuedCompletionStatus_(CPU *c) { usleep(10000); w64_set_last_error(258); RET(0); }

/* Resources: return not found (Unity uses standard data files, only version/icon resources) */
static void FindResourceW_(CPU *c) { w64_set_last_error(1813); RET(0); }
static void FindResourceExW_(CPU *c) { w64_set_last_error(1813); RET(0); }
static void LoadResource_(CPU *c) { RET(0); }
static void LockResource_(CPU *c) { RET(0); }
static void SizeofResource_(CPU *c) { RET(0); }

static void FindFirstFileExA_(CPU *c)
{
    /* Forward to FindFirstFileA(lpFileName, lpFindFileData) */
    w64_fn fn = w64_find("kernel32.dll", "FindFirstFileA");
    if (fn) {
        uint64_t fdata = A(2);
        c->rdx = fdata;
        fn(c);
        return;
    }
    w64_set_last_error(2);
    RET(~0ull);
}

/* ---- UCRT / VCRUNTIME140 (for fmod / fmodstudio / CSteamworks / UnityImageNativePlugin) ---- */
static void crt_malloc_(CPU *c) { RET((uintptr_t)malloc((size_t)A(0))); }
static void crt_free_(CPU *c) { free(AP(0)); RETV(); }
static void crt_callnewh_(CPU *c) { RET(0); }
static void crt_memcpy_(CPU *c) { if (A(2)) memcpy(AP(0), AP(1), (size_t)A(2)); RET(A(0)); }
static void crt_memmove_(CPU *c) { if (A(2)) memmove(AP(0), AP(1), (size_t)A(2)); RET(A(0)); }
static void crt_memset_(CPU *c) { if (A(2)) memset(AP(0), AI(1), (size_t)A(2)); RET(A(0)); }
static void crt_memcmp_(CPU *c) { RET((uint32_t)memcmp(AP(0), AP(1), (size_t)A(2))); }
static void crt_wmemcpy_s_(CPU *c)
{
    uint16_t *dst = AP(0); size_t dsz = (size_t)A(1);
    const uint16_t *src = AW(2); size_t cnt = (size_t)A(3);
    if (!dst || !src || cnt > dsz) RET(22);
    if (cnt) memcpy(dst, src, cnt * 2);
    RET(0);
}
static void crt_initterm_(CPU *c)
{
    uint64_t p = A(0), end = A(1);
    for (; p < end; p += 8) {
        uint64_t fn = rd64(p);
        if (fn) w64_call(c, fn, 0);
    }
    RETV();
}
static void crt_initterm_e_(CPU *c)
{
    uint64_t p = A(0), end = A(1);
    for (; p < end; p += 8) {
        uint64_t fn = rd64(p);
        if (fn) {
            int r = (int)w64_call(c, fn, 0);
            if (r) RET((uint32_t)r);
        }
    }
    RET(0);
}
static __thread int t_crt_errno;
static void crt_errno_(CPU *c) { RET((uintptr_t)&t_crt_errno); }
static void crt_ret0_(CPU *c) { RET(0); }
static void crt_ret1_(CPU *c) { RET(1); }
static void crt_noop_(CPU *c) { RETV(); }
static void crt_init_onexit_table_(CPU *c)
{
    if (A(0)) memset(AP(0), 0, 24);
    RET(0);
}
static void crt_CxxThrowException_(CPU *c)
{
    uint64_t ret = rd64(c->rsp);
    es3_eh_throw(c, ret, A(0), A(1));
    RETV();
}
static void crt_InitializeCriticalSectionEx_(CPU *c)
{
    w64_fn fn = w64_find("kernel32.dll", "InitializeCriticalSectionEx");
    if (fn) { fn(c); return; }
    RET(1);
}
static void crt_std_exc_copy_(CPU *c)
{
    if (A(0) && A(1)) {
        const char *s = (const char *)(uintptr_t)rd64(A(0));
        wr64(A(1), s ? (uintptr_t)strdup(s) : 0);
        wr8(A(1) + 8, s ? 1 : 0);
    }
    RETV();
}
static void crt_std_exc_destroy_(CPU *c)
{
    if (A(0) && rd8(A(0) + 8)) { free((void *)(uintptr_t)rd64(A(0))); wr64(A(0), 0); wr8(A(0) + 8, 0); }
    RETV();
}

/* ---- USER32 / GDI32 / SHLWAPI / VERSION / OLE32 ---- */
static void MessageBoxW_(CPU *c)
{
    char *txt = A(1) ? w16_to_utf8(AW(1), -1) : strdup("");
    char *cap = A(2) ? w16_to_utf8(AW(2), -1) : strdup("");
    w64_log("[msgbox] %s: %s", cap, txt);
    free(txt); free(cap);
    RET(1 /* IDOK */);
}
static void wsprintfA_(CPU *c)
{
    char *out = AP(0); const char *fmt = AS(1);
    w64_va v = { .c = c, .idx = 2 };
    int n = w64_vformat(out, 4096, fmt, &v);
    RET((uint32_t)(n < 0 ? 0 : n));
}
static void wvsprintfA_(CPU *c)
{
    char *out = AP(0); const char *fmt = AS(1);
    w64_va v = { .c = NULL, .va = A(2) };
    int n = w64_vformat(out, 4096, fmt, &v);
    RET((uint32_t)(n < 0 ? 0 : n));
}
static void PathFileExistsW_(CPU *c)
{
    char *u = A(0) ? w16_to_utf8(AW(0), -1) : strdup("");
    char hp[1024];
    w64_host_path(u, hp, sizeof hp);
    free(u);
    struct stat st;
    RET(stat(hp, &st) == 0 ? 1 : 0);
}
static void GetMessageA_(CPU *c)
{
    w64_fn fn = w64_find("user32.dll", "GetMessageW");
    if (fn) { fn(c); return; }
    RET(0);
}
static void DispatchMessageA_(CPU *c)
{
    w64_fn fn = w64_find("user32.dll", "DispatchMessageW");
    if (fn) { fn(c); return; }
    RET(0);
}
static void SendMessageA_(CPU *c)
{
    w64_fn fn = w64_find("user32.dll", "SendMessageW");
    if (fn) { fn(c); return; }
    RET(0);
}
static void GetWindowLongPtrA_(CPU *c)
{
    w64_fn fn = w64_find("user32.dll", "GetWindowLongPtrW");
    if (fn) { fn(c); return; }
    RET(0);
}
static void ChangeDisplaySettingsA_(CPU *c) { RET(0 /* DISP_CHANGE_SUCCESSFUL */); }
static void EnableWindow_(CPU *c) { RET(0); }
static void GetAncestor_(CPU *c) { RET(A(0)); }
static void WindowFromPoint_(CPU *c) { RET(0x00010001ull); }
static void GetMessagePos_(CPU *c) { RET(0); }
static void GetMessageTime_(CPU *c) { RET((uint32_t)(w64_now_ns() / 1000000ull)); }
static void GetProcessWindowStation_(CPU *c) { RET(0x1234ull); }
static void GetUserObjectInformationW_(CPU *c)
{
    if (A(2) && AU(3) >= 12) { memset(AP(2), 0, 12); wr32(A(2) + 8, 1 /* WSF_VISIBLE */); }
    if (A(4)) wr32(A(4), 12);
    RET(1);
}
static void LoadKeyboardLayoutA_(CPU *c) { RET(0x04090409ull); }
static void CreateDIBSection_(CPU *c)
{
    /* Return dummy HBITMAP and allocate pixel buffer if ppvBits != NULL */
    if (A(1) && A(3)) {
        int32_t w = (int32_t)rd32(A(1) + 4), h = (int32_t)rd32(A(1) + 8);
        if (w < 0) w = -w; if (h < 0) h = -h;
        if (w <= 0 || w > 4096) w = 32;
        if (h <= 0 || h > 4096) h = 32;
        void *bits = calloc((size_t)w * (size_t)h, 4);
        wr64(A(3), (uintptr_t)bits);
    }
    RET(0x000B0001ull);
}
static void CreateBitmap_(CPU *c) { RET(0x000B0002ull); }
static void DeleteObject_(CPU *c) { RET(1); }
static void GetObjectA_(CPU *c) { RET(0); }
static void CreateIconIndirect_(CPU *c) { RET(0x000C0001ull); }
static void CopyImage_(CPU *c) { RET(A(0)); }
static void CLSIDFromString_(CPU *c) { if (A(1)) memset(AP(1), 0, 16); RET(0); }
static void GetFileVersionInfoSizeW_(CPU *c) { RET(0); }
static void GetFileVersionInfoW_(CPU *c) { RET(0); }
static void ImmSetOpenStatus_(CPU *c) { RET(1); }
static void GetIpAddrTable_(CPU *c) { RET(122 /* ERROR_INSUFFICIENT_BUFFER -> fail cleanly */); }
static void DnsQuery_A_(CPU *c) { RET(9003 /* DNS_ERROR_RCODE_NAME_ERROR */); }
static void DnsFree_(CPU *c) { RETV(); }

/* ---- WS2_32 / WSOCK32 ordinals -> forward to existing named ws2_32 exports ---- */
static void ws_fwd(CPU *c, const char *name)
{
    w64_fn fn = w64_find("ws2_32.dll", name);
    if (fn) { fn(c); return; }
    RET((uint64_t)-1);
}
#define WS_ORD(num, nm) static void ws_ord_##num(CPU *c) { ws_fwd(c, #nm); }
WS_ORD(1, accept)
WS_ORD(2, bind)
WS_ORD(3, closesocket)
WS_ORD(4, connect)
WS_ORD(5, getpeername)
WS_ORD(6, getsockname)
WS_ORD(7, getsockopt)
WS_ORD(8, htonl)
WS_ORD(9, htons)
WS_ORD(10, ioctlsocket)
WS_ORD(11, inet_addr)
WS_ORD(12, inet_ntoa)
WS_ORD(13, listen)
WS_ORD(14, ntohl)
WS_ORD(15, ntohs)
WS_ORD(16, recv)
WS_ORD(17, recvfrom)
WS_ORD(18, select)
WS_ORD(19, send)
WS_ORD(20, sendto)
WS_ORD(21, setsockopt)
WS_ORD(22, shutdown)
WS_ORD(23, socket)
WS_ORD(52, gethostbyname)
WS_ORD(53, gethostbyaddr)
WS_ORD(57, gethostname)
WS_ORD(111, WSAGetLastError)
WS_ORD(112, WSASetLastError)
WS_ORD(115, WSAStartup)
WS_ORD(116, WSACleanup)
WS_ORD(151, __WSAFDIsSet)
static void ws_ord_103(CPU *c) { RET(0); } /* WSAAsyncGetHostByName */
static void ws_ord_108(CPU *c) { RET(0); } /* WSACancelAsyncRequest */
static void WSARecvFrom_(CPU *c) { RET((uint32_t)-1); }
static void WSACloseEvent_(CPU *c) { RET(1); }
static void WSAResetEvent_(CPU *c) { RET(1); }
static void WSASetEvent_(CPU *c) { RET(1); }
static void WSAWaitForMultipleEvents_(CPU *c) { RET(258 /* WSA_WAIT_TIMEOUT */); }
static void getnameinfo_(CPU *c) { RET(11001); }

/* ---- ADVAPI32 ---- */
static void RegEnumKeyExA_(CPU *c) { RET(259 /* ERROR_NO_MORE_ITEMS */); }
static void RegQueryInfoKeyA_(CPU *c) { RET(2); }
static void RegisterEventSourceW_(CPU *c) { RET(0); }
static void DeregisterEventSource_(CPU *c) { RET(1); }
static void ReportEventW_(CPU *c) { RET(1); }
static void CryptAcquireContextA_(CPU *c) { RET(0); }
static void CryptImportKey_(CPU *c) { RET(0); }
static void CryptDestroyKey_(CPU *c) { RET(1); }
static void CryptVerifySignatureA_(CPU *c) { RET(0); }

/* ---- OPENGL32 / HID / WINMM waveOut / MSACM32 / Rewired / OVRPlugin stubs ---- */
static void stub_ret0_(CPU *c) { RET(0); }
static void stub_ret1_(CPU *c) { RET(1); }
static void SetWindowsHookEx_(CPU *c) { RET(0x70010001ull); }
static void UnhookWindowsHookEx_(CPU *c) { RET(1); }
static void CallNextHookEx_(CPU *c) { RET(0); }
static void RegisterDeviceNotificationA_(CPU *c) { RET(0xDE71CEull); }
static void ovrp_GetVersion_(CPU *c) { static const char ver[] = "1.16.0"; RET((uintptr_t)ver); }
static void ovrp_GetControllerState4_(CPU *c) { RET(-1004 /* ovrpFailure_NotInitialized */); }
static void ovrp_GetControllerState_sret_(CPU *c) { RET(A(0)); /* zeroed sret_buf from mono_tramp_dispatch */ }
static void DirectInput8Create_(CPU *c) { RET(0x80004005u /* E_FAIL */); }

static const w64_export_t tf_compat_exports[] = {
    /* KERNEL32 */
    { "kernel32.dll", "GetVersion", GetVersion_ },
    { "kernel32.dll", "GetVersionExA", GetVersionExA_ },
    { "kernel32.dll", "VerifyVersionInfoA", VerifyVersionInfoA_ },
    { "kernel32.dll", "HeapSetInformation", HeapSetInformation_ },
    { "kernel32.dll", "SetHandleCount", SetHandleCount_ },
    { "kernel32.dll", "GetUserDefaultUILanguage", GetUserDefaultUILanguage_ },
    { "kernel32.dll", "FlushInstructionCache", FlushInstructionCache_ },
    { "kernel32.dll", "SetConsoleMode", SetConsoleMode_ },
    { "kernel32.dll", "FlushConsoleInputBuffer", FlushConsoleInputBuffer_ },
    { "kernel32.dll", "ReadConsoleInputA", ReadConsoleInputA_ },
    { "kernel32.dll", "GetDriveTypeA", GetDriveTypeA_ },
    { "kernel32.dll", "GlobalMemoryStatus", GlobalMemoryStatus_ },
    { "kernel32.dll", "GetDiskFreeSpaceExA", GetDiskFreeSpaceExA_ },
    { "kernel32.dll", "ExpandEnvironmentStringsA", ExpandEnvironmentStringsA_ },
    { "kernel32.dll", "FileTimeToLocalFileTime", FileTimeToLocalFileTime_ },
    { "kernel32.dll", "FileTimeToDosDateTime", FileTimeToDosDateTime_ },
    { "kernel32.dll", "lstrlenA", lstrlenA_ },
    { "kernel32.dll", "lstrcpyA", lstrcpyA_ },
    { "kernel32.dll", "lstrcpynA", lstrcpynA_ },
    { "kernel32.dll", "lstrcpynW", lstrcpynW_ },
    { "kernel32.dll", "GetDateFormatA", GetDateFormatA_ },
    { "kernel32.dll", "GetTimeFormatA", GetTimeFormatA_ },
    { "kernel32.dll", "EnumSystemLocalesA", EnumSystemLocalesA_ },
    { "kernel32.dll", "CreateSemaphoreA", CreateSemaphoreA_ },
    { "kernel32.dll", "CreateFileMappingA", CreateFileMappingA_ },
    { "kernel32.dll", "OpenEventW", OpenEventW_ },
    { "kernel32.dll", "ReleaseMutex", ReleaseMutex_ },
    { "kernel32.dll", "CreateIoCompletionPort", CreateIoCompletionPort_ },
    { "kernel32.dll", "GetQueuedCompletionStatus", GetQueuedCompletionStatus_ },
    { "kernel32.dll", "FindResourceW", FindResourceW_ },
    { "kernel32.dll", "FindResourceExW", FindResourceExW_ },
    { "kernel32.dll", "LoadResource", LoadResource_ },
    { "kernel32.dll", "LockResource", LockResource_ },
    { "kernel32.dll", "SizeofResource", SizeofResource_ },
    { "kernel32.dll", "FindFirstFileExA", FindFirstFileExA_ },

    /* UCRT / VCRUNTIME140 / MSVCP140 */
    { NULL, "malloc", crt_malloc_ },
    { NULL, "free", crt_free_ },
    { NULL, "_callnewh", crt_callnewh_ },
    { NULL, "memcpy", crt_memcpy_ },
    { NULL, "memmove", crt_memmove_ },
    { NULL, "memset", crt_memset_ },
    { NULL, "memcmp", crt_memcmp_ },
    { NULL, "wmemcpy_s", crt_wmemcpy_s_ },
    { NULL, "_initterm", crt_initterm_ },
    { NULL, "_initterm_e", crt_initterm_e_ },
    { NULL, "_errno", crt_errno_ },
    { NULL, "_crt_atexit", crt_ret0_ },
    { NULL, "_cexit", crt_noop_ },
    { NULL, "_initialize_onexit_table", crt_init_onexit_table_ },
    { NULL, "_register_onexit_function", crt_ret0_ },
    { NULL, "_execute_onexit_table", crt_ret0_ },
    { NULL, "_configure_narrow_argv", crt_ret0_ },
    { NULL, "_initialize_narrow_environment", crt_ret0_ },
    { NULL, "_invalid_parameter_noinfo", crt_noop_ },
    { NULL, "_invalid_parameter_noinfo_noreturn", crt_noop_ },
    { NULL, "_seh_filter_dll", crt_ret0_ },
    { NULL, "__vcrt_InitializeCriticalSectionEx", crt_InitializeCriticalSectionEx_ },
    { NULL, "__C_specific_handler", crt_ret0_ },
    { NULL, "__CxxFrameHandler3", crt_ret0_ },
    { NULL, "_CxxThrowException", crt_CxxThrowException_ },
    { NULL, "__std_exception_copy", crt_std_exc_copy_ },
    { NULL, "__std_exception_destroy", crt_std_exc_destroy_ },
    { NULL, "__std_type_info_destroy_list", crt_noop_ },
    { NULL, "?_Xbad_alloc@std@@YAXXZ", crt_noop_ },
    { NULL, "?_Xlength_error@std@@YAXPEBD@Z", crt_noop_ },

    /* USER32 / GDI32 / SHLWAPI / VERSION / OLE32 / IMM32 / DNSAPI / IPHLPAPI */
    { "user32.dll", "MessageBoxW", MessageBoxW_ },
    { "user32.dll", "wsprintfA", wsprintfA_ },
    { "user32.dll", "wvsprintfA", wvsprintfA_ },
    { "user32.dll", "GetMessageA", GetMessageA_ },
    { "user32.dll", "DispatchMessageA", DispatchMessageA_ },
    { "user32.dll", "SendMessageA", SendMessageA_ },
    { "user32.dll", "GetWindowLongPtrA", GetWindowLongPtrA_ },
    { "user32.dll", "ChangeDisplaySettingsA", ChangeDisplaySettingsA_ },
    { "user32.dll", "EnableWindow", EnableWindow_ },
    { "user32.dll", "GetAncestor", GetAncestor_ },
    { "user32.dll", "WindowFromPoint", WindowFromPoint_ },
    { "user32.dll", "GetMessagePos", GetMessagePos_ },
    { "user32.dll", "GetMessageTime", GetMessageTime_ },
    { "user32.dll", "GetProcessWindowStation", GetProcessWindowStation_ },
    { "user32.dll", "GetUserObjectInformationW", GetUserObjectInformationW_ },
    { "user32.dll", "LoadKeyboardLayoutA", LoadKeyboardLayoutA_ },
    { "user32.dll", "CreateIconIndirect", CreateIconIndirect_ },
    { "user32.dll", "CopyImage", CopyImage_ },
    { "user32.dll", "IsDialogMessageW", stub_ret0_ },
    { "user32.dll", "CreateDialogParamA", stub_ret0_ },
    { "user32.dll", "CreateDialogParamW", stub_ret0_ },
    { "user32.dll", "DialogBoxParamA", stub_ret0_ },
    { "user32.dll", "GetDlgItem", stub_ret0_ },
    { "user32.dll", "CheckDlgButton", stub_ret1_ },
    { "user32.dll", "IsDlgButtonChecked", stub_ret0_ },
    { "user32.dll", "SetWindowsHookExA", SetWindowsHookEx_ },
    { "user32.dll", "SetWindowsHookExW", SetWindowsHookEx_ },
    { "user32.dll", "UnhookWindowsHookEx", UnhookWindowsHookEx_ },
    { "user32.dll", "CallNextHookEx", CallNextHookEx_ },
    { "user32.dll", "RegisterDeviceNotificationA", RegisterDeviceNotificationA_ },
    { "user32.dll", "RegisterTouchWindow", stub_ret0_ },
    { "ovrplugin.dll", "ovrp_GetVersion", ovrp_GetVersion_ },
    { "ovrplugin.dll", "ovrp_GetInitialized", stub_ret0_ },
    { "ovrplugin.dll", "ovrp_GetConnectedControllers", stub_ret0_ },
    { "ovrplugin.dll", "ovrp_GetActiveController", stub_ret0_ },
    { "ovrplugin.dll", "ovrp_GetControllerState4", ovrp_GetControllerState4_ },
    { "ovrplugin.dll", "ovrp_GetControllerState2", ovrp_GetControllerState_sret_ },
    { "ovrplugin.dll", "ovrp_GetControllerState", ovrp_GetControllerState_sret_ },
    { "ovrplugin.dll", "ovrp_GetNodePresent", stub_ret0_ },
    { "ovrplugin.dll", "ovrp_GetNodePositionTracked", stub_ret0_ },
    { "ovrplugin.dll", "ovrp_GetNodeOrientationTracked", stub_ret0_ },
    { "dinput8.dll", "DirectInput8Create", DirectInput8Create_ },
    { "bthprops.cpl.dll", "BluetoothFindFirstDevice", stub_ret0_ },
    { "bthprops.cpl.dll", "BluetoothFindNextDevice", stub_ret0_ },
    { "bthprops.cpl.dll", "BluetoothFindDeviceClose", stub_ret1_ },
    { "gdi32.dll", "CreateBitmap", CreateBitmap_ },
    { "gdi32.dll", "CreateDIBSection", CreateDIBSection_ },
    { "gdi32.dll", "DeleteObject", DeleteObject_ },
    { "gdi32.dll", "GetObjectA", GetObjectA_ },
    { "shlwapi.dll", "PathFileExistsW", PathFileExistsW_ },
    { "ole32.dll", "CLSIDFromString", CLSIDFromString_ },
    { "version.dll", "GetFileVersionInfoSizeW", GetFileVersionInfoSizeW_ },
    { "version.dll", "GetFileVersionInfoW", GetFileVersionInfoW_ },
    { "imm32.dll", "ImmSetOpenStatus", ImmSetOpenStatus_ },
    { "iphlpapi.dll", "GetIpAddrTable", GetIpAddrTable_ },
    { "dnsapi.dll", "DnsQuery_A", DnsQuery_A_ },
    { "dnsapi.dll", "DnsFree", DnsFree_ },

    /* ADVAPI32 */
    { "advapi32.dll", "RegEnumKeyExA", RegEnumKeyExA_ },
    { "advapi32.dll", "RegQueryInfoKeyA", RegQueryInfoKeyA_ },
    { "advapi32.dll", "RegisterEventSourceW", RegisterEventSourceW_ },
    { "advapi32.dll", "DeregisterEventSource", DeregisterEventSource_ },
    { "advapi32.dll", "ReportEventW", ReportEventW_ },
    { "advapi32.dll", "CryptAcquireContextA", CryptAcquireContextA_ },
    { "advapi32.dll", "CryptImportKey", CryptImportKey_ },
    { "advapi32.dll", "CryptDestroyKey", CryptDestroyKey_ },
    { "advapi32.dll", "CryptVerifySignatureA", CryptVerifySignatureA_ },

    /* WS2_32 & WSOCK32 ordinals */
    { "ws2_32.dll", "#1", ws_ord_1 }, { "wsock32.dll", "#1", ws_ord_1 },
    { "ws2_32.dll", "#2", ws_ord_2 }, { "wsock32.dll", "#2", ws_ord_2 },
    { "ws2_32.dll", "#3", ws_ord_3 }, { "wsock32.dll", "#3", ws_ord_3 },
    { "ws2_32.dll", "#4", ws_ord_4 }, { "wsock32.dll", "#4", ws_ord_4 },
    { "ws2_32.dll", "#5", ws_ord_5 }, { "ws2_32.dll", "#6", ws_ord_6 },
    { "ws2_32.dll", "#7", ws_ord_7 }, { "ws2_32.dll", "#8", ws_ord_8 }, { "wsock32.dll", "#8", ws_ord_8 },
    { "ws2_32.dll", "#9", ws_ord_9 }, { "wsock32.dll", "#9", ws_ord_9 },
    { "ws2_32.dll", "#10", ws_ord_10 }, { "ws2_32.dll", "#11", ws_ord_11 },
    { "ws2_32.dll", "#12", ws_ord_12 }, { "wsock32.dll", "#12", ws_ord_12 },
    { "ws2_32.dll", "#13", ws_ord_13 }, { "wsock32.dll", "#13", ws_ord_13 },
    { "ws2_32.dll", "#14", ws_ord_14 }, { "ws2_32.dll", "#15", ws_ord_15 },
    { "ws2_32.dll", "#16", ws_ord_16 }, { "wsock32.dll", "#16", ws_ord_16 },
    { "ws2_32.dll", "#17", ws_ord_17 },
    { "ws2_32.dll", "#18", ws_ord_18 }, { "wsock32.dll", "#18", ws_ord_18 },
    { "ws2_32.dll", "#19", ws_ord_19 }, { "wsock32.dll", "#19", ws_ord_19 },
    { "ws2_32.dll", "#20", ws_ord_20 },
    { "ws2_32.dll", "#21", ws_ord_21 }, { "wsock32.dll", "#21", ws_ord_21 },
    { "ws2_32.dll", "#22", ws_ord_22 },
    { "ws2_32.dll", "#23", ws_ord_23 }, { "wsock32.dll", "#23", ws_ord_23 },
    { "ws2_32.dll", "#52", ws_ord_52 }, { "ws2_32.dll", "#53", ws_ord_53 }, { "ws2_32.dll", "#57", ws_ord_57 },
    { "ws2_32.dll", "#103", ws_ord_103 }, { "wsock32.dll", "#103", ws_ord_103 },
    { "ws2_32.dll", "#108", ws_ord_108 }, { "wsock32.dll", "#108", ws_ord_108 },
    { "ws2_32.dll", "#111", ws_ord_111 }, { "wsock32.dll", "#111", ws_ord_111 },
    { "ws2_32.dll", "#112", ws_ord_112 },
    { "ws2_32.dll", "#115", ws_ord_115 }, { "wsock32.dll", "#115", ws_ord_115 },
    { "ws2_32.dll", "#116", ws_ord_116 }, { "wsock32.dll", "#116", ws_ord_116 },
    { "ws2_32.dll", "#151", ws_ord_151 },
    { "ws2_32.dll", "WSARecvFrom", WSARecvFrom_ },
    { "ws2_32.dll", "WSACloseEvent", WSACloseEvent_ },
    { "ws2_32.dll", "WSAResetEvent", WSAResetEvent_ },
    { "ws2_32.dll", "WSASetEvent", WSASetEvent_ },
    { "ws2_32.dll", "WSAWaitForMultipleEvents", WSAWaitForMultipleEvents_ },
    { "ws2_32.dll", "getnameinfo", getnameinfo_ },

    /* OPENGL32 / HID / MSACM32 */
    { "opengl32.dll", "wglCreateContext", stub_ret0_ },
    { "opengl32.dll", "wglDeleteContext", stub_ret1_ },
    { "opengl32.dll", "wglGetCurrentContext", stub_ret0_ },
    { "opengl32.dll", "wglGetCurrentDC", stub_ret0_ },
    { "opengl32.dll", "wglGetProcAddress", stub_ret0_ },
    { "opengl32.dll", "wglMakeCurrent", stub_ret0_ },
    { "hid.dll", "HidD_GetPreparsedData", stub_ret0_ },
    { "hid.dll", "HidD_FreePreparsedData", stub_ret1_ },
    { "hid.dll", "HidD_GetIndexedString", stub_ret0_ },
    { "hid.dll", "HidD_GetManufacturerString", stub_ret0_ },
    { "hid.dll", "HidD_GetProductString", stub_ret0_ },
    { "hid.dll", "HidD_GetSerialNumberString", stub_ret0_ },
    { "hid.dll", "HidP_GetCaps", stub_ret0_ },
    { "hid.dll", "HidP_GetButtonCaps", stub_ret0_ },
    { "hid.dll", "HidP_GetValueCaps", stub_ret0_ },
    { "hid.dll", "HidP_GetData", stub_ret0_ },
    { "hid.dll", "HidP_MaxDataListLength", stub_ret0_ },
    { 0 }
};

__attribute__((constructor(102))) static void tf_compat_init(void)
{
    w64_register(tf_compat_exports);
}
