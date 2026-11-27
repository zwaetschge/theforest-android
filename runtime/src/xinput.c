/*
 * xinput.c - XInput for the recompiled game: up to four pads whose state the platform layer
 * sets (SDL gamepads and the on-screen pad on Android, android_main.c; nothing on the host).
 * Unity's input system uses XInput when Windows.Gaming.Input is not there.
 */
#include "w64.h"
#include <pthread.h>

typedef struct {
    int connected;
    uint32_t packet;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
    uint16_t latched, reported;          /* see hold() */
    unsigned long down_frame[16];
} pad_t;
static pad_t g_pads[4];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* A touch on a pad button can be shorter than a frame at the game's frame rate; Unity polls XInput
 * once per frame and would miss it. A pressed button is reported for at least two presented frames
 * (the D3D side's frame counter; without one, presses are not held). */
__attribute__((weak)) unsigned long hp_present_count(void) { return 0; }
static uint16_t hold(pad_t *p)
{
    unsigned long now = hp_present_count();
    for (int b = 0; b < 16; b++)
        if ((p->latched >> b & 1) && now >= p->down_frame[b] + 2) p->latched &= (uint16_t)~(1u << b);
    uint16_t r = p->buttons | p->latched;
    if (r != p->reported) { p->reported = r; p->packet++; }
    return r;
}

/* XINPUT_GAMEPAD_* button bits: DPAD_UP 0x1, DOWN 0x2, LEFT 0x4, RIGHT 0x8, START 0x10, BACK 0x20,
 * LEFT_THUMB 0x40, RIGHT_THUMB 0x80, LEFT_SHOULDER 0x100, RIGHT_SHOULDER 0x200, A 0x1000,
 * B 0x2000, X 0x4000, Y 0x8000. Sticks -32768..32767 (up = positive Y), triggers 0..255. */
void w64_xinput_set(int idx, int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                    int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
    if (idx < 0 || idx > 3) return;
    pthread_mutex_lock(&g_mu);
    pad_t *p = &g_pads[idx];
    if (p->connected != connected || p->buttons != buttons || p->lt != lt || p->rt != rt ||
        p->lx != lx || p->ly != ly || p->rx != rx || p->ry != ry) p->packet++;
    uint16_t pressed = (uint16_t)(buttons & ~p->buttons);
    if (pressed && hp_present_count()) {
        unsigned long now = hp_present_count();
        for (int b = 0; b < 16; b++) if (pressed >> b & 1) p->down_frame[b] = now;
        p->latched |= pressed;
    }
    p->connected = connected; p->buttons = buttons; p->lt = lt; p->rt = rt;
    p->lx = lx; p->ly = ly; p->rx = rx; p->ry = ry;
    pthread_mutex_unlock(&g_mu);
}

#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define ERROR_EMPTY_ 4306u

/* XInputGetState(idx, XINPUT_STATE*): packet 0, buttons 4, lt 6, rt 7, lx 8, ly 10, rx 12, ry 14 */
static void XInputGetState_(CPU *c)
{
    uint32_t i = AU(0);
    if (i > 3) RET(ERROR_DEVICE_NOT_CONNECTED);
    pthread_mutex_lock(&g_mu);
    uint16_t buttons = hold(&g_pads[i]);
    pad_t p = g_pads[i];
    pthread_mutex_unlock(&g_mu);
    if (!p.connected) RET(ERROR_DEVICE_NOT_CONNECTED);
    uint64_t s = A(1);
    wr32(s, p.packet); wr16(s + 4, buttons); wr8(s + 6, p.lt); wr8(s + 7, p.rt);
    wr16(s + 8, (uint16_t)p.lx); wr16(s + 10, (uint16_t)p.ly); wr16(s + 12, (uint16_t)p.rx); wr16(s + 14, (uint16_t)p.ry);
    RET(0);
}
static void XInputSetState_(CPU *c) { uint32_t i = AU(0); RET(i <= 3 && g_pads[i].connected ? 0 : ERROR_DEVICE_NOT_CONNECTED); }
/* XINPUT_CAPABILITIES: Type 0, SubType 1, Flags 2, Gamepad 4 (12 bytes), Vibration 16 (4) */
static void XInputGetCapabilities_(CPU *c)
{
    uint32_t i = AU(0);
    if (i > 3 || !g_pads[i].connected) RET(ERROR_DEVICE_NOT_CONNECTED);
    uint64_t s = A(2);
    memset((void *)(uintptr_t)s, 0, 20);
    wr8(s, 1 /* XINPUT_DEVTYPE_GAMEPAD */); wr8(s + 1, 1 /* GAMEPAD */);
    wr16(s + 4, 0xF3FF); wr8(s + 6, 255); wr8(s + 7, 255);
    wr16(s + 8, 0xFFC0); wr16(s + 10, 0xFFC0); wr16(s + 12, 0xFFC0); wr16(s + 14, 0xFFC0);
    wr16(s + 16, 0xFFFF); wr16(s + 18, 0xFFFF);
    RET(0);
}
static void XInputEnable_(CPU *c) { RETV(); }
static void XInputGetKeystroke_(CPU *c) { uint32_t i = AU(0); RET(i <= 3 && g_pads[i].connected ? ERROR_EMPTY_ : ERROR_DEVICE_NOT_CONNECTED); }
/* XINPUT_BATTERY_INFORMATION: type 0 (WIRED 1), level 1 (FULL 3) */
static void XInputGetBatteryInformation_(CPU *c) { uint32_t i = AU(0); if (i > 3 || !g_pads[i].connected) RET(ERROR_DEVICE_NOT_CONNECTED); wr8(A(2), 1); wr8(A(2) + 1, 3); RET(0); }

#define XI(dll) \
    { dll, "XInputGetState", XInputGetState_ }, { dll, "XInputSetState", XInputSetState_ }, \
    { dll, "XInputGetCapabilities", XInputGetCapabilities_ }, { dll, "XInputEnable", XInputEnable_ }, \
    { dll, "XInputGetKeystroke", XInputGetKeystroke_ }, { dll, "XInputGetBatteryInformation", XInputGetBatteryInformation_ }, \
    { dll, "#100", XInputGetState_ }        /* XInputGetStateEx (guide button) */
static const w64_export_t xinput_exports[] = {
    XI("xinput1_4.dll"), XI("xinput1_3.dll"), XI("xinput9_1_0.dll"), XI("xinput1_2.dll"), XI("xinput1_1.dll"),
    { 0 }
};
__attribute__((constructor(102))) static void xinput_init(void) { w64_register(xinput_exports); }
