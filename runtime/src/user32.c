/*
 * user32.c - windows, messages, monitors, keyboard/mouse state and the small things around them
 * for Unity's Windows player.
 *
 * One process-wide message queue (Unity pumps messages on its main thread). Window procedures are
 * guest code and are called with w64_call. The screen is W64_SCREEN=<w>x<h> (default 1920x1080);
 * the Android front end sets it to the real surface size. Input from the host (SDL on Linux,
 * touch on Android) arrives through w64_user32_post_input() as ordinary window messages.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <pthread.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

uint32_t w64_wait_objs(int n, const uint64_t *hs, int all, uint32_t ms);

/* ------------------------------------------------------------------ screen */
static int g_sw = 1920, g_sh = 1080, g_hz = 60;
static void screen_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *s = getenv("W64_SCREEN");
    if (s) sscanf(s, "%dx%d", &g_sw, &g_sh);
}
void w64_user32_set_screen(int w, int h) { screen_init(); g_sw = w; g_sh = h; }

/* ------------------------------------------------------------------ classes + windows */
typedef struct { uint16_t atom; char name[128]; uint64_t wndproc; int extra; } wclass_t;
typedef struct {
    int used; uint64_t hwnd; wclass_t *cls; uint64_t wndproc, user, id, parent;
    uint32_t style, exstyle; int x, y, w, h, visible, shown_once;
    uint8_t extra[256];
} wnd_t;
static wclass_t g_cls[64]; static int g_ncls;
static wnd_t g_wnd[64];
static pthread_mutex_t g_wmu2 = PTHREAD_MUTEX_INITIALIZER;
#define HWND_BASE 0x10010ull
#define HMONITOR_1 0x10001ull
#define HDC_FAKE 0x5DCull
static uint64_t g_focus, g_capture;

static wnd_t *wnd(uint64_t h)
{
    if (h < HWND_BASE || (h - HWND_BASE) % 16) return NULL;
    uint64_t i = (h - HWND_BASE) / 16;
    return i < 64 && g_wnd[i].used ? &g_wnd[i] : NULL;
}
uint64_t w64_user32_main_window(void) { for (int i = 0; i < 64; i++) if (g_wnd[i].used && !g_wnd[i].parent) return g_wnd[i].hwnd; return 0; }

static wclass_t *class_find(uint64_t name, int wide)
{
    if (name < 0x10000) { for (int i = 0; i < g_ncls; i++) if (g_cls[i].atom == name) return &g_cls[i]; return NULL; }
    char *n = wide ? w16_to_utf8((const uint16_t *)(uintptr_t)name, -1) : strdup((const char *)(uintptr_t)name);
    wclass_t *r = NULL;
    for (int i = 0; i < g_ncls; i++) if (!strcasecmp(g_cls[i].name, n)) { r = &g_cls[i]; break; }
    free(n);
    return r;
}
static uint16_t class_add(uint64_t name, int wide, uint64_t proc, int extra)
{
    wclass_t *c = class_find(name, wide);
    if (!c && g_ncls < 64) c = &g_cls[g_ncls++];
    if (!c) return 0;
    if (name >= 0x10000) {
        char *n = wide ? w16_to_utf8((const uint16_t *)(uintptr_t)name, -1) : strdup((const char *)(uintptr_t)name);
        snprintf(c->name, sizeof c->name, "%s", n);
        free(n);
    }
    c->atom = (uint16_t)(0xC000 + (c - g_cls));
    c->wndproc = proc; c->extra = extra;
    return c->atom;
}
/* WNDCLASSEXW: cbSize 0, style 4, lpfnWndProc 8, cbClsExtra 16, cbWndExtra 20, hInstance 24,
 * hIcon 32, hCursor 40, hbrBackground 48, lpszMenuName 56, lpszClassName 64, hIconSm 72 */
static void RegisterClassExW_(CPU *c) { uint64_t w = A(0); RET(class_add(rd64(w + 64), 1, rd64(w + 8), (int)rd32(w + 20))); }
/* WNDCLASSW: style 0, lpfnWndProc 8, cbClsExtra 16, cbWndExtra 20, hInstance 24, hIcon 32, hCursor 40,
 * hbrBackground 48, lpszMenuName 56, lpszClassName 64 */
static void RegisterClassW_(CPU *c) { uint64_t w = A(0); RET(class_add(rd64(w + 64), 1, rd64(w + 8), (int)rd32(w + 20))); }
static void UnregisterClassW_(CPU *c) { RET(1); }

typedef uint64_t (*host_wndproc_t)(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
static int is_guest_code_va(uint64_t va)
{
    return (va >= 0x100000000ull && va < 0x200000000ull) ||
           (va >= HLE_BASE && va < HLE_BASE + (uint64_t)W64_SENT_SLOTS * HLE_STRIDE);
}
static uint64_t call_wndproc_va(CPU *c, uint64_t va, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp)
{
    if (!va) return 0;
    if (is_guest_code_va(va))
        return w64_call(c, va, 4, hwnd, (uint64_t)msg, wp, lp);
    /* Native host function pointer (e.g. Mono C# Marshal.GetFunctionPointerForDelegate from Rewired) */
    return ((host_wndproc_t)(uintptr_t)va)(hwnd, msg, wp, lp);
}
static uint64_t call_proc(CPU *c, wnd_t *w, uint32_t msg, uint64_t wp, uint64_t lp)
{
    if (!w || !w->wndproc) return 0;
    return call_wndproc_va(c, w->wndproc, w->hwnd, msg, wp, lp);
}
static void CallWindowProcW_(CPU *c)
{
    RET(call_wndproc_va(c, A(0), A(1), AU(2), A(3), w64_arg(c, 4)));
}
static void send_size(CPU *c, wnd_t *w)
{
    call_proc(c, w, 0x0005 /* WM_SIZE */, 0, ((uint64_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
}

/* CreateWindowExW(exstyle, class, name, style, x, y, w, h, parent, menu, inst, param) */
void w64_user32_script_start(void);
static void CreateWindowExW_(CPU *c)
{
    w64_user32_script_start();
    screen_init();
    uint32_t exs = AU(0), style = AU(3);
    wclass_t *cls = class_find(A(1), 1);
    int x = AI(4), y = AI(5), w = AI(6), h = AI(7);
    uint64_t parent = w64_arg(c, 8), menu = w64_arg(c, 9), param = w64_arg(c, 11);
    pthread_mutex_lock(&g_wmu2);
    int i = 0; while (i < 64 && g_wnd[i].used) i++;
    if (i == 64) { pthread_mutex_unlock(&g_wmu2); RET(0); }
    wnd_t *wn = &g_wnd[i];
    memset(wn, 0, sizeof *wn);
    wn->used = 1; wn->hwnd = HWND_BASE + 16 * (uint64_t)i; wn->cls = cls;
    wn->wndproc = cls ? cls->wndproc : 0; wn->style = style; wn->exstyle = exs;
    wn->parent = parent; wn->id = menu;
    if (x == (int)0x80000000) x = 0;
    if (y == (int)0x80000000) y = 0;
    if (w == (int)0x80000000 || w <= 0) w = g_sw;
    if (h == (int)0x80000000 || h <= 0) h = g_sh;
    wn->x = x; wn->y = y; wn->w = w; wn->h = h;
    pthread_mutex_unlock(&g_wmu2);
    char *cn = A(1) >= 0x10000 ? w16_to_utf8(AW(1), -1) : strdup("#atom");
    w64_log("[user32] CreateWindowExW %s %dx%d style %#x -> %#llx", cn, w, h, style, (unsigned long long)wn->hwnd);
    free(cn);
    /* CREATESTRUCTW: lpCreateParams 0, hInstance 8, hMenu 16, hwndParent 24, cy 32, cx 36, y 40, x 44,
     * style 48, lpszName 56, lpszClass 64, dwExStyle 72 */
    uint8_t *cs = calloc(1, 80);
    memcpy(cs, &param, 8); memcpy(cs + 16, &menu, 8); memcpy(cs + 24, &parent, 8);
    memcpy(cs + 32, &h, 4); memcpy(cs + 36, &w, 4); memcpy(cs + 40, &y, 4); memcpy(cs + 44, &x, 4);
    memcpy(cs + 48, &style, 4); { uint64_t q = A(2); memcpy(cs + 56, &q, 8); q = A(1); memcpy(cs + 64, &q, 8); }
    memcpy(cs + 72, &exs, 4);
    uint64_t hwnd = wn->hwnd;
    call_proc(c, wn, 0x0081 /* WM_NCCREATE */, 0, (uint64_t)(uintptr_t)cs);
    call_proc(c, wn, 0x0001 /* WM_CREATE */, 0, (uint64_t)(uintptr_t)cs);
    free(cs);
    if (style & 0x10000000 /* WS_VISIBLE */) { wn->visible = 1; send_size(c, wn); }
    RET(hwnd);
}
static void DestroyWindow_(CPU *c)
{
    wnd_t *w = wnd(A(0));
    if (!w) RET(0);
    call_proc(c, w, 0x0002 /* WM_DESTROY */, 0, 0);
    call_proc(c, w, 0x0082 /* WM_NCDESTROY */, 0, 0);
    w->used = 0;
    RET(1);
}
static void DefWindowProcW_(CPU *c)
{
    uint32_t msg = AU(1);
    switch (msg) {
    case 0x0081: RET(1);                                  /* WM_NCCREATE */
    case 0x0010: { wnd_t *w = wnd(A(0)); if (w) call_proc(c, w, 0x0002, 0, 0); RET(0); }   /* WM_CLOSE -> WM_DESTROY */
    case 0x0084: RET(1);                                  /* WM_NCHITTEST: HTCLIENT */
    }
    RET(0);
}
static void ShowWindow_(CPU *c)
{
    wnd_t *w = wnd(A(0));
    if (!w) RET(0);
    int was = w->visible, cmd = AI(1);
    w->visible = cmd != 0;
    if (w->visible && !w->shown_once) {
        w->shown_once = 1;
        call_proc(c, w, 0x0018 /* WM_SHOWWINDOW */, 1, 0);
        send_size(c, w);
        call_proc(c, w, 0x001C /* WM_ACTIVATEAPP */, 1, 0);
        call_proc(c, w, 0x0006 /* WM_ACTIVATE */, 1 /* WA_ACTIVE */, 0);
        g_focus = w->hwnd;
        call_proc(c, w, 0x0007 /* WM_SETFOCUS */, 0, 0);
    }
    RET(was);
}
static void UpdateWindow_(CPU *c) { RET(wnd(A(0)) != NULL); }
static void set_pos(CPU *c, wnd_t *w, int x, int y, int cx, int cy, int move, int size)
{
    if (move) { w->x = x; w->y = y; }
    if (size && (cx != w->w || cy != w->h)) { w->w = cx; w->h = cy; send_size(c, w); }
}
/* SetWindowPos(hwnd, after, x, y, cx, cy, flags): SWP_NOSIZE 1, SWP_NOMOVE 2, SWP_SHOWWINDOW 0x40, SWP_HIDEWINDOW 0x80 */
static void SetWindowPos_(CPU *c)
{
    wnd_t *w = wnd(A(0));
    if (!w) RET(0);
    uint32_t fl = (uint32_t)w64_arg(c, 6);
    set_pos(c, w, AI(2), AI(3), (int)w64_arg(c, 4), (int)w64_arg(c, 5), !(fl & 2), !(fl & 1));
    if (fl & 0x40) w->visible = 1;
    if (fl & 0x80) w->visible = 0;
    RET(1);
}
static void MoveWindow_(CPU *c)
{
    wnd_t *w = wnd(A(0));
    if (!w) RET(0);
    set_pos(c, w, AI(1), AI(2), AI(3), (int)w64_arg(c, 4), 1, 1);
    RET(1);
}
static void put_rect(uint64_t r, int l, int t, int rr, int b) { int32_t v[4] = { l, t, rr, b }; memcpy((void *)(uintptr_t)r, v, 16); }
static void GetClientRect_(CPU *c) { wnd_t *w = wnd(A(0)); if (!w) { screen_init(); put_rect(A(1), 0, 0, g_sw, g_sh); RET(1); } put_rect(A(1), 0, 0, w->w, w->h); RET(1); }
static void GetWindowRect_(CPU *c)
{
    screen_init();
    wnd_t *w = wnd(A(0));
    if (!w) { put_rect(A(1), 0, 0, g_sw, g_sh); RET(1); }
    put_rect(A(1), w->x, w->y, w->x + w->w, w->y + w->h);
    RET(1);
}
static void AdjustWindowRectEx_(CPU *c) { RET(1); }       /* no borders: client == window */
static void ClientToScreen_(CPU *c) { wnd_t *w = wnd(A(0)); if (w) { int32_t *p = AP(1); p[0] += w->x; p[1] += w->y; } RET(1); }
static void ScreenToClient_(CPU *c) { wnd_t *w = wnd(A(0)); if (w) { int32_t *p = AP(1); p[0] -= w->x; p[1] -= w->y; } RET(1); }
/* WINDOWPLACEMENT: length, flags, showCmd, ptMinPosition, ptMaxPosition, rcNormalPosition */
static void GetWindowPlacement_(CPU *c)
{
    wnd_t *w = wnd(A(0));
    uint8_t *p = AP(1);
    memset(p + 4, 0, 40);
    uint32_t show = 1; memcpy(p + 8, &show, 4);
    if (w) put_rect(A(1) + 28, w->x, w->y, w->x + w->w, w->y + w->h);
    RET(1);
}
static uint64_t long_get(wnd_t *w, int idx)
{
    switch (idx) {
    case -4: return w->wndproc;                           /* GWLP_WNDPROC */
    case -6: return 0x180000000ull;                       /* GWLP_HINSTANCE */
    case -8: return w->parent;                            /* GWLP_HWNDPARENT */
    case -12: return w->id;                               /* GWLP_ID */
    case -16: return w->style;
    case -20: return w->exstyle;
    case -21: return w->user;                             /* GWLP_USERDATA */
    }
    if (idx >= 0 && idx + 8 <= 256) { uint64_t v; memcpy(&v, w->extra + idx, 8); return v; }
    return 0;
}
static uint64_t long_set(wnd_t *w, int idx, uint64_t v, int sz)
{
    uint64_t old = long_get(w, idx);
    switch (idx) {
    case -4: w->wndproc = v; break;
    case -8: w->parent = v; break;
    case -12: w->id = v; break;
    case -16: w->style = (uint32_t)v; break;
    case -20: w->exstyle = (uint32_t)v; break;
    case -21: w->user = v; break;
    default: if (idx >= 0 && idx + sz <= 256) memcpy(w->extra + idx, &v, (size_t)sz);
    }
    return old;
}
static void GetWindowLongPtrW_(CPU *c) { wnd_t *w = wnd(A(0)); RET(w ? long_get(w, AI(1)) : 0); }
static void GetWindowLongA_(CPU *c) { wnd_t *w = wnd(A(0)); RET(w ? (uint32_t)long_get(w, AI(1)) : 0); }
static void SetWindowLongPtrW_(CPU *c) { wnd_t *w = wnd(A(0)); RET(w ? long_set(w, AI(1), A(2), 8) : 0); }
static void SetWindowLongA_(CPU *c) { wnd_t *w = wnd(A(0)); RET(w ? (uint32_t)long_set(w, AI(1), AU(2), 4) : 0); }
static void IsWindowVisible_(CPU *c) { wnd_t *w = wnd(A(0)); RET(w && w->visible); }
static void IsWindow_(CPU *c) { RET(wnd(A(0)) != NULL); }
static void IsIconic_(CPU *c) { RET(0); }
static void GetParent_(CPU *c) { wnd_t *w = wnd(A(0)); RET(w ? w->parent : 0); }
static void GetDesktopWindow_(CPU *c) { RET(0x10000ull); }
static void GetFocus_(CPU *c) { RET(g_focus); }
static void SetFocus_(CPU *c) { uint64_t old = g_focus; if (wnd(A(0))) g_focus = A(0); RET(old); }
static void GetActiveWindow_(CPU *c) { RET(g_focus ? g_focus : w64_user32_main_window()); }
static void GetForegroundWindow_(CPU *c) { RET(g_focus ? g_focus : w64_user32_main_window()); }
static void SetForegroundWindow_(CPU *c) { RET(1); }
static void SetCapture_(CPU *c) { uint64_t o = g_capture; g_capture = A(0); RET(o); }
static void ReleaseCapture_(CPU *c) { g_capture = 0; RET(1); }
static void EnumWindows_(CPU *c)
{
    uint64_t proc = A(0), lp = A(1);
    for (int i = 0; i < 64; i++)
        if (g_wnd[i].used && !g_wnd[i].parent)
            if (!(uint32_t)w64_call(c, proc, 2, g_wnd[i].hwnd, lp)) break;
    RET(1);
}

/* ------------------------------------------------------------------ messages */
typedef struct { uint64_t hwnd; uint32_t msg; uint64_t wp, lp; uint32_t time; } qmsg_t;
static qmsg_t g_q[1024]; static int g_qh, g_qn;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
static int g_quit = -1;
typedef struct { uint64_t hwnd, id, proc; uint32_t ms; uint64_t next_ns; int used; } wtimer_t;
static wtimer_t g_timers[32];

static uint32_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
static void q_post(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp)
{
    pthread_mutex_lock(&g_qmu);
    if (g_qn < 1024) { g_q[(g_qh + g_qn) % 1024] = (qmsg_t){ hwnd, msg, wp, lp, now_ms() }; g_qn++; }
    pthread_mutex_unlock(&g_qmu);
}
/* Host input (SDL / Android touch) -> messages for the main window. */
void w64_user32_post_input(uint32_t msg, uint64_t wp, uint64_t lp)
{
    uint64_t h = g_focus ? g_focus : w64_user32_main_window();
    if (h) q_post(h, msg, wp, lp);
}
void (*w64_user32_pump)(void);                            /* set by the host front end */

static int q_take(qmsg_t *m, uint64_t hwnd, uint32_t lo, uint32_t hi, int remove)
{
    if (w64_user32_pump) w64_user32_pump();
    uint64_t now = w64_now_ns();
    for (int i = 0; i < 32; i++)
        if (g_timers[i].used && now >= g_timers[i].next_ns) {
            g_timers[i].next_ns = now + (uint64_t)g_timers[i].ms * 1000000ull;
            q_post(g_timers[i].hwnd, 0x0113 /* WM_TIMER */, g_timers[i].id, g_timers[i].proc);
        }
    pthread_mutex_lock(&g_qmu);
    for (int k = 0; k < g_qn; k++) {
        qmsg_t *q = &g_q[(g_qh + k) % 1024];
        if (hwnd && hwnd != (uint64_t)-1 && q->hwnd != hwnd) continue;
        if ((lo || hi) && (q->msg < lo || q->msg > hi)) continue;
        *m = *q;
        if (remove) {
            for (int j = k; j > 0; j--) g_q[(g_qh + j) % 1024] = g_q[(g_qh + j - 1) % 1024];
            g_qh = (g_qh + 1) % 1024; g_qn--;
        }
        pthread_mutex_unlock(&g_qmu);
        return 1;
    }
    pthread_mutex_unlock(&g_qmu);
    if (g_quit >= 0) { *m = (qmsg_t){ 0, 0x0012 /* WM_QUIT */, (uint64_t)g_quit, 0, now_ms() }; if (remove) g_quit = -1; return 1; }
    return 0;
}
/* MSG: hwnd 0, message 8, wParam 16, lParam 24, time 32, pt 36 */
static void put_msg(uint64_t p, const qmsg_t *m)
{
    memset((void *)(uintptr_t)p, 0, 48);
    wr64(p, m->hwnd); wr32(p + 8, m->msg); wr64(p + 16, m->wp); wr64(p + 24, m->lp); wr32(p + 32, m->time);
}
static void PeekMessageW_(CPU *c)
{
    qmsg_t m;
    if (!q_take(&m, A(1), AU(2), AU(3), (int)(w64_arg(c, 4) & 1))) RET(0);
    put_msg(A(0), &m);
    RET(1);
}
static void GetMessageW_(CPU *c)
{
    qmsg_t m;
    while (!q_take(&m, A(1), AU(2), AU(3), 1)) usleep(2000);
    put_msg(A(0), &m);
    RET(m.msg != 0x0012);
}
static void TranslateMessage_(CPU *c) { RET(0); }
static void DispatchMessageW_(CPU *c)
{
    uint64_t p = A(0);
    uint64_t hwnd = rd64(p), wp = rd64(p + 16), lp = rd64(p + 24);
    uint32_t msg = rd32(p + 8);
    if (msg == 0x0113 && lp) RET(w64_call(c, lp, 4, hwnd, (uint64_t)msg, wp, (uint64_t)now_ms()));   /* TIMERPROC */
    RET(call_proc(c, wnd(hwnd), msg, wp, lp));
}
static void SendMessageW_(CPU *c) { RET(call_proc(c, wnd(A(0)), AU(1), A(2), A(3))); }
static void SendMessageTimeoutA_(CPU *c) { uint64_t r = call_proc(c, wnd(A(0)), AU(1), A(2), A(3)); if (w64_arg(c, 6)) wr64(w64_arg(c, 6), r); RET(1); }
static void PostMessageW_(CPU *c) { q_post(A(0), AU(1), A(2), A(3)); RET(1); }
static void PostQuitMessage_(CPU *c) { g_quit = AI(0); RETV(); }
static void MsgWaitForMultipleObjects_(CPU *c)       /* (n, handles, waitAll, ms, wakeMask) */
{
    uint32_t n = AU(0), ms = AU(3);
    uint64_t hs[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs[i] = rd64(A(1) + 8 * i);
    uint64_t t0 = w64_now_ns();
    for (;;) {
        pthread_mutex_lock(&g_qmu); int have = g_qn > 0 || g_quit >= 0; pthread_mutex_unlock(&g_qmu);
        if (have) RET(n);                                 /* WAIT_OBJECT_0 + n: input available */
        if (n) { uint32_t r = w64_wait_objs((int)n, hs, AI(2), 2); if (r != 258) RET(r); }
        else usleep(2000);
        if (ms != 0xFFFFFFFFu && w64_now_ns() - t0 >= (uint64_t)ms * 1000000ull) RET(258);   /* WAIT_TIMEOUT */
        if (w64_user32_pump) w64_user32_pump();
    }
}
static void SetTimer_(CPU *c)
{
    for (int i = 0; i < 32; i++)
        if (!g_timers[i].used || (g_timers[i].hwnd == A(0) && g_timers[i].id == A(1))) {
            g_timers[i] = (wtimer_t){ A(0), A(1) ? A(1) : (uint64_t)(i + 1), A(3), AU(2), w64_now_ns() + (uint64_t)AU(2) * 1000000ull, 1 };
            RET(g_timers[i].id);
        }
    RET(0);
}
static void KillTimer_(CPU *c) { for (int i = 0; i < 32; i++) if (g_timers[i].used && g_timers[i].hwnd == A(0) && g_timers[i].id == A(1)) g_timers[i].used = 0; RET(1); }
static void GetMessageExtraInfo_(CPU *c) { RET(0); }
static uint32_t g_nextmsg = 0xC100;
static void RegisterWindowMessage_(CPU *c) { RET(__atomic_fetch_add(&g_nextmsg, 1, __ATOMIC_RELAXED)); }

/* ------------------------------------------------------------------ monitors / display modes */
static void MonitorFrom_(CPU *c) { RET(HMONITOR_1); }
/* MONITORINFO(EX)W: cbSize 0, rcMonitor 4, rcWork 20, dwFlags 36, szDevice[32] 40 (EX) */
static void get_monitor_info(CPU *c, int wide)
{
    screen_init();
    uint64_t mi = A(1);
    uint32_t cb = rd32(mi);
    put_rect(mi + 4, 0, 0, g_sw, g_sh);
    put_rect(mi + 20, 0, 0, g_sw, g_sh);
    wr32(mi + 36, 1);                                     /* MONITORINFOF_PRIMARY */
    if (cb >= 72) {
        if (wide) utf8_to_w16("\\\\.\\DISPLAY1", (uint16_t *)(uintptr_t)(mi + 40), 32);
        else strcpy((char *)(uintptr_t)(mi + 40), "\\\\.\\DISPLAY1");
    }
    RET(1);
}
static void GetMonitorInfoW_(CPU *c) { get_monitor_info(c, 1); }
static void GetMonitorInfoA_(CPU *c) { get_monitor_info(c, 0); }
static void EnumDisplayMonitors_(CPU *c)
{
    screen_init();
    int32_t *r = calloc(1, 16); r[2] = g_sw; r[3] = g_sh;
    w64_call(c, A(2), 4, HMONITOR_1, HDC_FAKE, (uint64_t)(uintptr_t)r, A(3));
    free(r);
    RET(1);
}
/* DEVMODEW: dmSize 68, dmFields 72, dmBitsPerPel 168, dmPelsWidth 172, dmPelsHeight 176, dmDisplayFrequency 184 (220 bytes)
 * DEVMODEA: dmSize 36, dmFields 40, dmBitsPerPel 104, dmPelsWidth 108, dmPelsHeight 112, dmDisplayFrequency 120 (156 bytes) */
/* Display modes: the standard desktop modes that fit, then the screen itself (the render surface
 * on Android). A single 16:10 mode made House Party's own resolution list come out empty, and the
 * game then asked for 1x1. Shared with DXGI (gfx/dxvk/hp_wsi.cpp, w64_user32_mode). */
static const uint16_t g_modes[][2] = {
    { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 },
    { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 }, { 3840, 2160 },
};
int w64_user32_mode(int i, int *w, int *h)
{
    screen_init();
    int n = 0, have_screen = 0;
    for (size_t k = 0; k < sizeof g_modes / sizeof *g_modes; k++) {
        if (g_modes[k][0] > g_sw || g_modes[k][1] > g_sh) continue;
        if (g_modes[k][0] == g_sw && g_modes[k][1] == g_sh) have_screen = 1;
        if (n++ == i) { *w = g_modes[k][0]; *h = g_modes[k][1]; return 1; }
    }
    if (!have_screen && n == i) { *w = g_sw; *h = g_sh; return 1; }
    return 0;
}
static void fill_devmode(uint64_t d, int wide, int w, int h)
{
    screen_init();
    uint32_t size = wide ? 220 : 156, osz = wide ? 68 : 36, ofl = wide ? 72 : 40, obpp = wide ? 168 : 104;
    memset((void *)(uintptr_t)d, 0, size);
    if (wide) utf8_to_w16("\\\\.\\DISPLAY1", (uint16_t *)(uintptr_t)d, 32); else strcpy((char *)(uintptr_t)d, "\\\\.\\DISPLAY1");
    wr16(d + osz, (uint16_t)size);
    wr32(d + ofl, 0x40000 | 0x80000 | 0x100000 | 0x400000);   /* BITSPERPEL | PELSWIDTH | PELSHEIGHT | DISPLAYFREQUENCY */
    wr32(d + obpp, 32);
    wr32(d + obpp + 4, (uint32_t)w);
    wr32(d + obpp + 8, (uint32_t)h);
    wr32(d + obpp + 16, (uint32_t)g_hz);
}
/* EnumDisplaySettings(name, mode, DEVMODE*): ENUM_CURRENT/REGISTRY_SETTINGS (-1/-2) = the screen */
static void enum_display(CPU *c, int wide)
{
    int m = AI(1), w, h;
    screen_init();
    static int logged;
    if (logged < 40) { logged++; w64_log("[user32] EnumDisplaySettings%s(%s, %d) screen %dx%d", wide ? "W" : "A",
                                          A(0) ? "name" : "NULL", m, g_sw, g_sh); }
    if (m < 0) { fill_devmode(A(2), wide, g_sw, g_sh); RET(1); }
    if (!w64_user32_mode(m, &w, &h)) RET(0);
    fill_devmode(A(2), wide, w, h);
    RET(1);
}
static void EnumDisplaySettingsW_(CPU *c) { enum_display(c, 1); }
static void EnumDisplaySettingsA_(CPU *c) { enum_display(c, 0); }
/* DISPLAY_DEVICEA: cb 0, DeviceName[32] 4, DeviceString[128] 36, StateFlags 164, DeviceID[128] 168, DeviceKey[128] 296 */
static void EnumDisplayDevicesA_(CPU *c)
{
    if (AU(1) > 0) RET(0);
    uint64_t d = A(2);
    memset((void *)(uintptr_t)(d + 4), 0, 420);
    strcpy((char *)(uintptr_t)(d + 4), A(0) ? "\\\\.\\DISPLAY1\\Monitor0" : "\\\\.\\DISPLAY1");
    strcpy((char *)(uintptr_t)(d + 36), A(0) ? "Generic Monitor" : "Android GPU");
    wr32(d + 164, 1 | 4);                                 /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    RET(1);
}
/* Display configuration API: one adapter, one source (\\.\DISPLAY1), one target. Unity builds
 * Screen.resolutions from the source's GDI name (EnumDisplaySettings(name, i)) only when this works. */
#define DC_ADAPTER_LO 0x1234u
static void GetDisplayConfigBufferSizes_(CPU *c) { if (A(1)) wr32(A(1), 1); if (A(2)) wr32(A(2), 2); RET(0); }
static void QueryDisplayConfig_(CPU *c)       /* (flags, *npath, path[], *nmode, mode[], topology*) */
{
    screen_init();
    if (!A(1) || !A(3)) RET(87);
    if (rd32(A(1)) < 1 || rd32(A(3)) < 2) { wr32(A(1), 1); wr32(A(3), 2); RET(122); }
    uint64_t p = A(2), m = A(4);
    memset((void *)(uintptr_t)p, 0, 72);
    wr32(p + 0, DC_ADAPTER_LO); wr32(p + 8, 0); wr32(p + 12, 0); wr32(p + 16, 1);           /* source: id 0, mode 0, in use */
    wr32(p + 20, DC_ADAPTER_LO); wr32(p + 28, 0x100); wr32(p + 32, 1);                        /* target: id 0x100, mode 1 */
    wr32(p + 36, 0x80000000u /* INTERNAL */); wr32(p + 40, 1 /* IDENTITY */); wr32(p + 44, 1 /* IDENTITY */);
    wr32(p + 48, (uint32_t)g_hz); wr32(p + 52, 1); wr32(p + 56, 1 /* PROGRESSIVE */); wr32(p + 60, 1); wr32(p + 64, 1);
    wr32(p + 68, 1 /* DISPLAYCONFIG_PATH_ACTIVE */);
    memset((void *)(uintptr_t)m, 0, 128);
    wr32(m + 0, 1 /* SOURCE */); wr32(m + 4, 0); wr32(m + 8, DC_ADAPTER_LO);
    wr32(m + 16, (uint32_t)g_sw); wr32(m + 20, (uint32_t)g_sh); wr32(m + 24, 4 /* 32BPP */);
    uint64_t t = m + 64;
    wr32(t + 0, 2 /* TARGET */); wr32(t + 4, 0x100); wr32(t + 8, DC_ADAPTER_LO);
    wr64(t + 16, (uint64_t)g_sw * g_sh * g_hz);
    wr32(t + 24, (uint32_t)g_hz * g_sh); wr32(t + 28, 1);                                     /* hsync */
    wr32(t + 32, (uint32_t)g_hz); wr32(t + 36, 1);                                              /* vsync */
    wr32(t + 40, (uint32_t)g_sw); wr32(t + 44, (uint32_t)g_sh); wr32(t + 48, (uint32_t)g_sw); wr32(t + 52, (uint32_t)g_sh);
    wr32(t + 56, 255); wr32(t + 60, 1);
    wr32(A(1), 1); wr32(A(3), 2);
    if (A(5)) wr32(A(5), 1 /* DISPLAYCONFIG_TOPOLOGY_INTERNAL */);
    RET(0);
}
static void DisplayConfigGetDeviceInfo_(CPU *c)  /* (DISPLAYCONFIG_DEVICE_INFO_HEADER*) */
{
    uint64_t h = A(0);
    uint32_t type = rd32(h), size = rd32(h + 4);
    if (type == 1 && size >= 84) {                  /* GET_SOURCE_NAME: viewGdiDeviceName[32] at 20 */
        utf8_to_w16("\\\\.\\DISPLAY1", (uint16_t *)(uintptr_t)(h + 20), 32);
        RET(0);
    }
    if (type == 2 && size >= 420) {                 /* GET_TARGET_NAME */
        memset((void *)(uintptr_t)(h + 20), 0, size - 20);
        wr32(h + 20, 1 /* friendlyNameFromEdid */); wr32(h + 24, 0x80000000u);
        utf8_to_w16("Android Display", (uint16_t *)(uintptr_t)(h + 36), 64);
        utf8_to_w16("\\\\?\\DISPLAY#AND0001#1", (uint16_t *)(uintptr_t)(h + 164), 128);
        RET(0);
    }
    RET(50);                                        /* ERROR_NOT_SUPPORTED for the rest (HDR info...) */
}
static void GetSystemMetrics_(CPU *c)
{
    screen_init();
    switch (AI(0)) {
    case 0: case 16: case 78: RET((uint32_t)g_sw);        /* SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN */
    case 1: case 17: case 79: RET((uint32_t)g_sh);
    case 80: RET(1);                                      /* SM_CMONITORS */
    case 43: RET(3);                                      /* SM_CMOUSEBUTTONS */
    case 19: RET(1);                                      /* SM_MOUSEPRESENT */
    case 32: case 33: RET(8);                             /* SM_CXSIZEFRAME */
    case 4: RET(23);                                      /* SM_CYCAPTION */
    }
    RET(0);
}
static void SystemParametersInfoW_(CPU *c)
{
    screen_init();
    if (AU(0) == 0x30 && A(2)) put_rect(A(2), 0, 0, g_sw, g_sh);   /* SPI_GETWORKAREA */
    RET(1);
}

/* ------------------------------------------------------------------ keyboard / mouse state */
static uint8_t g_keys[256];
static int g_mx, g_my;
void w64_user32_key_state(int vk, int down) { if (vk >= 0 && vk < 256) g_keys[vk] = down ? 0x80 : 0; }
void w64_user32_mouse_pos(int x, int y) { g_mx = x; g_my = y; }
static void GetKeyState_(CPU *c) { int vk = AI(0) & 0xFF; RET((uint16_t)(g_keys[vk] ? 0xFF80 : 0)); }
static void GetAsyncKeyState_(CPU *c) { int vk = AI(0) & 0xFF; RET((uint16_t)(g_keys[vk] ? 0x8000 : 0)); }
static void GetCursorPos_(CPU *c) { int32_t *p = AP(0); p[0] = g_mx; p[1] = g_my; RET(1); }
static void SetCursorPos_(CPU *c) { g_mx = AI(0); g_my = AI(1); RET(1); }
/* Cursor state as the game sets it (ShowCursor count, SetCursor(NULL), ClipCursor): the Android
 * front end hides the on-screen pad while the game shows a mouse cursor (menus, dialogue) and
 * shows it while the cursor is hidden or locked (walking around). */
static int g_show_count, g_cursor_null, g_clipped;
static void cursor_changed(void)
{
    static int last = -1;
    int v = g_show_count >= 0 && !g_cursor_null && !g_clipped;
    if (v != last) { last = v; w64_log("[user32] cursor %s (show %d, null %d, clip %d)", v ? "shown" : "hidden", g_show_count, g_cursor_null, g_clipped); }
}
int w64_user32_cursor_shown(void) { return g_show_count >= 0 && !g_cursor_null && !g_clipped; }
static void ClipCursor_(CPU *c) { g_clipped = A(0) != 0; cursor_changed(); RET(1); }
static void SetCursor_(CPU *c) { static uint64_t prev; uint64_t p = prev; prev = A(0); g_cursor_null = A(0) == 0; cursor_changed(); RET(p); }
static void ShowCursor_(CPU *c) { g_show_count += AI(0) ? 1 : -1; cursor_changed(); RET((uint32_t)g_show_count); }
static void TrackMouseEvent_(CPU *c) { RET(1); }
static void GetKeyboardLayout_(CPU *c) { RET(0x04090409ull); }
static void GetKeyboardLayoutNameW_(CPU *c) { utf8_to_w16("00000409", (uint16_t *)AP(0), 9); RET(1); }
void w64_user32_raw_mouse(int dx, int dy, uint16_t buttons, int16_t wheel);
void w64_user32_raw_key(int vk, int down);
/* ------------------------------------------------------------------ keyboard layout (US) */
/* virtual key -> scan code set 1 (0xE0xx = extended) */
static uint16_t vk_to_sc(int vk)
{
    static const char row1[] = "QWERTYUIOP", row2[] = "ASDFGHJKL", row3[] = "ZXCVBNM";
    for (int i = 0; row1[i]; i++) if (vk == row1[i]) return (uint16_t)(0x10 + i);
    for (int i = 0; row2[i]; i++) if (vk == row2[i]) return (uint16_t)(0x1E + i);
    for (int i = 0; row3[i]; i++) if (vk == row3[i]) return (uint16_t)(0x2C + i);
    if (vk >= '1' && vk <= '9') return (uint16_t)(0x02 + vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= 0x70 && vk <= 0x79) return (uint16_t)(0x3B + vk - 0x70);       /* F1-F10 */
    switch (vk) {
    case 0x7A: return 0x57; case 0x7B: return 0x58;                          /* F11 F12 */
    case 0x1B: return 0x01; case 0xBD: return 0x0C; case 0xBB: return 0x0D; case 0x08: return 0x0E;
    case 0x09: return 0x0F; case 0xDB: return 0x1A; case 0xDD: return 0x1B; case 0x0D: return 0x1C;
    case 0x11: case 0xA2: return 0x1D; case 0xA3: return 0xE01D;
    case 0xBA: return 0x27; case 0xDE: return 0x28; case 0xC0: return 0x29;
    case 0x10: case 0xA0: return 0x2A; case 0xA1: return 0x36; case 0xDC: return 0x2B;
    case 0xBC: return 0x33; case 0xBE: return 0x34; case 0xBF: return 0x35;
    case 0x12: case 0xA4: return 0x38; case 0xA5: return 0xE038; case 0x20: return 0x39; case 0x14: return 0x3A;
    case 0x26: return 0xE048; case 0x25: return 0xE04B; case 0x27: return 0xE04D; case 0x28: return 0xE050;
    case 0x2D: return 0xE052; case 0x2E: return 0xE053; case 0x24: return 0xE047; case 0x23: return 0xE04F;
    case 0x21: return 0xE049; case 0x22: return 0xE051;
    }
    return 0;
}
static int sc_to_vk(uint16_t sc, int ext)
{
    for (int vk = 1; vk < 256; vk++) {
        if (vk == 0x10 || vk == 0x11 || vk == 0x12) continue;                /* prefer the sided keys */
        uint16_t s = vk_to_sc(vk);
        if (s && (s & 0xFF) == (sc & 0xFF) && !!(s & 0xE000) == !!ext) return vk;
    }
    return 0;
}
static const char *key_name(uint16_t sc, int ext)
{
    int vk = sc_to_vk(sc, ext);
    static __thread char one[2];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { one[0] = (char)vk; one[1] = 0; return one; }
    if (vk >= 0x70 && vk <= 0x7B) { static __thread char f[4]; snprintf(f, sizeof f, "F%d", vk - 0x6F); return f; }
    switch (vk) {
    case 0x1B: return "Esc"; case 0x08: return "Backspace"; case 0x09: return "Tab"; case 0x0D: return "Enter";
    case 0x20: return "Space"; case 0x14: return "Caps Lock"; case 0xA0: return "Shift"; case 0xA1: return "Right Shift";
    case 0xA2: return "Ctrl"; case 0xA3: return "Right Ctrl"; case 0xA4: return "Alt"; case 0xA5: return "Right Alt";
    case 0x26: return "Up"; case 0x28: return "Down"; case 0x25: return "Left"; case 0x27: return "Right";
    case 0x2D: return "Insert"; case 0x2E: return "Delete"; case 0x24: return "Home"; case 0x23: return "End";
    case 0x21: return "Page Up"; case 0x22: return "Page Down";
    case 0xBD: return "-"; case 0xBB: return "="; case 0xDB: return "["; case 0xDD: return "]"; case 0xDC: return "\\";
    case 0xBA: return ";"; case 0xDE: return "'"; case 0xC0: return "`"; case 0xBC: return ","; case 0xBE: return ".";
    case 0xBF: return "/";
    }
    return "";
}
static int vk_char(int vk, int shift)
{
    if (vk >= 'A' && vk <= 'Z') return shift ? vk : vk + 32;
    if (vk >= '0' && vk <= '9') return shift ? ")!@#$%^&*("[vk - '0'] : vk;
    switch (vk) {
    case 0x20: return ' '; case 0x0D: return '\r'; case 0x08: return 8; case 0x09: return 9; case 0x1B: return 27;
    case 0xBD: return shift ? '_' : '-'; case 0xBB: return shift ? '+' : '='; case 0xDB: return shift ? '{' : '[';
    case 0xDD: return shift ? '}' : ']'; case 0xDC: return shift ? '|' : '\\'; case 0xBA: return shift ? ':' : ';';
    case 0xDE: return shift ? '"' : '\''; case 0xC0: return shift ? '~' : '`'; case 0xBC: return shift ? '<' : ',';
    case 0xBE: return shift ? '>' : '.'; case 0xBF: return shift ? '?' : '/';
    }
    return 0;
}
static void MapVirtualKeyW_(CPU *c)
{
    uint32_t code = AU(0), type = AU(1);
    switch (type) {
    case 0: RET(vk_to_sc((int)code) & 0xFF);                                 /* MAPVK_VK_TO_VSC */
    case 4: { uint16_t s = vk_to_sc((int)code); RET((s & 0xE000) ? (0xE000u | (s & 0xFF)) : s); }
    case 1: case 3: RET((uint32_t)sc_to_vk((uint16_t)(code & 0xFF), (code & 0xFF00) == 0xE000));
    case 2: { int ch = vk_char((int)code, 1); RET((uint32_t)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch)); }
    }
    RET(0);
}
/* ToUnicode(vk, sc, keystate[256], buf, n, flags) */
static void ToUnicode_(CPU *c)
{
    const uint8_t *ks = AP(2);
    int shift = ks && ((ks[0x10] | ks[0xA0] | ks[0xA1]) & 0x80);
    int ch = vk_char(AI(0), shift);
    if (!ch || !A(3) || AI(4) < 1) RET(0);
    ((uint16_t *)AP(3))[0] = (uint16_t)ch;
    if (AI(4) > 1) ((uint16_t *)AP(3))[1] = 0;
    RET(1);
}
static void GetKeyNameTextW_(CPU *c)
{
    uint32_t lp = AU(0);
    const char *n = key_name((uint16_t)((lp >> 16) & 0xFF), (lp >> 24) & 1);
    if (!A(1) || AI(2) < 1) RET(0);
    utf8_to_w16(n, (uint16_t *)AP(1), AI(2));
    RET(w16len((const uint16_t *)AP(1)));
}
static void GetKeyboardState_(CPU *c) { memcpy(AP(0), g_keys, 256); RET(1); }

/* ------------------------------------------------------------------ raw input */
/* The platform layer reports mouse motion/buttons and keys; each becomes a RAWINPUT record and a
 * WM_INPUT message (lParam = HRAWINPUT). GetRawInputBuffer drains the pending records - and their
 * WM_INPUT messages, as on Windows, where the buffer read takes them out of the queue. */
#define RI_N 1024
#define HDEV_MOUSE 0x52490001ull
#define HDEV_KBD   0x52490002ull
typedef struct { uint32_t type; uint8_t data[24]; uint32_t seq; } rawrec_t;
static rawrec_t g_ri[RI_N];
static uint32_t g_ri_seq, g_ri_read;          /* next sequence number; first one not yet drained */
static pthread_mutex_t g_rimu = PTHREAD_MUTEX_INITIALIZER;
static int g_ri_mouse, g_ri_kbd;
static uint64_t g_ri_mouse_target, g_ri_kbd_target;
static uint32_t ri_size(uint32_t type) { return type == 0 ? 48 : 40; }

static void ri_push(uint32_t type, const void *data, size_t n)
{
    if (!(type == 0 ? g_ri_mouse : g_ri_kbd)) return;
    pthread_mutex_lock(&g_rimu);
    uint32_t s = g_ri_seq++;
    rawrec_t *r = &g_ri[s % RI_N];
    r->type = type; r->seq = s;
    memset(r->data, 0, sizeof r->data); memcpy(r->data, data, n);
    if (g_ri_seq - g_ri_read > RI_N) g_ri_read = g_ri_seq - RI_N;
    pthread_mutex_unlock(&g_rimu);
    uint64_t tgt = type == 0 ? g_ri_mouse_target : g_ri_kbd_target;
    uint64_t h = tgt ? tgt : (g_focus ? g_focus : w64_user32_main_window());
    if (h) q_post(h, 0x00FF /* WM_INPUT */, 0 /* RIM_INPUT */, 0x7A000000ull + (s % RI_N) + ((uint64_t)s << 32));
}
/* mouse: relative motion, RI_MOUSE_* button flags, wheel in 120ths */
void w64_user32_raw_mouse(int dx, int dy, uint16_t buttons, int16_t wheel)
{
    uint8_t m[24] = { 0 };
    uint16_t bf = buttons | (wheel ? 0x0400 : 0);
    memcpy(m + 4, &bf, 2); memcpy(m + 6, &wheel, 2);
    int32_t x = dx, y = dy; memcpy(m + 12, &x, 4); memcpy(m + 16, &y, 4);
    ri_push(0, m, sizeof m);
}
void w64_user32_raw_key(int vk, int down)
{
    uint16_t sc = vk_to_sc(vk), make = sc & 0xFF, flags = (down ? 0 : 1) | ((sc & 0xE000) ? 2 : 0), v = (uint16_t)vk;
    uint32_t msg = down ? 0x100 : 0x101;
    uint8_t k[16] = { 0 };
    memcpy(k, &make, 2); memcpy(k + 2, &flags, 2); memcpy(k + 6, &v, 2); memcpy(k + 8, &msg, 4);
    ri_push(1, k, sizeof k);
}
static void ri_header(uint64_t p, const rawrec_t *r)
{
    wr32(p, r->type); wr32(p + 4, ri_size(r->type)); wr64(p + 8, r->type == 0 ? HDEV_MOUSE : HDEV_KBD); wr64(p + 16, 0);
}
static int ri_find(uint64_t h, rawrec_t *out)
{
    if ((h & 0xFF000000ull) != 0x7A000000ull) return 0;
    uint32_t s = (uint32_t)(h >> 32);
    pthread_mutex_lock(&g_rimu);
    rawrec_t *r = &g_ri[s % RI_N];
    int ok = r->seq == s && g_ri_seq - s <= RI_N;
    if (ok) *out = *r;
    pthread_mutex_unlock(&g_rimu);
    return ok;
}
/* RegisterRawInputDevices(RAWINPUTDEVICE[] {u16 page, u16 usage, u32 flags, HWND target}, n, size) */
static void RegisterRawInputDevices_(CPU *c)
{
    for (uint32_t i = 0; i < AU(1); i++) {
        uint64_t d = A(0) + 16ull * i;
        uint16_t page = rd16(d), usage = rd16(d + 2);
        uint32_t fl = rd32(d + 4);
        int on = !(fl & 0x1 /* RIDEV_REMOVE */);
        if (page == 1 && usage == 2) { g_ri_mouse = on; if (on && rd64(d + 8)) g_ri_mouse_target = rd64(d + 8); }
        if (page == 1 && usage == 6) { g_ri_kbd = on;   if (on && rd64(d + 8)) g_ri_kbd_target = rd64(d + 8); }
        w64_log("[user32] RegisterRawInputDevices page %u usage %u flags %#x hwnd %#llx", page, usage, fl, (unsigned long long)rd64(d + 8));
    }
    RET(1);
}
/* GetRawInputData(h, command, data, *size, header size) */
static void GetRawInputData_(CPU *c)
{
    rawrec_t r;
    if (!ri_find(A(0), &r)) RET(0xFFFFFFFFu);
    uint32_t cmd = AU(1), need = cmd == 0x10000005 /* RID_HEADER */ ? 24 : ri_size(r.type);
    if (!A(2)) { wr32(A(3), need); RET(0); }
    if (rd32(A(3)) < need) { w64_set_last_error(122); RET(0xFFFFFFFFu); }
    ri_header(A(2), &r);
    if (need > 24) memcpy((uint8_t *)AP(2) + 24, r.data, need - 24);
    RET(need);
}
static void q_drop_wm_input_upto(uint32_t seq_end)
{
    pthread_mutex_lock(&g_qmu);
    int w = 0;
    for (int k = 0; k < g_qn; k++) {
        qmsg_t m = g_q[(g_qh + k) % 1024];
        if (m.msg == 0x00FF && (m.lp & 0xFF000000ull) == 0x7A000000ull && (int32_t)((uint32_t)(m.lp >> 32) - seq_end) < 0) continue;
        g_q[(g_qh + w) % 1024] = m; w++;
    }
    g_qn = w;
    pthread_mutex_unlock(&g_qmu);
}
/* GetRawInputBuffer(data, *size, header size): pending records, 8-byte aligned */
static void GetRawInputBuffer_(CPU *c)
{
    if (!A(0)) { wr32(A(1), 48); RET(0); }
    uint32_t cap = rd32(A(1)), off = 0, n = 0;
    pthread_mutex_lock(&g_rimu);
    while (g_ri_read != g_ri_seq) {
        rawrec_t *r = &g_ri[g_ri_read % RI_N];
        uint32_t sz = ri_size(r->type);
        if (off + sz > cap) break;
        ri_header(A(0) + off, r);
        memcpy((uint8_t *)AP(0) + off + 24, r->data, sz - 24);
        off += (sz + 7) & ~7u; n++; g_ri_read++;
    }
    uint32_t upto = g_ri_read;
    pthread_mutex_unlock(&g_rimu);
    if (n) q_drop_wm_input_upto(upto);
    RET(n);
}
/* GetRawInputDeviceList(RAWINPUTDEVICELIST[] {HANDLE, u32 type}, *n, size) */
static void GetRawInputDeviceList_(CPU *c)
{
    if (!A(0)) { wr32(A(1), 2); RET(0); }
    if (rd32(A(1)) < 2) { wr32(A(1), 2); w64_set_last_error(122); RET(0xFFFFFFFFu); }
    wr64(A(0), HDEV_MOUSE); wr32(A(0) + 8, 0); wr64(A(0) + 16, HDEV_KBD); wr32(A(0) + 24, 1);
    RET(2);
}
/* GetRawInputDeviceInfoW(h, command, data, *size): RIDI_DEVICENAME / RIDI_DEVICEINFO */
static void GetRawInputDeviceInfoW_(CPU *c)
{
    uint64_t h = A(0);
    uint32_t cmd = AU(1);
    if (h != HDEV_MOUSE && h != HDEV_KBD) RET(0xFFFFFFFFu);
    if (cmd == 0x20000007) {                                       /* RIDI_DEVICENAME (chars) */
        const char *n = h == HDEV_MOUSE ? "\\\\?\\HID#VID_0000&PID_0001#mouse" : "\\\\?\\HID#VID_0000&PID_0002#keyboard";
        uint32_t need = (uint32_t)strlen(n) + 1;
        if (!A(2) || rd32(A(3)) < need) { wr32(A(3), need); RET(A(2) ? 0xFFFFFFFFu : 0); }
        utf8_to_w16(n, (uint16_t *)AP(2), (int)need);
        RET(need);
    }
    if (cmd == 0x2000000b) {                                       /* RIDI_DEVICEINFO (32 bytes) */
        if (!A(2) || rd32(A(3)) < 32) { wr32(A(3), 32); RET(A(2) ? 0xFFFFFFFFu : 0); }
        uint8_t *d = AP(2);
        memset(d + 4, 0, 28);
        wr32(A(2), 32);
        if (h == HDEV_MOUSE) { wr32(A(2) + 4, 0); wr32(A(2) + 8, 1); wr32(A(2) + 12, 3); wr32(A(2) + 16, 100); }
        else { wr32(A(2) + 4, 1); wr32(A(2) + 8, 4); wr32(A(2) + 12, 0); wr32(A(2) + 16, 1); wr32(A(2) + 20, 12); wr32(A(2) + 24, 104); }
        RET(32);
    }
    RET(0xFFFFFFFFu);
}


/* ------------------------------------------------------------------ the rest */
static void ret1(CPU *c) { RET(1); }
static void ret0(CPU *c) { RET(0); }
static void GetDC_(CPU *c) { RET(HDC_FAKE); }
static void LoadIconOrCursor_(CPU *c) { RET(0x7F00ull + (A(1) & 0xFF)); }
static void GetDoubleClickTime_(CPU *c) { RET(500); }
static void GetCaretBlinkTime_(CPU *c) { RET(530); }
static void GetThreadDesktop_(CPU *c) { RET(0x10002ull); }
static void RegisterDeviceNotificationW_(CPU *c) { RET(0xDE71CEull); }
static void DialogBoxParamW_(CPU *c) { w64_log("[user32] DialogBoxParamW (no dialogs) -> IDOK"); RET(1); }
static void CopyRect_(CPU *c) { if (!A(0) || !A(1)) RET(0); memcpy(AP(0), AP(1), 16); RET(1); }
static void OffsetRect_(CPU *c) { int32_t *r = AP(0); r[0] += AI(1); r[2] += AI(1); r[1] += AI(2); r[3] += AI(2); RET(1); }
static void PtInRect_(CPU *c) { int32_t *r = AP(0); int32_t x = (int32_t)A(1), y = (int32_t)(A(1) >> 32); RET(x >= r[0] && x < r[2] && y >= r[1] && y < r[3]); }

static const w64_export_t user32_exports[] = {
    { "USER32.dll", "RegisterClassExW", RegisterClassExW_ }, { "USER32.dll", "RegisterClassW", RegisterClassW_ },
    { "USER32.dll", "UnregisterClassW", UnregisterClassW_ },
    { "USER32.dll", "CreateWindowExW", CreateWindowExW_ }, { "USER32.dll", "DestroyWindow", DestroyWindow_ },
    { "USER32.dll", "DefWindowProcW", DefWindowProcW_ }, { "USER32.dll", "DefWindowProcA", DefWindowProcW_ },
    { "USER32.dll", "CallWindowProcW", CallWindowProcW_ }, { "USER32.dll", "CallWindowProcA", CallWindowProcW_ },
    { "USER32.dll", "ShowWindow", ShowWindow_ },
    { "USER32.dll", "UpdateWindow", UpdateWindow_ }, { "USER32.dll", "SetWindowPos", SetWindowPos_ },
    { "USER32.dll", "MoveWindow", MoveWindow_ }, { "USER32.dll", "GetClientRect", GetClientRect_ },
    { "USER32.dll", "GetWindowRect", GetWindowRect_ }, { "USER32.dll", "AdjustWindowRectEx", AdjustWindowRectEx_ },
    { "USER32.dll", "ClientToScreen", ClientToScreen_ }, { "USER32.dll", "ScreenToClient", ScreenToClient_ },
    { "USER32.dll", "GetWindowPlacement", GetWindowPlacement_ },
    { "USER32.dll", "GetWindowLongPtrW", GetWindowLongPtrW_ }, { "USER32.dll", "GetWindowLongA", GetWindowLongA_ },
    { "USER32.dll", "SetWindowLongPtrW", SetWindowLongPtrW_ }, { "USER32.dll", "SetWindowLongPtrA", SetWindowLongPtrW_ },
    { "USER32.dll", "SetWindowLongA", SetWindowLongA_ },
    { "USER32.dll", "IsWindowVisible", IsWindowVisible_ }, { "USER32.dll", "IsWindow", IsWindow_ }, { "USER32.dll", "IsIconic", IsIconic_ },
    { "USER32.dll", "GetParent", GetParent_ }, { "USER32.dll", "GetDesktopWindow", GetDesktopWindow_ },
    { "USER32.dll", "GetFocus", GetFocus_ }, { "USER32.dll", "SetFocus", SetFocus_ },
    { "USER32.dll", "GetActiveWindow", GetActiveWindow_ }, { "USER32.dll", "GetForegroundWindow", GetForegroundWindow_ },
    { "USER32.dll", "SetForegroundWindow", SetForegroundWindow_ }, { "USER32.dll", "AllowSetForegroundWindow", ret1 },
    { "USER32.dll", "SetCapture", SetCapture_ }, { "USER32.dll", "ReleaseCapture", ReleaseCapture_ },
    { "USER32.dll", "EnumWindows", EnumWindows_ },
    { "USER32.dll", "PeekMessageW", PeekMessageW_ }, { "USER32.dll", "PeekMessageA", PeekMessageW_ },
    { "USER32.dll", "GetMessageW", GetMessageW_ }, { "USER32.dll", "TranslateMessage", TranslateMessage_ },
    { "USER32.dll", "DispatchMessageW", DispatchMessageW_ }, { "USER32.dll", "SendMessageW", SendMessageW_ },
    { "USER32.dll", "SendMessageTimeoutA", SendMessageTimeoutA_ }, { "USER32.dll", "PostMessageW", PostMessageW_ },
    { "USER32.dll", "PostQuitMessage", PostQuitMessage_ }, { "USER32.dll", "MsgWaitForMultipleObjects", MsgWaitForMultipleObjects_ },
    { "USER32.dll", "SetTimer", SetTimer_ }, { "USER32.dll", "KillTimer", KillTimer_ },
    { "USER32.dll", "GetMessageExtraInfo", GetMessageExtraInfo_ },
    { "USER32.dll", "RegisterWindowMessageA", RegisterWindowMessage_ }, { "USER32.dll", "RegisterWindowMessageW", RegisterWindowMessage_ },
    { "USER32.dll", "MonitorFromWindow", MonitorFrom_ }, { "USER32.dll", "MonitorFromPoint", MonitorFrom_ },
    { "USER32.dll", "MonitorFromRect", MonitorFrom_ },
    { "USER32.dll", "GetMonitorInfoW", GetMonitorInfoW_ }, { "USER32.dll", "GetMonitorInfoA", GetMonitorInfoA_ },
    { "USER32.dll", "EnumDisplayMonitors", EnumDisplayMonitors_ },
    { "USER32.dll", "EnumDisplaySettingsW", EnumDisplaySettingsW_ }, { "USER32.dll", "EnumDisplaySettingsA", EnumDisplaySettingsA_ },
    { "USER32.dll", "EnumDisplayDevicesA", EnumDisplayDevicesA_ },
    { "USER32.dll", "GetDisplayConfigBufferSizes", GetDisplayConfigBufferSizes_ }, { "USER32.dll", "QueryDisplayConfig", QueryDisplayConfig_ },
    { "USER32.dll", "DisplayConfigGetDeviceInfo", DisplayConfigGetDeviceInfo_ },
    { "USER32.dll", "GetSystemMetrics", GetSystemMetrics_ }, { "USER32.dll", "SystemParametersInfoW", SystemParametersInfoW_ },
    { "USER32.dll", "GetKeyState", GetKeyState_ }, { "USER32.dll", "GetAsyncKeyState", GetAsyncKeyState_ },
    { "USER32.dll", "GetCursorPos", GetCursorPos_ }, { "USER32.dll", "SetCursorPos", SetCursorPos_ },
    { "USER32.dll", "ClipCursor", ClipCursor_ }, { "USER32.dll", "SetCursor", SetCursor_ },
    { "USER32.dll", "ShowCursor", ShowCursor_ }, { "USER32.dll", "TrackMouseEvent", TrackMouseEvent_ },
    { "USER32.dll", "DestroyCursor", ret1 }, { "USER32.dll", "DestroyIcon", ret1 },
    { "USER32.dll", "LoadCursorA", LoadIconOrCursor_ }, { "USER32.dll", "LoadIconA", LoadIconOrCursor_ },
    { "USER32.dll", "LoadIconW", LoadIconOrCursor_ }, { "USER32.dll", "LoadImageW", LoadIconOrCursor_ },
    { "USER32.dll", "GetKeyboardLayout", GetKeyboardLayout_ }, { "USER32.dll", "GetKeyboardLayoutNameW", GetKeyboardLayoutNameW_ },
    { "USER32.dll", "MapVirtualKeyW", MapVirtualKeyW_ }, { "USER32.dll", "MapVirtualKeyA", MapVirtualKeyW_ },
    { "USER32.dll", "MapVirtualKeyExA", MapVirtualKeyW_ }, { "USER32.dll", "ToUnicode", ToUnicode_ },
    { "USER32.dll", "GetKeyNameTextW", GetKeyNameTextW_ }, { "USER32.dll", "GetKeyboardState", GetKeyboardState_ },
    { "USER32.dll", "ToUnicodeEx", ToUnicode_ }, { "USER32.dll", "MapVirtualKeyExW", MapVirtualKeyW_ },
    { "USER32.dll", "RegisterRawInputDevices", RegisterRawInputDevices_ }, { "USER32.dll", "GetRawInputData", GetRawInputData_ },
    { "USER32.dll", "GetRawInputBuffer", GetRawInputBuffer_ }, { "USER32.dll", "GetRawInputDeviceList", GetRawInputDeviceList_ },
    { "USER32.dll", "GetRawInputDeviceInfoW", GetRawInputDeviceInfoW_ },
    { "USER32.dll", "GetDC", GetDC_ }, { "USER32.dll", "ReleaseDC", ret1 }, { "USER32.dll", "ValidateRect", ret1 },
    { "USER32.dll", "SetWindowTextW", ret1 }, { "USER32.dll", "GetDoubleClickTime", GetDoubleClickTime_ },
    { "USER32.dll", "GetCaretBlinkTime", GetCaretBlinkTime_ }, { "USER32.dll", "DragDetect", ret0 },
    { "USER32.dll", "GetThreadDesktop", GetThreadDesktop_ }, { "USER32.dll", "GetUserObjectInformationA", ret0 },
    { "USER32.dll", "RegisterDeviceNotificationW", RegisterDeviceNotificationW_ }, { "USER32.dll", "UnregisterDeviceNotification", ret1 },
    { "USER32.dll", "DialogBoxParamW", DialogBoxParamW_ }, { "USER32.dll", "EndDialog", ret1 },
    { "USER32.dll", "SendDlgItemMessageW", ret0 }, { "USER32.dll", "SetDlgItemTextA", ret1 }, { "USER32.dll", "SetDlgItemTextW", ret1 },
    { "USER32.dll", "OpenClipboard", ret0 }, { "USER32.dll", "CloseClipboard", ret1 }, { "USER32.dll", "EmptyClipboard", ret0 },
    { "USER32.dll", "GetClipboardData", ret0 }, { "USER32.dll", "SetClipboardData", ret0 }, { "USER32.dll", "IsClipboardFormatAvailable", ret0 },
    { "USER32.dll", "CopyRect", CopyRect_ }, { "USER32.dll", "OffsetRect", OffsetRect_ }, { "USER32.dll", "PtInRect", PtInRect_ },
    { "dwmapi.dll", "DwmGetWindowAttribute", ret0 }, { "dwmapi.dll", "DwmSetWindowAttribute", ret0 },
    { 0 }
};
__attribute__((constructor(102))) static void user32_init(void) { w64_register(user32_exports); }

/* ------------------------------------------------------------------ scripted input (host tests) */
/* Unity's UI sees a click only when down and up arrive in different frames; at a low frame rate a
 * short tap fits in one. Wait until n more frames were presented (at most 1 s). */
__attribute__((weak)) unsigned long hp_present_count(void) { return 0; }
void w64_user32_hold_frames(int n)
{
    unsigned long f0 = hp_present_count();
    for (int i = 0; i < 100; i++) {
        usleep(10000);
        if (i >= 8 && hp_present_count() >= f0 + (unsigned long)n) break;
    }
}
/* W64_INPUT="sec:click:x:y;sec:key:vk;sec:move:dx:dy;sec:text:abc" - seconds since the first window */
void w64_user32_click(int x, int y)
{
    w64_user32_mouse_pos(x, y);
    w64_user32_post_input(0x0200, 0, (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16));
    w64_user32_key_state(1, 1);
    w64_user32_raw_mouse(0, 0, 0x0001, 0);
    w64_user32_post_input(0x0201, 1, (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16));
    w64_user32_hold_frames(2);
    w64_user32_key_state(1, 0);
    w64_user32_raw_mouse(0, 0, 0x0002, 0);
    w64_user32_post_input(0x0202, 0, (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16));
}
void w64_user32_key(int vk, int down)
{
    w64_user32_key_state(vk, down);
    w64_user32_raw_key(vk, down);
    uint64_t lp = 1 | ((uint64_t)(vk_to_sc(vk) & 0xFF) << 16) | ((vk_to_sc(vk) & 0xE000) ? (1ull << 24) : 0) | (down ? 0 : (3ull << 30));
    w64_user32_post_input(down ? 0x0100 : 0x0101, (uint64_t)vk, lp);
    if (down) { int ch = vk_char(vk, (g_keys[0x10] | g_keys[0xA0]) & 0x80); if (ch) w64_user32_post_input(0x0102, (uint64_t)ch, lp); }
}
static void *input_script(void *arg)
{
    char *s = strdup((const char *)arg), *save = NULL;
    uint64_t t0 = w64_now_ns();
    for (char *t = strtok_r(s, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        double at = 0; char cmd[16] = ""; int a = 0, b = 0;
        char txt[128] = "";
        if (sscanf(t, "%lf:%15[a-z]:%127[^;]", &at, cmd, txt) < 2) continue;
        sscanf(txt, "%i:%i", &a, &b);
        while ((w64_now_ns() - t0) / 1e9 < at) usleep(50000);
        w64_log("[input] %s", t);
        if (!strcmp(cmd, "click")) w64_user32_click(a, b);
        else if (!strcmp(cmd, "key")) { w64_user32_key(a, 1); usleep(100000); w64_user32_key(a, 0); }
        else if (!strcmp(cmd, "down")) w64_user32_key(a, 1);
        else if (!strcmp(cmd, "up")) w64_user32_key(a, 0);
        else if (!strcmp(cmd, "move")) w64_user32_raw_mouse(a, b, 0, 0);
        else if (!strcmp(cmd, "text")) for (char *p = txt; *p; p++) w64_user32_post_input(0x0102, (uint64_t)(uint8_t)*p, 1);
    }
    free(s);
    return NULL;
}
void w64_user32_script_start(void)
{
    static int started;
    const char *e = getenv("W64_INPUT");
    if (started || !e) return;
    started = 1;
    pthread_t th;
    pthread_create(&th, NULL, input_script, (void *)e);
}
