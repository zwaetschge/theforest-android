/*
 * android_main.c - SDL3 entry on Android. SDLActivity calls SDL_main on its SDLThread with the
 * arguments ForestActivity.getArguments() returns: <game dir> <user dir>.
 *
 * The game runs on its own thread (the recompiled code wants a big stack); this thread owns the
 * SDL window: it hands the ANativeWindow to DXVK (gfx/dxvk/hp_wsi.cpp), turns touch and keys into
 * Win32 window messages for the game's window (runtime/src/user32.c) and pauses presentation
 * while the app is in the background.
 *
 * Debug switches come from the system property debug.tf.env (or debug.hp.env).
 */
#define _GNU_SOURCE
#include "w64.h"
#include <SDL3/SDL.h>
#include <android/log.h>
#include <android/native_window.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

void *hp_wsi_set_window(ANativeWindow *window);
int hp_wsi_has_window(void);
void hp_wsi_set_screen(uint32_t w, uint32_t h);
void hp_wsi_pause(int paused, int wait_ms);
void hp_dxvk_save_pipeline_cache(void) __attribute__((weak));
void w64_user32_set_screen(int w, int h);
void w64_user32_post_input(uint32_t msg, uint64_t wp, uint64_t lp);
void w64_user32_key_state(int vk, int down);
void w64_user32_mouse_pos(int x, int y);
void w64_user32_raw_mouse(int dx, int dy, uint16_t buttons, int16_t wheel);
void w64_user32_key(int vk, int down);
void w64_user32_hold_frames(int n);
unsigned long hp_present_count(void);
void w64_xinput_set(int idx, int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                    int16_t lx, int16_t ly, int16_t rx, int16_t ry);

void w64_shutdown(int code)
{
    w64_log("[android] exit %d", code);
    _exit(code);
}

static void apply_env_prop(const char *prop)
{
    char v[PROP_VALUE_MAX] = "";
    __system_property_get(prop, v);
    if (!v[0]) return;
    char *save = NULL;
    for (char *t = strtok_r(v, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        setenv(t, eq + 1, 1);
        __android_log_print(ANDROID_LOG_INFO, "theforest", "env %s=%s", t, eq + 1);
    }
}

static void env_from_property(void)
{
    apply_env_prop("debug.hp.env");
    apply_env_prop("debug.tf.env");
}

static int g_rc = -1;
static void *game_thread(void *arg)
{
    (void)arg;
    if (w64_load(w64_gamedir)) { w64_log("[android] cannot load the game from %s", w64_gamedir); g_rc = 2; return NULL; }
    g_rc = w64_run_entry();
    w64_log("[android] game returned %d", g_rc);
    w64_shutdown(g_rc);
    return NULL;
}

static SDL_Window *g_win;

static void attach_surface(void)
{
    ANativeWindow *nw = SDL_GetPointerProperty(SDL_GetWindowProperties(g_win), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    hp_wsi_set_window(nw);
    w64_log("[android] surface %p", (void *)nw);
}

#define WM_ACTIVATEAPP 0x001C

static bool SDLCALL app_lifecycle_watch(void *userdata, SDL_Event *e)
{
    (void)userdata;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        w64_user32_post_input(WM_ACTIVATEAPP, 0, 0);
        hp_wsi_pause(1, 2000);
        hp_wsi_set_window(NULL);
        if (hp_dxvk_save_pipeline_cache) hp_dxvk_save_pipeline_cache();
        w64_log("[android] paused (surface detached)");
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        attach_surface();
        hp_wsi_pause(0, 0);
        w64_user32_post_input(WM_ACTIVATEAPP, 1, 0);
    }
    return true;
}

/* ---- audio: WASAPI emulation (runtime/src/wasapi.c) plays through an SDL audio stream ---- */
static SDL_AudioStream *g_astream;
static int g_ach = 2;
int w64_audio_sink_open(int rate, int channels)
{
    SDL_AudioSpec spec = { SDL_AUDIO_F32, channels, rate };
    g_astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_astream) { w64_log("[android] audio: %s", SDL_GetError()); return -1; }
    g_ach = channels;
    SDL_ResumeAudioStreamDevice(g_astream);
    return 0;
}
void w64_audio_sink_write(const float *frames, int n) { if (g_astream) SDL_PutAudioStreamData(g_astream, frames, n * g_ach * (int)sizeof(float)); }
int w64_audio_sink_queued(void) { return g_astream ? SDL_GetAudioStreamQueued(g_astream) / (g_ach * (int)sizeof(float)) : 0; }

/* ---- gamepads -> XInput pad 0 (+ virtual menu cursor on Title Screen & Menus) ---- */
#define MAX_HW_PADS 8
static SDL_Gamepad *g_pads[MAX_HW_PADS];
static struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } g_hw, g_vpad;
static pthread_mutex_t g_pad_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_sw, g_sh;
static int g_vcursor_vis;
static float g_vcx, g_vcy;
static int g_in_gameplay;
static uint64_t g_hidden_since_ns;

void w64_user32_click(int x, int y);
void w64_user32_get_mouse_pos(int *x, int *y);
int w64_user32_cursor_shown(void);

static void *tap_thread(void *arg)
{
    intptr_t v = (intptr_t)arg;
    w64_user32_click((int)(v >> 16), (int)(v & 0xFFFF));
    return NULL;
}
static void spawn_click(int x, int y)
{
    pthread_t th;
    intptr_t v = ((intptr_t)(x & 0xFFFF) << 16) | ((intptr_t)(y & 0xFFFF));
    if (pthread_create(&th, NULL, tap_thread, (void *)v) == 0) pthread_detach(th);
}
static int16_t deadzone(int16_t v)
{
    return (v > -2500 && v < 2500) ? 0 : v;
}
static void pad_publish(void)
{
    pthread_mutex_lock(&g_pad_mu);
    #define PICK(f) (g_vpad.f ? g_vpad.f : g_hw.f)
    w64_xinput_set(0, 1, g_hw.buttons | g_vpad.buttons, g_hw.lt > g_vpad.lt ? g_hw.lt : g_vpad.lt, g_hw.rt > g_vpad.rt ? g_hw.rt : g_vpad.rt,
                   PICK(lx), PICK(ly), PICK(rx), PICK(ry));
    #undef PICK
    pthread_mutex_unlock(&g_pad_mu);
}
static uint16_t xbutton(int b)
{
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return 0x1000; case SDL_GAMEPAD_BUTTON_EAST: return 0x2000;
    case SDL_GAMEPAD_BUTTON_WEST: return 0x4000; case SDL_GAMEPAD_BUTTON_NORTH: return 0x8000;
    case SDL_GAMEPAD_BUTTON_BACK: return 0x20; case SDL_GAMEPAD_BUTTON_START: return 0x10;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 0x40; case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 0x80;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 0x100; case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 0x200;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return 0x1; case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 0x2;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 0x4; case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 0x8;
    }
    return 0;
}
static int16_t inv(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; }
static int g_pad_log_cnt;
static int g_java_hwpad_active;
static uint16_t g_prev_hw_buttons;
static void vcursor_init_pos(void)
{
    if (!g_vcursor_vis && g_sw > 0 && g_sh > 0) {
        int mx = 0, my = 0;
        w64_user32_get_mouse_pos(&mx, &my);
        if (mx <= 2 && my <= 2) {
            /* Default right on SINGLE PLAYER for instant handheld navigation */
            g_vcx = g_sw * 0.16f;
            g_vcy = g_sh * 0.45f;
        } else {
            g_vcx = (float)mx;
            g_vcy = (float)my;
        }
        g_vcursor_vis = 1;
        int ix = (int)g_vcx, iy = (int)g_vcy;
        w64_user32_mouse_pos(ix, iy);
        w64_user32_post_input(0x0200, 0, (uint64_t)(uint16_t)ix | ((uint64_t)(uint16_t)iy << 16));
    }
}
static void vcursor_tick(float dt)
{
    int shown = w64_user32_cursor_shown();
    uint64_t now = w64_now_ns();
    if (!shown) {
        g_vcursor_vis = 0;
        if (!g_hidden_since_ns) g_hidden_since_ns = now;
        else if (now - g_hidden_since_ns > 1500000000ull) g_in_gameplay = 1;
        return;
    }
    g_hidden_since_ns = 0;
    if (g_sw <= 0 || g_sh <= 0) return;

    /* In any menu (Title Screen, Options, Pause): Right Stick moves an optional visible mouse cursor
     * (useful for Options sliders & Title Screen) without interfering with native Left-Stick / D-Pad / A
     * XInput navigation! Moving Left Stick or pressing D-Pad hides the virtual cursor so native XInput A works. */
    pthread_mutex_lock(&g_pad_mu);
    int16_t rx = g_hw.rx, ry = g_hw.ry, lx = g_hw.lx, ly = g_hw.ly;
    uint16_t btns = g_hw.buttons;
    pthread_mutex_unlock(&g_pad_mu);

    float dx = 0.0f, dy = 0.0f;
    if (rx || ry) {
        dx = (float)rx / 32767.0f;
        dy = -(float)ry / 32767.0f;
    } else {
        if (lx || ly || (btns & 0x0Fu)) g_vcursor_vis = 0;
        return;
    }
    vcursor_init_pos();
    float speed = (float)g_sw * 0.55f;
    g_vcx += dx * speed * dt;
    g_vcy += dy * speed * dt;
    if (g_vcx < 0.0f) g_vcx = 0.0f;
    if (g_vcx > (float)(g_sw - 1)) g_vcx = (float)(g_sw - 1);
    if (g_vcy < 0.0f) g_vcy = 0.0f;
    if (g_vcy > (float)(g_sh - 1)) g_vcy = (float)(g_sh - 1);
    int ix = (int)g_vcx, iy = (int)g_vcy;
    w64_user32_mouse_pos(ix, iy);
    w64_user32_raw_mouse(dx > 0.0f ? 1 : (dx < 0.0f ? -1 : 0), dy > 0.0f ? 1 : (dy < 0.0f ? -1 : 0), 0, 0);
    w64_user32_post_input(0x0200, 0, (uint64_t)(uint16_t)ix | ((uint64_t)(uint16_t)iy << 16));
}
static void gamepad_event(const SDL_Event *e)
{
    if (g_java_hwpad_active) return;
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad *p = SDL_OpenGamepad(e->gdevice.which);
        if (p) {
            int stored = 0;
            for (int i = 0; i < MAX_HW_PADS; i++) {
                if (!g_pads[i]) { g_pads[i] = p; stored = 1; break; }
            }
            w64_log("[android] gamepad %s (id=%u%s)", SDL_GetGamepadName(p) ? SDL_GetGamepadName(p) : "?",
                    (unsigned)e->gdevice.which, stored ? "" : ", pool full");
            if (!stored) SDL_CloseGamepad(p);
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (int i = 0; i < MAX_HW_PADS; i++) {
            if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e->gdevice.which) {
                SDL_CloseGamepad(g_pads[i]);
                g_pads[i] = NULL;
                pthread_mutex_lock(&g_pad_mu);
                memset(&g_hw, 0, sizeof g_hw);
                pthread_mutex_unlock(&g_pad_mu);
                break;
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        uint16_t m = xbutton(e->gbutton.button);
        if (g_pad_log_cnt < 10 && e->gbutton.down) {
            g_pad_log_cnt++;
            w64_log("[android] sdl-pad btn=%d mask=%#x cursor=%d in_game=%d vcur=%d",
                    (int)e->gbutton.button, (unsigned)m, w64_user32_cursor_shown(), g_in_gameplay, g_vcursor_vis);
        }
        if (w64_user32_cursor_shown() && g_vcursor_vis && e->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
            if (e->gbutton.down) spawn_click((int)g_vcx, (int)g_vcy);
            return;
        }
        pthread_mutex_lock(&g_pad_mu);
        if (e->gbutton.down) g_hw.buttons |= m; else g_hw.buttons &= (uint16_t)~m;
        pthread_mutex_unlock(&g_pad_mu);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int16_t v = e->gaxis.value;
        pthread_mutex_lock(&g_pad_mu);
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: g_hw.lx = deadzone(v); break;
        case SDL_GAMEPAD_AXIS_LEFTY: g_hw.ly = deadzone(inv(v)); break;
        case SDL_GAMEPAD_AXIS_RIGHTX: g_hw.rx = deadzone(v); break;
        case SDL_GAMEPAD_AXIS_RIGHTY: g_hw.ry = deadzone(inv(v)); break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: g_hw.lt = (uint8_t)(v > 1000 ? v >> 7 : 0); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: g_hw.rt = (uint8_t)(v > 1000 ? v >> 7 : 0); break;
        }
        pthread_mutex_unlock(&g_pad_mu);
        break;
    }
    default: return;
    }
    pad_publish();
}

/* ---- on-screen pad (PadView.java) & direct Android hardware gamepad (ForestActivity.java) ---- */
#include <jni.h>
JNIEXPORT void JNICALL Java_ch_plum_theforest_ForestActivity_nativeSetHwPad(JNIEnv *env, jclass cls, jint buttons, jint lt, jint rt,
                                                                            jint lx, jint ly, jint rx, jint ry)
{
    (void)env; (void)cls;
    g_java_hwpad_active = 1;
    uint16_t b = (uint16_t)buttons;
    uint16_t newly_pressed = (uint16_t)(b & ~g_prev_hw_buttons);
    g_prev_hw_buttons = b;
    int shown = w64_user32_cursor_shown();
    if (g_pad_log_cnt < 15 && (newly_pressed || (g_pad_log_cnt < 3 && (lx || ly || rx || ry || lt || rt)))) {
        g_pad_log_cnt++;
        w64_log("[android] hwpad btn=%#x lt=%d rt=%d L=(%d,%d) R=(%d,%d) cur=%d vcur=%d",
                (unsigned)b, (int)lt, (int)rt, (int)lx, (int)ly, (int)rx, (int)ry, shown, g_vcursor_vis);
    }
    if (shown && g_vcursor_vis && (b & 0x1000u)) {
        if (newly_pressed & 0x1000u) spawn_click((int)g_vcx, (int)g_vcy);
        b &= (uint16_t)~0x1000u;
    }
    pthread_mutex_lock(&g_pad_mu);
    g_hw.buttons = b; g_hw.lt = (uint8_t)lt; g_hw.rt = (uint8_t)rt;
    g_hw.lx = (int16_t)lx; g_hw.ly = (int16_t)ly; g_hw.rx = (int16_t)rx; g_hw.ry = (int16_t)ry;
    pthread_mutex_unlock(&g_pad_mu);
    pad_publish();
}
JNIEXPORT void JNICALL Java_ch_plum_theforest_ForestActivity_nativeSetPad(JNIEnv *env, jclass cls, jint buttons, jint lt, jint rt,
                                                                          jint lx, jint ly, jint rx, jint ry)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&g_pad_mu);
    g_vpad.buttons = (uint16_t)buttons; g_vpad.lt = (uint8_t)lt; g_vpad.rt = (uint8_t)rt;
    g_vpad.lx = (int16_t)lx; g_vpad.ly = (int16_t)ly; g_vpad.rx = (int16_t)rx; g_vpad.ry = (int16_t)ry;
    pthread_mutex_unlock(&g_pad_mu);
    pad_publish();
}
JNIEXPORT void JNICALL Java_ch_plum_theforest_ForestActivity_nativeLook(JNIEnv *env, jclass cls, jfloat dx, jfloat dy)
{
    (void)env; (void)cls;
    static float rx, ry;
    rx += dx * (float)g_sw; ry += dy * (float)g_sh;
    int ix = (int)rx, iy = (int)ry;
    rx -= (float)ix; ry -= (float)iy;
    if (ix || iy) w64_user32_raw_mouse(ix, iy, 0, 0);
}
JNIEXPORT jboolean JNICALL Java_ch_plum_theforest_ForestActivity_nativeCursorShown(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return w64_user32_cursor_shown() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jfloat JNICALL Java_ch_plum_theforest_ForestActivity_nativeVCursorX(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (g_vcursor_vis && g_sw > 0) ? (g_vcx / (float)g_sw) : -1.0f;
}
JNIEXPORT jfloat JNICALL Java_ch_plum_theforest_ForestActivity_nativeVCursorY(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (g_vcursor_vis && g_sh > 0) ? (g_vcy / (float)g_sh) : -1.0f;
}
JNIEXPORT void JNICALL Java_ch_plum_theforest_ForestActivity_nativeTap(JNIEnv *env, jclass cls, jfloat x, jfloat y)
{
    (void)env; (void)cls;
    spawn_click((int)(x * g_sw), (int)(y * g_sh));
}

/* ---- touch -> mouse ---- */
#define WM_MOUSEMOVE   0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP   0x0202
static SDL_FingerID g_finger;
static int g_finger_down, g_lx, g_ly, g_down_x, g_down_y, g_menu_touch, g_menu_drag;
static unsigned long g_down_frame;
static uint64_t mlp(int x, int y) { return (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16); }
static int g_touch_log_cnt;
static void touch(const SDL_TouchFingerEvent *f, int kind)
{
    int x = (int)(f->x * g_sw), y = (int)(f->y * g_sh);
    if (kind == SDL_EVENT_FINGER_DOWN) {
        if (g_finger_down) return;
        g_finger = f->fingerID; g_finger_down = 1;
        g_down_x = x; g_down_y = y; g_menu_drag = 0;
        g_vcursor_vis = 0;
        g_menu_touch = w64_user32_cursor_shown();
        if (g_menu_touch) {
            /* NGUI UICamera.ProcessMouse() ignores clicks in Controller scheme if mouse delta == 0,
             * and cancels OnClick if delta > clickThreshold on the press frame. Settle hover for 1 frame first! */
            if (x == g_lx && y == g_ly) {
                int nx = x > 0 ? x - 1 : x + 1;
                w64_user32_mouse_pos(nx, y);
                w64_user32_raw_mouse(nx - x, 0, 0, 0);
                w64_user32_post_input(WM_MOUSEMOVE, 0, mlp(nx, y));
                w64_user32_hold_frames(1);
            }
            w64_user32_mouse_pos(x, y);
            w64_user32_raw_mouse(x != g_lx ? (x - g_lx) : 1, y - g_ly, 0, 0);
            w64_user32_post_input(WM_MOUSEMOVE, 0, mlp(x, y));
            w64_user32_hold_frames(1);
        } else {
            w64_user32_mouse_pos(x, y);
            w64_user32_post_input(WM_MOUSEMOVE, 0, mlp(x, y));
            w64_user32_raw_mouse(x - g_lx, y - g_ly, 0, 0);
        }
        w64_user32_key_state(1 /* VK_LBUTTON */, 1);
        w64_user32_raw_mouse(0, 0, 0x0001 /* RI_MOUSE_LEFT_BUTTON_DOWN */, 0);
        w64_user32_post_input(WM_LBUTTONDOWN, 1, mlp(x, y));
        g_down_frame = hp_present_count();
        if (g_touch_log_cnt < 10) {
            g_touch_log_cnt++;
            w64_log("[android] touch down (%d,%d) menu=%d f=%lu", x, y, g_menu_touch, g_down_frame);
        }
    } else if (f->fingerID == g_finger && g_finger_down) {
        w64_user32_mouse_pos(x, y);
        if (kind == SDL_EVENT_FINGER_MOTION) {
            w64_user32_raw_mouse(x - g_lx, y - g_ly, 0, 0);
            w64_user32_post_input(WM_MOUSEMOVE, 1, mlp(x, y));
        } else {
            g_finger_down = 0;
            if (hp_present_count() < g_down_frame + 2) w64_user32_hold_frames(2);
            w64_user32_key_state(1, 0);
            w64_user32_raw_mouse(0, 0, 0x0002 /* RI_MOUSE_LEFT_BUTTON_UP */, 0);
            w64_user32_post_input(WM_LBUTTONUP, 0, mlp(x, y));
            if (g_touch_log_cnt < 10) {
                g_touch_log_cnt++;
                w64_log("[android] touch up (%d,%d) f=%lu", x, y, hp_present_count());
            }
        }
    } else return;
    g_lx = x; g_ly = y;
}

static int tf_input_vk_from_scancode(int sc)
{
    if (sc >= 4 && sc <= 29) return 'A' + (sc - 4);
    if (sc >= 30 && sc <= 38) return '1' + (sc - 30);
    if (sc == 39) return '0';
    if (sc >= 58 && sc <= 69) return 0x70 + (sc - 58);
    switch (sc) {
    case 40: return 0x0D; case 41: return 0x1B; case 42: return 0x08; case 43: return 0x09; case 44: return 0x20;
    case 45: return 0xBD; case 46: return 0xBB; case 47: return 0xDB; case 48: return 0xDD; case 49: return 0xDC;
    case 51: return 0xBA; case 52: return 0xDE; case 53: return 0xC0; case 54: return 0xBC; case 55: return 0xBE;
    case 56: return 0xBF; case 57: return 0x14;
    case 73: return 0x2D; case 74: return 0x24; case 75: return 0x21; case 76: return 0x2E; case 77: return 0x23;
    case 78: return 0x22; case 79: return 0x27; case 80: return 0x25; case 81: return 0x28; case 82: return 0x26;
    case 224: case 228: return 0x11;
    case 225: case 229: return 0x10;
    case 226: case 230: return 0x12;
    }
    return 0;
}

static void key(const SDL_KeyboardEvent *k)
{
    int vk = tf_input_vk_from_scancode(k->scancode);
    if (vk) w64_user32_key(vk, k->type == SDL_EVENT_KEY_DOWN);
}

__attribute__((visibility("default"))) int SDL_main(int argc, char *argv[])
{
    {
        stack_t ss;
        ss.ss_sp = malloc(65536);
        ss.ss_size = 65536;
        ss.ss_flags = 0;
        if (ss.ss_sp) sigaltstack(&ss, NULL);
    }
    env_from_property();
    snprintf(w64_gamedir, sizeof w64_gamedir, "%s", argc > 1 ? argv[1] : "/sdcard/Android/data/ch.plum.theforest/files/game");
    snprintf(w64_userdir, sizeof w64_userdir, "%s", argc > 2 ? argv[2] : "/sdcard/Android/data/ch.plum.theforest/files/user");
    mkdir(w64_userdir, 0777);
    setenv("HOME", w64_userdir, 0);
    setenv("XDG_DATA_HOME", w64_userdir, 0);
    setenv("XDG_CONFIG_HOME", w64_userdir, 0);
    setenv("DXVK_STATE_CACHE_PATH", w64_userdir, 0);
    char bcl[1200];
    snprintf(bcl, sizeof bcl, "%s/mono_bcl", w64_userdir);
    setenv("W64_MONO_BCL", bcl, 0);
    char logp[1200];
    snprintf(logp, sizeof logp, "%s/runtime.log", w64_userdir);
    unlink(logp);
    setenv("W64_LOGFILE", logp, 0);
    if (getenv("W64_TRACE")) w64_trace = atoi(getenv("W64_TRACE"));
    { extern int g_eh_trace; if (getenv("W64_EH_TRACE")) g_eh_trace = 1; }
    if (chdir(w64_gamedir)) {}

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) { w64_log("[android] SDL_Init: %s", SDL_GetError()); return 1; }
    g_win = SDL_CreateWindow("The Forest", 0, 0, SDL_WINDOW_FULLSCREEN);
    if (!g_win) { w64_log("[android] SDL_CreateWindow: %s", SDL_GetError()); return 1; }
    SDL_GetWindowSizeInPixels(g_win, &g_sw, &g_sh);
    const char *rs = getenv("W64_RENDER_SIZE");
    if (rs) sscanf(rs, "%dx%d", &g_sw, &g_sh);
    if (g_sw < g_sh) { int t = g_sw; g_sw = g_sh; g_sh = t; }
    if (rs) { g_sw &= ~15; g_sh &= ~15; }
    w64_user32_set_screen(g_sw, g_sh);
    hp_wsi_set_screen((uint32_t)g_sw, (uint32_t)g_sh);
    char scr[32];
    snprintf(scr, sizeof scr, "%dx%d", g_sw, g_sh);
    setenv("W64_SCREEN", scr, 1);
    if (!getenv("W64_ARGS")) {
        static char args[512];
        snprintf(args, sizeof args, "-screen-fullscreen 0 -popupwindow -screen-width %d -screen-height %d "
                 "-logFile C:\\Users\\Player\\player.log", g_sw, g_sh);
        setenv("W64_ARGS", args, 1);
    }
    w64_log("[android] game %s user %s bcl %s screen %dx%d args %s",
            w64_gamedir, w64_userdir, bcl, g_sw, g_sh, getenv("W64_ARGS"));
    attach_surface();
    SDL_AddEventWatch(app_lifecycle_watch, NULL);
    pad_publish();
    { void w64_user32_script_start(void); w64_user32_script_start(); }

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, game_thread, NULL)) { w64_log("[android] pthread_create failed"); return 1; }

    uint64_t last_tick = w64_now_ns();
    for (;;) {
        SDL_Event e;
        int wait_ms = w64_user32_cursor_shown() ? 16 : 100;
        int got = SDL_WaitEventTimeout(&e, wait_ms);
        uint64_t now = w64_now_ns();
        float dt = (float)(now - last_tick) * 1e-9f;
        if (dt > 0.05f) dt = 0.05f;
        if (dt >= 0.012f) { last_tick = now; vcursor_tick(dt); }
        if (!got) continue;
        switch (e.type) {
        case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP:
            touch(&e.tfinger, e.type);
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
            key(&e.key);
            break;
        case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_REMOVED: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            gamepad_event(&e);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_EXPOSED:
            if (!hp_wsi_has_window()) {
                attach_surface();
                if (hp_wsi_has_window()) {
                    hp_wsi_pause(0, 0);
                    w64_user32_post_input(WM_ACTIVATEAPP, 1, 0);
                }
            }
            break;
        case SDL_EVENT_TERMINATING:
        case SDL_EVENT_QUIT:
            w64_shutdown(0);
            break;
        default:
            break;
        }
    }
    return 0;
}
