/*
 * steam.c - steam_api64.dll offline shim for The Forest (AppID 242760).
 */
#include "w64.h"
#include <pthread.h>
#include <time.h>

const char *w64_current_hostfn(void);

static int steam_fake(void)
{
    const char *m = getenv("W64_STEAM");
    return !m || strcmp(m, "fail");
}

static void SteamAPI_RestartAppIfNecessary_(CPU *c) { RET(0); }
static void SteamInternal_SteamAPI_Init_(CPU *c)
{
    w64_log("[steam] SteamInternal_SteamAPI_Init -> %s", steam_fake() ? "ok (fake)" : "no client");
    RET(steam_fake() ? 0 : 2);
}
static void SteamAPI_GetHSteamUser_(CPU *c) { RET(1); }
static void SteamAPI_RunCallbacks_(CPU *c) { RETV(); }
static void SteamAPI_Shutdown_(CPU *c) { RETV(); }
static void SteamAPI_RegisterCallback_(CPU *c) { RETV(); }
static void SteamAPI_UnregisterCallback_(CPU *c) { RETV(); }
static void SteamAPI_RegisterCallResult_(CPU *c) { RETV(); }
static void SteamAPI_UnregisterCallResult_(CPU *c) { RETV(); }
static void SteamGameServer_RunCallbacks_(CPU *c) { RETV(); }
static void SteamGameServer_GetHSteamUser_(CPU *c) { RET(0); }
static void SteamGameServer_Shutdown_(CPU *c) { RETV(); }
static void SteamGameServer_GetSteamID_(CPU *c) { RET(0); }
static void SteamInternal_GameServer_Init_V2_(CPU *c) { RET(2); }

static void SteamInternal_ContextInit_(CPU *c)
{
    uint64_t d = A(0);
    if (rd64(d + 8) != 1) {
        wr64(d + 8, 1);
        w64_call(c, rd64(d), 1, d + 16);
    }
    RET(d + 16);
}

#define NMETH 160
typedef struct { char ver[64]; uint64_t vtbl[NMETH]; uint64_t self_vptr; } iface_t;
static iface_t *g_if[64];
static int g_nif;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_seen[1024];
static int g_nseen;

static void make_iface(CPU *c, const char *ver);

static void method(CPU *c)
{
    const char *n = w64_current_hostfn();
    uint64_t key = (uint64_t)(uintptr_t)n;
    int dup = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nseen; i++) if (g_seen[i] == key) { dup = 1; break; }
    if (!dup && g_nseen < 1024) g_seen[g_nseen++] = key;
    pthread_mutex_unlock(&g_mu);
    const char *h = strchr(n, '#');
    int k = h ? atoi(h + 1) : -1;
    size_t pl = h ? (size_t)(h - n) : strlen(n);
    char ifn[64];
    snprintf(ifn, sizeof ifn, "%.*s", (int)pl, n);
    const uint64_t steamid = 0x0110000100000001ull;
    uint64_t r = 0;
    int sret = 0;
    if (!strncmp(ifn, "SteamClient", 11)) {
        if (k == 0 || k == 1 || k == 2 || k == 3) { RET(1); } /* CreateSteamPipe / BRelease / ConnectToGlobalUser */
        if (k == 9) { make_iface(c, A(2) ? AS(2) : "SteamUtils009"); return; }
        if (k == 5 || k == 6 || k == 8 || (k >= 10 && k <= 18) || (k >= 22 && k <= 29) || k == 33 || k == 34) {
            const char *ver = A(3) ? AS(3) : NULL;
            make_iface(c, (ver && *ver) ? ver : "SteamGeneric");
            return;
        }
    } else if (!strncmp(ifn, "SteamUser", 9)) {
        if (k == 0 || k == 1) r = 1;
        else if (k == 2) sret = 1;
        else if (k == 18) r = 0;
        else if (k == 23 || k == 24) r = 1;
    } else if (!strncmp(ifn, "SteamFriends", 12)) {
        if (k == 0) r = (uintptr_t)"Player";
        else if (k == 2) r = 1;
    } else if (!strncmp(ifn, "SteamUtils", 10)) {
        if (k == 2) r = 1;
        else if (k == 3) r = (uint64_t)time(NULL);
        else if (k == 4) r = (uintptr_t)"US";
        else if (k == 7) r = 255;
        else if (k == 8) r = 242760;                          /* The Forest AppID */
        else if (k == 21) r = (uintptr_t)"english";
    } else if (!strncmp(ifn, "STEAMAPPS_INTERFACE_VERSION", 27) || !strncmp(ifn, "SteamApps", 9)) {
        if (k == 0 || k == 6 || k == 7) r = 1;                /* BIsSubscribed, BIsSubscribedApp, BIsDlcInstalled */
        else if (k == 4) r = (uintptr_t)"english";
        else if (k == 5) r = (uintptr_t)"english";
        else if (k == 15) r = 0;
        else if (k == 19) r = 1;
        else if (k == 20) sret = 1;
    }
    if (sret) { wr64(c->rdx, steamid); r = c->rdx; }
    if (!dup) w64_log("[steam] %s -> %#llx%s", n, (unsigned long long)r, sret ? " (CSteamID)" : "");
    RET(r);
}

static void make_iface(CPU *c, const char *ver)
{
    if (!steam_fake()) RET(0);
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nif; i++) if (!strcmp(g_if[i]->ver, ver)) { pthread_mutex_unlock(&g_mu); RET((uintptr_t)&g_if[i]->self_vptr); }
    iface_t *f = calloc(1, sizeof *f);
    snprintf(f->ver, sizeof f->ver, "%s", ver);
    for (int k = 0; k < NMETH; k++) {
        char *nm = malloc(96);
        snprintf(nm, 96, "%s#%d", ver, k);
        f->vtbl[k] = w64_hostfn(nm, method);
    }
    f->self_vptr = (uint64_t)(uintptr_t)f->vtbl;
    if (g_nif < 64) g_if[g_nif++] = f;
    pthread_mutex_unlock(&g_mu);
    w64_log("[steam] interface %s -> fake", ver);
    RET((uintptr_t)&f->self_vptr);
}

static void SteamAPI_InitSafe_(CPU *c) { w64_log("[steam] SteamAPI_InitSafe -> %d", steam_fake()); RET(steam_fake() ? 1 : 0); }
static void SteamAPI_Init_(CPU *c) { w64_log("[steam] SteamAPI_Init -> %d", steam_fake()); RET(steam_fake() ? 1 : 0); }
static void SteamAPI_IsSteamRunning_(CPU *c) { RET(steam_fake() ? 1 : 0); }
static void SteamAPI_GetHSteamPipe_(CPU *c) { RET(steam_fake() ? 1 : 0); }
static void Steam_GetHSteamUserCurrent_(CPU *c) { RET(steam_fake() ? 1 : 0); }
static void SteamAPI_GetSteamInstallPath_(CPU *c) { RET((uintptr_t)"C:\\Steam"); }
static void SteamAPI_NoOp_(CPU *c) { RETV(); }
static void SteamAPI_Ret0_(CPU *c) { RET(0); }
static void SteamClient_(CPU *c) { make_iface(c, "SteamClient017"); }
static void SteamInternal_FindOrCreateUserInterface_(CPU *c) { make_iface(c, A(1) ? AS(1) : "?"); }
static void SteamInternal_FindOrCreateGameServerInterface_(CPU *c) { RET(0); }

static const w64_export_t steam_exports[] = {
    { "steam_api64.dll", "SteamAPI_RestartAppIfNecessary", SteamAPI_RestartAppIfNecessary_ },
    { "steam_api64.dll", "SteamInternal_SteamAPI_Init", SteamInternal_SteamAPI_Init_ },
    { "steam_api64.dll", "SteamAPI_Init", SteamAPI_Init_ },
    { "steam_api64.dll", "SteamAPI_InitSafe", SteamAPI_InitSafe_ },
    { "steam_api64.dll", "SteamAPI_IsSteamRunning", SteamAPI_IsSteamRunning_ },
    { "steam_api64.dll", "SteamAPI_GetHSteamUser", SteamAPI_GetHSteamUser_ },
    { "steam_api64.dll", "SteamAPI_GetHSteamPipe", SteamAPI_GetHSteamPipe_ },
    { "steam_api64.dll", "Steam_GetHSteamUserCurrent", Steam_GetHSteamUserCurrent_ },
    { "steam_api64.dll", "SteamAPI_GetSteamInstallPath", SteamAPI_GetSteamInstallPath_ },
    { "steam_api64.dll", "SteamClient", SteamClient_ },
    { "steam_api64.dll", "SteamAPI_RunCallbacks", SteamAPI_RunCallbacks_ },
    { "steam_api64.dll", "Steam_RunCallbacks", SteamAPI_RunCallbacks_ },
    { "steam_api64.dll", "Steam_RegisterInterfaceFuncs", SteamAPI_NoOp_ },
    { "steam_api64.dll", "SteamAPI_SetBreakpadAppID", SteamAPI_NoOp_ },
    { "steam_api64.dll", "SteamAPI_SetMiniDumpComment", SteamAPI_NoOp_ },
    { "steam_api64.dll", "SteamAPI_SetTryCatchCallbacks", SteamAPI_NoOp_ },
    { "steam_api64.dll", "SteamAPI_UseBreakpadCrashHandler", SteamAPI_NoOp_ },
    { "steam_api64.dll", "SteamAPI_WriteMiniDump", SteamAPI_NoOp_ },
    { "steam_api64.dll", "SteamAPI_Shutdown", SteamAPI_Shutdown_ },
    { "steam_api64.dll", "SteamAPI_RegisterCallback", SteamAPI_RegisterCallback_ },
    { "steam_api64.dll", "SteamAPI_UnregisterCallback", SteamAPI_UnregisterCallback_ },
    { "steam_api64.dll", "SteamAPI_RegisterCallResult", SteamAPI_RegisterCallResult_ },
    { "steam_api64.dll", "SteamAPI_UnregisterCallResult", SteamAPI_UnregisterCallResult_ },
    { "steam_api64.dll", "SteamInternal_ContextInit", SteamInternal_ContextInit_ },
    { "steam_api64.dll", "SteamInternal_FindOrCreateUserInterface", SteamInternal_FindOrCreateUserInterface_ },
    { "steam_api64.dll", "SteamInternal_FindOrCreateGameServerInterface", SteamInternal_FindOrCreateGameServerInterface_ },
    { "steam_api64.dll", "SteamGameServer_RunCallbacks", SteamGameServer_RunCallbacks_ },
    { "steam_api64.dll", "SteamGameServer_GetHSteamUser", SteamGameServer_GetHSteamUser_ },
    { "steam_api64.dll", "SteamGameServer_GetHSteamPipe", SteamAPI_Ret0_ },
    { "steam_api64.dll", "SteamGameServer_BSecure", SteamAPI_Ret0_ },
    { "steam_api64.dll", "SteamGameServer_InitSafe", SteamAPI_Ret0_ },
    { "steam_api64.dll", "SteamGameServer_Shutdown", SteamGameServer_Shutdown_ },
    { "steam_api64.dll", "SteamGameServer_GetSteamID", SteamGameServer_GetSteamID_ },
    { "steam_api64.dll", "SteamInternal_GameServer_Init_V2", SteamInternal_GameServer_Init_V2_ },
    { 0 }
};
__attribute__((constructor(102))) static void steam_init(void) { w64_register(steam_exports); }
