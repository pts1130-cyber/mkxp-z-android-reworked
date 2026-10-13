package com.hatkid.mkxpz.gamepad;

import android.annotation.SuppressLint;
import android.content.Context;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.util.AttributeSet;

/**
 * D-Pad with smooth 8-way sliding.
 * Keys are diffed (only the keys that actually change are released/pressed),
 * so sliding from UP to LEFT goes UP -> UP+LEFT -> LEFT without any gap.
 */
public class GamepadDPad extends GamepadButton
{
    public Boolean isDiagonal = true;

    // Dead zone (fraction of half size): enter / leave (hysteresis)
    private static final float DEAD_ENTER = 0.20f;
    private static final float DEAD_LEAVE = 0.12f;
    // If one axis is this many times larger than the other, the other axis is ignored
    private static final float AXIS_RATIO = 2.0f;

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

    private void update(float x, float y)
    {
        float half = Math.min(this.getWidth(), this.getHeight()) / 2f;
        float dx = x - this.getWidth() / 2f;
        float dy = y - this.getHeight() / 2f;
        float ax = Math.abs(dx);
        float ay = Math.abs(dy);

        boolean up = false, down = false, left = false, right = false;

        float dzX = half * ((kLeft || kRight) ? DEAD_LEAVE : DEAD_ENTER);
        float dzY = half * ((kUp || kDown) ? DEAD_LEAVE : DEAD_ENTER);

        boolean useX = ax >= dzX;
        boolean useY = ay >= dzY;

        if (!isDiagonal) {
            // 4-way: strongest axis only
            if (useX && ax >= ay) useY = false;
            else if (useY) useX = false;
        } else {
            // 8-way: suppress the weak axis when clearly along one axis
            if (ax > ay * AXIS_RATIO) useY = false;
            else if (ay > ax * AXIS_RATIO) useX = false;
        }

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
                apply(false, false, false, false);
                break;
        }

        return true;
    }
}
