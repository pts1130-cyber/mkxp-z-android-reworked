package com.hatkid.mkxpz.gamepad;

import android.view.KeyEvent;

public class GamepadConfig
{
    /** In-screen gamepad settings **/

    // Opacity of view elements in percentage (semi-transparent, default: 30)
    public Integer opacity = 30;

    // View elements scale in percentage (default: 100)
    public Integer scale = 100;

    // Whether use diagonal (8-way) movement on D-Pad.
    // false = 4-way with sticky direction (right for tile based RPG Maker / Essentials games)
    public Boolean diagonalMovement = false;

    /** Key bindings for each RGSS input **/
    public final Integer keycodeA = KeyEvent.KEYCODE_Z;
    public final Integer keycodeB = KeyEvent.KEYCODE_X;
    public final Integer keycodeC = KeyEvent.KEYCODE_C;
    public final Integer keycodeX = KeyEvent.KEYCODE_A;
    public final Integer keycodeY = KeyEvent.KEYCODE_S;
    public final Integer keycodeZ = KeyEvent.KEYCODE_D;
    public final Integer keycodeL = KeyEvent.KEYCODE_Q;
    public final Integer keycodeR = KeyEvent.KEYCODE_W;
    public final Integer keycodeCTRL = KeyEvent.KEYCODE_F12;
    public final Integer keycodeALT = KeyEvent.KEYCODE_F9;
    public final Integer keycodeSHIFT = KeyEvent.KEYCODE_SHIFT_LEFT;
}
