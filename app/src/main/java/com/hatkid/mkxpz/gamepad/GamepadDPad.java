package com.hatkid.mkxpz.gamepad;

import android.annotation.SuppressLint;
import android.content.Context;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.util.AttributeSet;

/**
 * On-screen D-Pad.
 *
 * isDiagonal == false (default, tile based games such as Pokemon Essentials):
 *   exactly ONE of up / down / left / right is held at any time.
 *   - nothing is pressed while the finger is inside the centre dead zone
 *   - the first direction is the nearest axis (left/right if |dx| >= |dy|, else up/down)
 *   - once a direction is held it is KEPT until the finger is clearly inside another
 *     sector (more than ~54 degrees away from the held axis).  So rolling the thumb
 *     around the 45 degree line can no longer flicker between two directions, and the
 *     character keeps walking straight instead of "going somewhere else".
 *
 * isDiagonal == true: previous 8-way behaviour (kept for reference / optional use).
 *
 * Keys are diffed: the key that is no longer wanted is released first, then the new one
 * is pressed, so two perpendicular keys are never held together in 4-way mode.
 */
public class GamepadDPad extends GamepadButton
{
    public Boolean isDiagonal = false;

    // Dead zone (fraction of half size): enter / leave (hysteresis)
    private static final float DEAD_ENTER = 0.18f;
    private static final float DEAD_LEAVE = 0.10f;
    // 4-way: switch to the perpendicular axis only when its component is this many
    // times larger than the held axis component (1.4 ~ 54.5 degrees, i.e. 9.5 degrees
    // of "stickiness" past the 45 degree line)
    private static final float SWITCH_RATIO = 1.4f;
    // 8-way: if one axis is this many times larger than the other, the other axis is ignored
    private static final float AXIS_RATIO = 2.0f;

    private static final int D_NONE = 0;
    private static final int D_UP = 1;
    private static final int D_DOWN = 2;
    private static final int D_LEFT = 3;
    private static final int D_RIGHT = 4;

    private int cur = D_NONE;
    private boolean kUp, kDown, kLeft, kRight;

    private OnKeyDownListener mOnKeyDownListener = key -> {};
    private OnKeyUpListener mOnKeyUpListener = key -> {};

    public void setOnKeyDownListener(OnKeyDownListener onKeyDownListener)
    {
        mOnKeyDownListener = onKeyDownListener;
    }

    public void setOnKeyUpListener(OnKeyUpListener onKeyUpListener)
    {
        mOnKeyUpListener = onKeyUpListener;
    }

    public GamepadDPad(Context context)
    {
        super(context);
    }

    public GamepadDPad(Context context, AttributeSet attrs)
    {
        super(context, attrs);
    }

    public GamepadDPad(Context context, AttributeSet attrs, int defStyleAttr)
    {
        super(context, attrs, defStyleAttr);
    }

    public GamepadDPad(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes)
    {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    private void apply(boolean up, boolean down, boolean left, boolean right)
    {
        // Release first, then press: no key is ever re-pressed needlessly
        if (kUp && !up) mOnKeyUpListener.onKeyUp(KeyEvent.KEYCODE_DPAD_UP);
        if (kDown && !down) mOnKeyUpListener.onKeyUp(KeyEvent.KEYCODE_DPAD_DOWN);
        if (kLeft && !left) mOnKeyUpListener.onKeyUp(KeyEvent.KEYCODE_DPAD_LEFT);
        if (kRight && !right) mOnKeyUpListener.onKeyUp(KeyEvent.KEYCODE_DPAD_RIGHT);

        if (!kUp && up) mOnKeyDownListener.onKeyDown(KeyEvent.KEYCODE_DPAD_UP);
        if (!kDown && down) mOnKeyDownListener.onKeyDown(KeyEvent.KEYCODE_DPAD_DOWN);
        if (!kLeft && left) mOnKeyDownListener.onKeyDown(KeyEvent.KEYCODE_DPAD_LEFT);
        if (!kRight && right) mOnKeyDownListener.onKeyDown(KeyEvent.KEYCODE_DPAD_RIGHT);

        kUp = up;
        kDown = down;
        kLeft = left;
        kRight = right;
    }

    private void releaseAll()
    {
        cur = D_NONE;
        apply(false, false, false, false);
    }

    /** 4-way direction with a dead zone and sticky (hysteresis) sector switching. */
    private int pickDir4(float dx, float dy, float half)
    {
        float ax = Math.abs(dx);
        float ay = Math.abs(dy);
        float r = (float) Math.sqrt(dx * dx + dy * dy);
        float dz = half * (cur == D_NONE ? DEAD_ENTER : DEAD_LEAVE);

        if (r < dz)
            return D_NONE;

        // Keep the held direction until the finger is clearly inside another sector
        switch (cur)
        {
            case D_LEFT:
                if (dx < 0 && ay < ax * SWITCH_RATIO) return D_LEFT;
                break;
            case D_RIGHT:
                if (dx > 0 && ay < ax * SWITCH_RATIO) return D_RIGHT;
                break;
            case D_UP:
                if (dy < 0 && ax < ay * SWITCH_RATIO) return D_UP;
                break;
            case D_DOWN:
                if (dy > 0 && ax < ay * SWITCH_RATIO) return D_DOWN;
                break;
            default:
                break;
        }

        // New direction: nearest axis
        if (ax >= ay)
            return dx < 0 ? D_LEFT : D_RIGHT;

        return dy < 0 ? D_UP : D_DOWN;
    }

    private void update(float x, float y)
    {
        float half = Math.min(this.getWidth(), this.getHeight()) / 2f;
        float dx = x - this.getWidth() / 2f;
        float dy = y - this.getHeight() / 2f;

        if (!isDiagonal) {
            cur = pickDir4(dx, dy, half);
            apply(cur == D_UP, cur == D_DOWN, cur == D_LEFT, cur == D_RIGHT);
            return;
        }

        // ---- 8-way (optional) ----
        float ax = Math.abs(dx);
        float ay = Math.abs(dy);

        boolean up = false, down = false, left = false, right = false;

        float dzX = half * ((kLeft || kRight) ? DEAD_LEAVE : DEAD_ENTER);
        float dzY = half * ((kUp || kDown) ? DEAD_LEAVE : DEAD_ENTER);

        boolean useX = ax >= dzX;
        boolean useY = ay >= dzY;

        // suppress the weak axis when clearly along one axis
        if (ax > ay * AXIS_RATIO) useY = false;
        else if (ay > ax * AXIS_RATIO) useX = false;

        if (useX) { left = dx < 0; right = dx > 0; }
        if (useY) { up = dy < 0; down = dy > 0; }

        apply(up, down, left, right);
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override
    public boolean onTouchEvent(MotionEvent evt)
    {
        switch (evt.getActionMasked())
        {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
                update(evt.getX(), evt.getY());
                break;

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
            case MotionEvent.ACTION_OUTSIDE:
                releaseAll();
                break;
        }

        return true;
    }

    // Never leave a key stuck down if the view goes away or the window loses focus
    @Override
    protected void onDetachedFromWindow()
    {
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override
    public void onWindowFocusChanged(boolean hasWindowFocus)
    {
        if (!hasWindowFocus)
            releaseAll();
        super.onWindowFocusChanged(hasWindowFocus);
    }
}
