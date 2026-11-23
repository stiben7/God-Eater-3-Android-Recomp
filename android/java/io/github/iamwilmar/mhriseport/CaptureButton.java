package io.github.iamwilmar.mhriseport;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Toast;

import java.io.File;

// The floating capture button (Debug → Capture button): a small round button in a window of its own above the touch
// controls, like a screen recorder's. Tap: capture the next frame. Hold: save the report zip. Drag: move it; where it
// was left is remembered. Its window only covers the button, so every other touch still reaches the game.
final class CaptureButton extends View {
    static final String SWITCH = "capture_button";

    interface Host {
        File folder();
        void moved(int x, int y);  // the window follows the finger
        void tick();               // a short vibration, as the touch buttons give
    }

    private enum State { IDLE, CAPTURING, SAVING, DONE, FAILED }

    private static final long HOLD_MS = 600, CAPTURE_TIMEOUT_MS = 15000, RESULT_MS = 2500;

    private final Host host;
    private final SharedPreferences prefs;
    private final float density;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF arc = new RectF();

    private State state = State.IDLE;
    private long since;          // when the state began
    private float downX, downY;  // finger position on the screen at the press
    private int startX, startY;  // window position at the press
    private boolean dragging, held;
    int x, y;                    // window position (top left), in pixels

    CaptureButton(Context context, Host host) {
        super(context);
        this.host = host;
        density = context.getResources().getDisplayMetrics().density;
        prefs = context.getSharedPreferences("capture_button", Context.MODE_PRIVATE);
        x = prefs.getInt("x", Math.round(16 * density));
        y = prefs.getInt("y", Math.round(96 * density));
    }

    int size() {
        return Math.round(52 * density);
    }

    // ---- touch ----

    private final Runnable hold = this::onHold;

    private void onHold() {
        if (dragging || state == State.CAPTURING || state == State.SAVING) return;
        held = true;
        host.tick();
        setState(State.SAVING);
        toast("Saving the report…");
        BugReport.save(host.folder(), result -> post(() -> {
            setState(result.startsWith("Report saved") ? State.DONE : State.FAILED);
            toast(result);
        }));
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = e.getRawX();
                downY = e.getRawY();
                startX = x;
                startY = y;
                dragging = held = false;
                postDelayed(hold, HOLD_MS);
                return true;
            case MotionEvent.ACTION_MOVE: {
                float dx = e.getRawX() - downX, dy = e.getRawY() - downY;
                if (!dragging && !held && dx * dx + dy * dy > 100 * density * density) {  // (past 10 dp it's a drag)
                    dragging = true;
                    removeCallbacks(hold);
                }
                if (dragging) {
                    x = startX + Math.round(dx);
                    y = startY + Math.round(dy);
                    host.moved(x, y);
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
                removeCallbacks(hold);
                if (dragging) prefs.edit().putInt("x", x).putInt("y", y).apply();
                else if (!held) capture();
                return true;
            case MotionEvent.ACTION_CANCEL:
                removeCallbacks(hold);
                return true;
        }
        return false;
    }

    private void capture() {
        if (state == State.CAPTURING || state == State.SAVING) return;  // (one at a time: a capture takes a few frames)
        File folder = host.folder();
        if (folder == null) {
            toast("No data folder: open the game from the setup screen first.");
            return;
        }
        host.tick();
        BugReport.capture(folder);
        setState(State.CAPTURING);
        watch(new File(folder, "trazar"));
    }

    // The runtime deletes trazar once the frame is saved: that's when the capture is done.
    private void watch(File request) {
        postDelayed(() -> {
            if (state != State.CAPTURING) return;
            if (!request.exists()) {
                setState(State.DONE);
                toast("Frame captured. Hold the button to save the report.");
            } else if (SystemClock.uptimeMillis() - since > CAPTURE_TIMEOUT_MS) {
                setState(State.FAILED);
                toast("The game didn't pick up the capture. Is it still loading?");
            } else {
                watch(request);
            }
        }, 250);
    }

    private void setState(State s) {
        state = s;
        since = SystemClock.uptimeMillis();
        invalidate();
        if (s == State.DONE || s == State.FAILED)
            postDelayed(() -> {
                if (state == s && SystemClock.uptimeMillis() - since >= RESULT_MS) {
                    state = State.IDLE;
                    invalidate();
                }
            }, RESULT_MS);
        if (s == State.CAPTURING || s == State.SAVING) postDelayed(this::spin, 16);
    }

    private void spin() {
        if (state != State.CAPTURING && state != State.SAVING) return;
        invalidate();
        postDelayed(this::spin, 16);
    }

    private void toast(String text) {
        Toast.makeText(getContext(), text, Toast.LENGTH_SHORT).show();
    }

    // ---- drawing ----

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), cx = w / 2, cy = getHeight() / 2f, r = w / 2 - 2 * density;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xB0201410);
        c.drawCircle(cx, cy, r, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(2 * density);
        paint.setColor(0xFFE8D9B0);
        c.drawCircle(cx, cy, r, paint);

        int dot;
        switch (state) {
            case DONE: dot = 0xFF4CC46A; break;
            case FAILED: dot = 0xFF8A8A8A; break;
            case SAVING: dot = 0xFF4A9BE0; break;
            default: dot = 0xFFE03A30; break;
        }
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(dot);
        c.drawCircle(cx, cy, r * 0.45f, paint);

        if (state == State.CAPTURING || state == State.SAVING) {  // a turning arc while it works
            float a = ((SystemClock.uptimeMillis() - since) % 1000) * 0.36f;
            float ar = r - 4 * density;
            arc.set(cx - ar, cy - ar, cx + ar, cy + ar);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(3 * density);
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setColor(dot);
            c.drawArc(arc, a, 100, false, paint);
        }
    }
}
