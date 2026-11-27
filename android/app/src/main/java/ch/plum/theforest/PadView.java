package ch.plum.theforest;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen gamepad over the game surface. Drives XInput pad 0 (ForestActivity.nativeSetPad)
 * plus touch-look camera rotation (ForestActivity.nativeLook) and short-tap clicks.
 */
public class PadView extends View {
    static final int A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000, LB = 0x100, RB = 0x200,
            START = 0x10, BACK = 0x20, LS = 0x40, RS = 0x80;
    static final int LT = 1 << 16, RT = 1 << 17;

    private static final class Btn {
        final String label; final int bits; final RectF r = new RectF(); final boolean round;
        Btn(String label, int bits, boolean round) { this.label = label; this.bits = bits; this.round = round; }
    }

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Btn[] btns = {
        new Btn("A", A, true), new Btn("B", B, true), new Btn("X", X, true), new Btn("Y", Y, true),
        new Btn("LB", LB, false), new Btn("RB", RB, false), new Btn("LT", LT, false), new Btn("RT", RT, false),
        new Btn("Back", BACK, false), new Btn("Start", START, false),
        new Btn("LS", LS, true), new Btn("RS", RS, true),
    };
    private final RectF toggle = new RectF();
    private boolean shown = !ForestActivity.hasHwPad(), autoHidden = false, hwHiddenApplied = ForestActivity.hasHwPad();
    private float lookX, lookY, vcx = -1f, vcy = -1f;
    static final float LOOK_GAIN = 1.0f;
    private final Runnable cursorPoll = new Runnable() {
        @Override public void run() {
            boolean hide;
            float nx = -1f, ny = -1f;
            try {
                hide = ForestActivity.nativeCursorShown();
                if (hide) {
                    nx = ForestActivity.nativeVCursorX();
                    ny = ForestActivity.nativeVCursorY();
                }
            } catch (UnsatisfiedLinkError e) { hide = false; }
            boolean redraw = false;
            if (!hwHiddenApplied && ForestActivity.hasHwPad()) {
                hwHiddenApplied = true;
                if (shown) { shown = false; release(); redraw = true; }
            }
            if (hide != autoHidden) {
                autoHidden = hide;
                if (hide) release();
                redraw = true;
            }
            if (nx != vcx || ny != vcy) {
                vcx = nx; vcy = ny;
                redraw = true;
            }
            if (redraw) invalidate();
            postDelayed(this, (nx >= 0f) ? 33 : 200);
        }
    };
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); post(cursorPoll); }
    @Override protected void onDetachedFromWindow() { removeCallbacks(cursorPoll); super.onDetachedFromWindow(); }
    private boolean active() { return shown && !autoHidden; }
    private final SparseArray<Integer> owner = new SparseArray<>();
    private float stickCx, stickCy, stickR, lx, ly, rx, ry, rsX0, rsY0;
    private long rsT0;
    private boolean rsMoved;
    private int held;

    public PadView(Context c) {
        super(c);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(3f);
        stroke.setColor(Color.argb(170, 255, 255, 255));
        text.setColor(Color.argb(220, 255, 255, 255));
        text.setTextAlign(Paint.Align.CENTER);
    }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        float u = Math.min(w, h) / 12f;
        stickR = u * 1.6f; stickCx = u * 2.4f; stickCy = h - u * 2.6f;
        float cx = w - u * 2.6f, cy = h - u * 2.8f, b = u * 0.75f, d = u * 1.25f;
        btns[0].r.set(cx - b, cy + d - b, cx + b, cy + d + b);
        btns[1].r.set(cx + d - b, cy - b, cx + d + b, cy + b);
        btns[2].r.set(cx - d - b, cy - b, cx - d + b, cy + b);
        btns[3].r.set(cx - b, cy - d - b, cx + b, cy - d + b);
        float sw = u * 2.2f, sh = u * 0.9f;
        btns[4].r.set(u * 0.6f, h - u * 6.6f, u * 0.6f + sw, h - u * 6.6f + sh);
        btns[6].r.set(u * 0.6f, h - u * 7.8f, u * 0.6f + sw, h - u * 7.8f + sh);
        btns[5].r.set(w - u * 0.6f - sw, h - u * 6.6f, w - u * 0.6f, h - u * 6.6f + sh);
        btns[7].r.set(w - u * 0.6f - sw, h - u * 7.8f, w - u * 0.6f, h - u * 7.8f + sh);
        btns[8].r.set(w / 2f - u * 2.6f, u * 0.4f, w / 2f - u * 0.4f, u * 1.2f);
        btns[9].r.set(w / 2f + u * 0.4f, u * 0.4f, w / 2f + u * 2.6f, u * 1.2f);
        float sb = u * 0.55f;
        btns[10].r.set(stickCx + stickR * 1.15f - sb, stickCy - stickR * 1.05f - sb, stickCx + stickR * 1.15f + sb, stickCy - stickR * 1.05f + sb);
        btns[11].r.set(cx - d * 2.3f - sb, cy + d - sb, cx - d * 2.3f + sb, cy + d + sb);
        toggle.set(w - u * 2.0f, u * 0.4f, w - u * 0.4f, u * 1.2f);
        text.setTextSize(u * 0.45f);
    }

    @Override protected void onDraw(Canvas cv) {
        fill.setColor(Color.argb(90, 0, 0, 0));
        cv.drawRoundRect(toggle, 12, 12, fill);
        cv.drawRoundRect(toggle, 12, 12, stroke);
        cv.drawText(shown ? "Pad ✕" : "Pad", toggle.centerX(), toggle.centerY() + text.getTextSize() / 3, text);
        if (vcx >= 0f && vcy >= 0f) {
            float px = vcx * getWidth(), py = vcy * getHeight();
            float r = Math.max(12f, Math.min(getWidth(), getHeight()) * 0.014f);
            fill.setColor(Color.argb(190, 255, 224, 64));
            cv.drawCircle(px, py, r * 0.45f, fill);
            fill.setColor(Color.argb(80, 0, 0, 0));
            cv.drawCircle(px, py, r, fill);
            cv.drawCircle(px, py, r, stroke);
        }
        if (!active()) return;
        fill.setColor(Color.argb(60, 255, 255, 255));
        cv.drawCircle(stickCx, stickCy, stickR, fill);
        cv.drawCircle(stickCx, stickCy, stickR, stroke);
        fill.setColor(Color.argb(140, 255, 255, 255));
        cv.drawCircle(stickCx + lx * stickR, stickCy - ly * stickR, stickR * 0.4f, fill);
        for (Btn b : btns) {
            boolean down = (held & b.bits) != 0;
            fill.setColor(down ? Color.argb(150, 140, 210, 140) : Color.argb(70, 0, 0, 0));
            if (b.round) { cv.drawOval(b.r, fill); cv.drawOval(b.r, stroke); }
            else { cv.drawRoundRect(b.r, 14, 14, fill); cv.drawRoundRect(b.r, 14, 14, stroke); }
            cv.drawText(b.label, b.r.centerX(), b.r.centerY() + text.getTextSize() / 3, text);
        }
    }

    private int hit(float x, float y) {
        if (toggle.contains(x, y)) return -3;
        if (!active()) return Integer.MIN_VALUE;
        float dx = x - stickCx, dy = y - stickCy;
        if (dx * dx + dy * dy < stickR * stickR * 2.2f) return -1;
        for (int i = 0; i < btns.length; i++) {
            RectF r = new RectF(btns[i].r);
            r.inset(-r.width() * 0.15f, -r.height() * 0.15f);
            if (r.contains(x, y)) return i;
        }
        if (x > getWidth() * 0.45f && y > getHeight() * 0.25f) return -2;
        return Integer.MIN_VALUE;
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(), idx = e.getActionIndex(), id = e.getPointerId(idx);
        switch (a) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: {
            int h = hit(e.getX(idx), e.getY(idx));
            if (h == Integer.MIN_VALUE) return a != MotionEvent.ACTION_DOWN || owner.size() > 0;
            if (h == -3) { shown = !shown; release(); invalidate(); return true; }
            owner.put(id, h);
            if (h == -2) { rsX0 = lookX = e.getX(idx); rsY0 = lookY = e.getY(idx); rsT0 = e.getEventTime(); rsMoved = false; }
            if (h >= 0) { held |= btns[h].bits; send(); }
            move(e);
            return true;
        }
        case MotionEvent.ACTION_MOVE:
            move(e);
            return true;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: case MotionEvent.ACTION_CANCEL: {
            Integer h = owner.get(id);
            owner.remove(id);
            if (a == MotionEvent.ACTION_CANCEL || a == MotionEvent.ACTION_UP) owner.clear();
            if (h != null) {
                if (h == -1) { lx = 0; ly = 0; }
                if (h == -2) {
                    if (!rsMoved && e.getEventTime() - rsT0 < 300)
                        ForestActivity.nativeTap(e.getX(idx) / getWidth(), e.getY(idx) / getHeight());
                }
                if (h >= 0) held &= ~btns[h].bits;
            }
            if (owner.size() == 0) { held = 0; lx = ly = rx = ry = 0; }
            send();
            return true;
        }
        }
        return true;
    }

    private void move(MotionEvent e) {
        boolean padChanged = false;
        for (int i = 0; i < e.getPointerCount(); i++) {
            Integer h = owner.get(e.getPointerId(i));
            if (h == null) continue;
            float x = e.getX(i), y = e.getY(i);
            if (h == -1) {
                float dx = (x - stickCx) / stickR, dy = (y - stickCy) / stickR, m = (float) Math.hypot(dx, dy);
                if (m > 1) { dx /= m; dy /= m; }
                if (dx != lx || -dy != ly) { lx = dx; ly = -dy; padChanged = true; }
            } else if (h == -2) {
                if (Math.hypot(x - rsX0, y - rsY0) > stickR * 0.15f) rsMoved = true;
                if (rsMoved) ForestActivity.nativeLook((x - lookX) * LOOK_GAIN / getWidth(), (y - lookY) * LOOK_GAIN / getHeight());
                lookX = x; lookY = y;
            }
        }
        if (padChanged) send();
    }

    private void release() { owner.clear(); held = 0; lx = ly = rx = ry = 0; send(); }

    private void send() {
        ForestActivity.nativeSetPad(held & 0xFFFF, (held & LT) != 0 ? 255 : 0, (held & RT) != 0 ? 255 : 0,
                (int) (lx * 32767), (int) (ly * 32767), (int) (rx * 32767), (int) (ry * 32767));
        invalidate();
    }
}
