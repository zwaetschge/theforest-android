package ch.plum.theforest;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * Runs the recompiled game: SDL3 hosts the window, input and audio; libmain.so is the game
 * (Win64 runtime, lifted TheForest.exe + plugins, native Unity Mono 4.8, Direct3D 11 via DXVK).
 *
 * Physical gamepads (AYN Odin 3 built-in Xbox Wireless Controller, Bluetooth Xbox/PlayStation/Switch
 * pads, USB controllers) are read directly via Android KeyEvent/MotionEvent and mapped 1:1 to XInput
 * slot 0, bypassing SDL3's Android axis-index sorting quirks and view-focus requirements.
 */
public class ForestActivity extends SDLActivity {
    static final int RENDER_MAX = 1280;
    private static final float STICK_DEADZONE = 0.12f;

    private static volatile boolean sHwPadDetected;
    private int hwButtons, hwHatButtons;
    private int hwLt, hwRt;
    private boolean hwLtBtn, hwRtBtn;
    private int hwLx, hwLy, hwRx, hwRy;

    static boolean hasHwPad() { return sHwPadDetected; }

    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override protected String[] getArguments() {
        File user = Launcher.userDir(this);
        Launcher.ensureMonoBcl(this, user);
        QualityPreset.ensureDefault(this, user);
        return new String[] { Launcher.gameDir(this).getAbsolutePath(), user.getAbsolutePath() };
    }

    static native void nativeSetPad(int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    static native void nativeSetHwPad(int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    static native void nativeTap(float x, float y);
    static native void nativeLook(float dx, float dy);
    static native boolean nativeCursorShown();
    static native float nativeVCursorX();
    static native float nativeVCursorY();

    private static boolean detectPhysicalGamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null) continue;
            int src = d.getSources();
            boolean isPad = (src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
            if (isPad) {
                Log.i("TheForest", "physical gamepad detected: " + d.getName() + " (id=" + id + " src=0x" + Integer.toHexString(src) + ")");
                return true;
            }
        }
        return false;
    }

    private static boolean isFromGamepad(InputDevice d, int source) {
        int s = source;
        if (d != null) s |= d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static int mapGamepadKey(int keyCode) {
        switch (keyCode) {
        case KeyEvent.KEYCODE_BUTTON_A: return 0x1000;
        case KeyEvent.KEYCODE_BUTTON_B: return 0x2000;
        case KeyEvent.KEYCODE_BUTTON_X: return 0x4000;
        case KeyEvent.KEYCODE_BUTTON_Y: return 0x8000;
        case KeyEvent.KEYCODE_BUTTON_L1: return 0x0100;
        case KeyEvent.KEYCODE_BUTTON_R1: return 0x0200;
        case KeyEvent.KEYCODE_BUTTON_START: return 0x0010;
        case KeyEvent.KEYCODE_BUTTON_SELECT:
        case KeyEvent.KEYCODE_BUTTON_MODE: return 0x0020;
        case KeyEvent.KEYCODE_BUTTON_THUMBL: return 0x0040;
        case KeyEvent.KEYCODE_BUTTON_THUMBR: return 0x0080;
        case KeyEvent.KEYCODE_DPAD_UP: return 0x0001;
        case KeyEvent.KEYCODE_DPAD_DOWN: return 0x0002;
        case KeyEvent.KEYCODE_DPAD_LEFT: return 0x0004;
        case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x0008;
        default: return 0;
        }
    }

    private static int scaleStick(float v) {
        float av = Math.abs(v);
        if (av < STICK_DEADZONE) return 0;
        float norm = (av - STICK_DEADZONE) / (1f - STICK_DEADZONE);
        if (norm > 1f) norm = 1f;
        int out = Math.round(Math.copySign(norm * 32767f, v));
        if (out > 32767) out = 32767;
        if (out < -32767) out = -32767;
        return out;
    }

    private static int scaleTrigger(float v) {
        if (v < 0.05f) return 0;
        int out = Math.round(v * 255f);
        return Math.min(255, Math.max(0, out));
    }

    private void publishHwPad() {
        sHwPadDetected = true;
        int lt = hwLtBtn ? 255 : hwLt;
        int rt = hwRtBtn ? 255 : hwRt;
        try {
            nativeSetHwPad(hwButtons | hwHatButtons, lt, rt, hwLx, hwLy, hwRx, hwRy);
        } catch (UnsatisfiedLinkError ignored) {}
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int kc = e.getKeyCode();
        boolean fromPad = isFromGamepad(e.getDevice(), e.getSource());
        if (KeyEvent.isGamepadButton(kc) || (fromPad && (kc == KeyEvent.KEYCODE_DPAD_UP || kc == KeyEvent.KEYCODE_DPAD_DOWN
                || kc == KeyEvent.KEYCODE_DPAD_LEFT || kc == KeyEvent.KEYCODE_DPAD_RIGHT || kc == KeyEvent.KEYCODE_DPAD_CENTER))) {
            boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
            if (down && e.getRepeatCount() > 0) return true;
            if (kc == KeyEvent.KEYCODE_BUTTON_L2) {
                hwLtBtn = down;
                publishHwPad();
                return true;
            }
            if (kc == KeyEvent.KEYCODE_BUTTON_R2) {
                hwRtBtn = down;
                publishHwPad();
                return true;
            }
            int bit = (kc == KeyEvent.KEYCODE_DPAD_CENTER) ? 0x1000 : mapGamepadKey(kc);
            if (bit != 0) {
                if (down) hwButtons |= bit; else hwButtons &= ~bit;
                publishHwPad();
                return true;
            }
            // Swallow unmapped gamepad keys (e.g. BUTTON_C/Z) so they don't trigger Android system actions
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_MOVE
                && (e.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0) {
            float lx = e.getAxisValue(MotionEvent.AXIS_X);
            float ly = -e.getAxisValue(MotionEvent.AXIS_Y);
            float z = e.getAxisValue(MotionEvent.AXIS_Z);
            float rxAlt = e.getAxisValue(MotionEvent.AXIS_RX);
            float rx = (Math.abs(z) >= Math.abs(rxAlt)) ? z : rxAlt;
            float rz = e.getAxisValue(MotionEvent.AXIS_RZ);
            float ryAlt = e.getAxisValue(MotionEvent.AXIS_RY);
            float ry = -((Math.abs(rz) >= Math.abs(ryAlt)) ? rz : ryAlt);
            float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
            float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
            float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X);
            float hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int hat = 0;
            if (hx < -0.5f) hat |= 0x0004;
            else if (hx > 0.5f) hat |= 0x0008;
            if (hy < -0.5f) hat |= 0x0001;
            else if (hy > 0.5f) hat |= 0x0002;
            hwLx = scaleStick(lx);
            hwLy = scaleStick(ly);
            hwRx = scaleStick(rx);
            hwRy = scaleStick(ry);
            hwLt = scaleTrigger(lt);
            hwRt = scaleTrigger(rt);
            hwHatButtons = hat;
            publishHwPad();
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    @Override protected void onCreate(Bundle b) {
        sHwPadDetected = detectPhysicalGamepad();
        super.onCreate(b);
        if (mSurface == null) return;
        if (mLayout != null)
            mLayout.addView(new PadView(this), new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        SharedPreferences prefs = getSharedPreferences(Launcher.PREFS, MODE_PRIVATE);
        if (prefs.getBoolean(Launcher.PREF_FULL_RES, false)) return;
        DisplayMetrics dm = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        int w = Math.max(dm.widthPixels, dm.heightPixels), h = Math.min(dm.widthPixels, dm.heightPixels);
        if (w <= RENDER_MAX) return;
        int sw = RENDER_MAX, sh = Math.round((float) h * RENDER_MAX / w / 16f) * 16;
        Log.i("TheForest", "render surface " + sw + "x" + sh + " (display " + w + "x" + h + ")");
        mSurface.setLayoutParams(new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        mSurface.getHolder().setFixedSize(sw, sh);
        nativeSetenv("W64_RENDER_SIZE", sw + "x" + sh);
    }
}
