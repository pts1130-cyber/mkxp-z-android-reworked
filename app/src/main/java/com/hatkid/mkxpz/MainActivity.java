package com.hatkid.mkxpz;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.content.pm.PackageManager;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.InputDevice;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Vibrator;
import android.os.VibrationEffect;
import android.os.storage.StorageManager;
import android.os.storage.OnObbStateChangeListener;
import android.net.Uri;
import android.provider.Settings;
import android.util.Log;
import java.util.Locale;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;

import org.libsdl.app.SDLActivity;
import com.hatkid.mkxpz.gamepad.Gamepad;
import com.hatkid.mkxpz.gamepad.GamepadConfig;

public class MainActivity extends SDLActivity
{
    // This activity inherits from SDLActivity activity.
    // Put your Java-side stuff here.

    private static final String TAG = "mkxp-z[Activity]";
    private static final String GAME_PATH_DEFAULT = Environment.getExternalStorageDirectory() + "/mkxp-z";
    private static String GAME_PATH = GAME_PATH_DEFAULT;
    private static String OBB_MAIN_FILENAME;
    private static boolean DEBUG = false;

    protected boolean mStarted = false;

    private StorageManager mStorageManager;

    // In-screen gamepad
    private final Gamepad mGamepad = new Gamepad();
    private boolean mGamepadInvisible = false;

    private void runSDLThread()
    {
        if (!mStarted) {
            Log.i(TAG, "Game path: " + GAME_PATH);
        }

        mStarted = true;

        // Run (resume) native SDL thread
        if (mHasMultiWindow) {
            resumeNativeThread();
        }
    }

    OnObbStateChangeListener obbListener = new OnObbStateChangeListener()
    {
        @Override
        public void onObbStateChange(String path, int state)
        {
            super.onObbStateChange(path, state);

            Log.v(TAG, "OBB state of " + path + " changed to " + state);

            switch (state)
            {
                case OnObbStateChangeListener.MOUNTED:
                    String obbPath = mStorageManager.getMountedObbPath(path);
                    Log.v(TAG, "OBB " + path + " is mounted to " + obbPath);
                    GAME_PATH = obbPath;
                    break;

                case OnObbStateChangeListener.UNMOUNTED:
                    Log.v(TAG, "OBB " + path + " is unmounted");
                    GAME_PATH = GAME_PATH_DEFAULT;
                    break;

                default:
                    Log.e(TAG, "Failed to mount OBB " + path + ": Got state " + state);
                    break;
            }

            runSDLThread();
        }
    };

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data)
    {
        if (requestCode == 110) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
                // Close the App because the User did not allow the all files access permission to be used.
                mSingleton.finish();
            }
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);

        writeExitReasons();

        mStorageManager = (StorageManager) getSystemService(STORAGE_SERVICE);

        // Get main OBB filepath
        final String obbPrefix = "main"; // "main", "patch"
        final int obbVersion = 1;
        OBB_MAIN_FILENAME = getObbDir() + "/" + obbPrefix + "." + obbVersion + "." + getPackageName() + ".obb";

        // Get Debug flag
        try {
            ActivityInfo actInfo = getPackageManager().getActivityInfo(this.getComponentName(), PackageManager.GET_META_DATA);
            DEBUG = actInfo.metaData.getBoolean("mkxp_debug");
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(TAG, "Failed to set debug flag: " + e);
            e.printStackTrace();
        }

        // Check for all files access permission (Android 11+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                // Request all files access permission
                // TODO: AlertDialog: polite notice that mkxp-z requires All Files Access permission.
                Uri uri = Uri.parse("package:" + getPackageName());
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, uri);
                startActivityForResult(intent, 110);
            }
        }

        // Setup in-screen gamepad
        mGamepadInvisible = (isAndroidTV() || isChromebook());
        GamepadConfig gpadConfig = new GamepadConfig();
        mGamepad.init(gpadConfig, mGamepadInvisible);
        mGamepad.setOnHideListener(() -> {
            mGamepad.hideNow();
            mGamepadInvisible = true;
        });
        // v1.0.13: while a text-entry (name/search) is active, the on-screen X (cancel) button
        // also sends Escape, which is what the game's text-entry screens use to cancel.
        mGamepad.setOnKeyDownListener(key -> {
            SDLActivity.onNativeKeyDown(key);
            if (key == KeyEvent.KEYCODE_X)
                SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_ESCAPE);
        });
        mGamepad.setOnKeyUpListener(key -> {
            SDLActivity.onNativeKeyUp(key);
            if (key == KeyEvent.KEYCODE_X)
                SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_ESCAPE);
        });

        if (mLayout != null) {
            mGamepad.attachTo(this, mLayout);
        }

        arcApplyFullscreen();
    }

    // ARC: force true full-screen (cut-out area + hidden system bars) at start and
    // every time the window gets focus, so the first screen is not small/letterboxed.
    private void arcApplyFullscreen()
    {
        try {
            Window w = getWindow();
            if (Build.VERSION.SDK_INT >= 28) {
                WindowManager.LayoutParams lp = w.getAttributes();
                lp.layoutInDisplayCutoutMode = (Build.VERSION.SDK_INT >= 30)
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                w.setAttributes(lp);
            }
            w.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            View d = w.getDecorView();
            d.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            d.requestLayout();
        } catch (Throwable e) {
            Log.w(TAG, "arcApplyFullscreen failed: " + e);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus)
    {
        super.onWindowFocusChanged(hasFocus);

        if (hasFocus) {
            arcApplyFullscreen();
            Handler h = new Handler(Looper.getMainLooper());
            h.postDelayed(this::arcApplyFullscreen, 400);
            h.postDelayed(this::arcApplyFullscreen, 1500);
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig)
    {
        super.onConfigurationChanged(newConfig);

        // Rotation: re-inflate gamepad so layout / layout-port is picked up
        if (mLayout != null) {
            boolean hidden = (isAndroidTV() || isChromebook());
            mGamepad.detachFrom(mLayout);
            mGamepad.init(new GamepadConfig(), hidden);
            mGamepad.attachTo(this, mLayout);
            mGamepadInvisible = hidden;
        }
    }

    @Override
    protected void onStart()
    {
        super.onStart();

        if (!mStarted) {
            // Check for main OBB file
            if (new File(OBB_MAIN_FILENAME).exists()) {
                Log.v(TAG, "Main OBB file found, starting with main OBB mount");

                // Try to mount main OBB file
                mStorageManager.mountObb(OBB_MAIN_FILENAME, null, obbListener);
            } else {
                Log.v(TAG, "Main OBB file not found, starting without main OBB mount");

                // Run from default game directory
                runSDLThread();
            }
        } else {
            // onStart: Resume SDL thread
            runSDLThread();
        }
    }

    // ARC: why the previous runs ended (Android 11+), written to <game>/_log/engine_exit.txt; native crash / ANR traces beside it
    private void writeExitReasons()
    {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
            return;
        final Context ctx = getApplicationContext();
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
                    List<ApplicationExitInfo> list = am.getHistoricalProcessExitReasons(ctx.getPackageName(), 0, 5);
                    File dir = new File(GAME_PATH_DEFAULT + "/_log");
                    if (!dir.isDirectory() && !dir.mkdirs())
                        return;
                    SimpleDateFormat fmt = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);
                    StringBuilder sb = new StringBuilder();
                    sb.append("지난 종료 기록 (최근 순, 이번 실행 ").append(fmt.format(new Date())).append(")\n");
                    int i = 0;
                    for (ApplicationExitInfo e : list) {
                        sb.append("#").append(i).append(' ').append(fmt.format(new Date(e.getTimestamp())))
                          .append(" 이유=").append(reasonName(e.getReason())).append('(').append(e.getReason()).append(')')
                          .append(" 상태=").append(e.getStatus())
                          .append(" 중요도=").append(e.getImportance())
                          .append(" 메모리PSS=").append(e.getPss() / 1024).append("MB RSS=").append(e.getRss() / 1024).append("MB")
                          .append(" 설명=").append(e.getDescription()).append("\n");
                        if (i == 0 && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                            InputStream in = null;
                            try { in = e.getTraceInputStream(); } catch (Exception ignored) {}
                            if (in != null) {
                                String ext = e.getReason() == ApplicationExitInfo.REASON_ANR ? ".txt" : ".pb";
                                File tf = new File(dir, "engine_exit_trace" + ext);
                                OutputStream out = new FileOutputStream(tf);
                                byte[] buf = new byte[65536];
                                int n;
                                while ((n = in.read(buf)) > 0)
                                    out.write(buf, 0, n);
                                out.close();
                                in.close();
                                sb.append("   추적 기록 -> ").append(tf.getName()).append(' ').append(tf.length()).append("바이트\n");
                            }
                        }
                        i++;
                    }
                    OutputStream o = new FileOutputStream(new File(dir, "engine_exit.txt"));
                    o.write(sb.toString().getBytes("UTF-8"));
                    o.close();
                } catch (Throwable t) {
                    Log.w(TAG, "exit reasons: " + t);
                }
            }
        }).start();
    }

    private static String reasonName(int r)
    {
        switch (r) {
            case 1: return "스스로 종료";
            case 2: return "신호로 종료";
            case 3: return "메모리 부족 강제종료";
            case 4: return "자바 충돌";
            case 5: return "네이티브 충돌";
            case 6: return "응답 없음(ANR)";
            case 7: return "초기화 실패";
            case 8: return "권한 바뀜";
            case 9: return "리소스 과다";
            case 10: return "사용자 요청";
            case 11: return "사용자 중지";
            case 12: return "의존 앱 종료";
            case 13: return "기타";
            case 14: return "얼림";
            case 15: return "패키지 상태 바뀜";
            case 16: return "패키지 업데이트";
            default: return "알 수 없음";
        }
    }

    // ARC: memory pressure -> same path as onLowMemory (SDL_APP_LOWMEMORY); the engine frees spare caches
    @Override
    public void onTrimMemory(int level)
    {
        super.onTrimMemory(level);

        if (level == TRIM_MEMORY_RUNNING_LOW ||
            level == TRIM_MEMORY_RUNNING_CRITICAL ||
            level >= TRIM_MEMORY_BACKGROUND) {
            Log.v(TAG, "onTrimMemory(" + level + ")");
            if (!SDLActivity.mBrokenLibraries)
                SDLActivity.nativeLowMemory();
        }
    }

    @Override
    protected void onDestroy()
    {
        super.onDestroy();

        // HACK: Exiting the JVM (process) since Ruby does not likes when we
        // trying to re-initialize Ruby VM in mkxp-z (JNI native library)
        // that leads to segmentation fault, even we have cleanup the Ruby VM.
        System.exit(0);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent evt)
    {
        if (
            evt.getKeyCode() != KeyEvent.KEYCODE_BACK &&
            evt.getKeyCode() != KeyEvent.KEYCODE_VOLUME_UP &&
            evt.getKeyCode() != KeyEvent.KEYCODE_VOLUME_DOWN &&
            evt.getKeyCode() != KeyEvent.KEYCODE_VOLUME_MUTE
        ) {
            // Hide gamepad view on key events when visible
            if (!mGamepadInvisible) {
                mGamepad.hideView();
                mGamepadInvisible = true;
            }
        }

        if (mGamepad.processGamepadEvent(evt))
            return true;

        return super.dispatchKeyEvent(evt);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent evt)
    {
        // Show gamepad view on touch when hidden
        if (mGamepadInvisible && evt.getActionMasked() == MotionEvent.ACTION_DOWN) {
            mGamepad.showView();
            mGamepadInvisible = false;
        }

        return super.dispatchTouchEvent(evt);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent evt)
    {
        if (mGamepad.processDPadEvent(evt))
            return true;

        return super.onGenericMotionEvent(evt);
    }

    /**
     * This method is for arguments for launching native mkxp-z.
     * 
     * @return arguments for the mkxp-z
     */
    @Override
    protected String[] getArguments()
    {
        String[] args;

        if (DEBUG) {
            // Arguments in Debug mode
            args = new String[] { "debug" };
        } else {
            // Arguments in normal mode
            args = new String[] {};
        }

        return args;
    }

    /**
     * This static method is used in native mkxp-z. (see systemImpl.cpp)
     * This method returns a string of current device locale tag. (e.g. "en_US")
     * 
     * @return string of locale tag
     */
    @SuppressWarnings("unused")
    private static String getSystemLanguage()
    {
        return Locale.getDefault().toString();
    }

    /**
     * This static method is used in native mkxp-z. (see android-binding.cpp)
     * This method returns a boolean indicating that the device has a vibrator or not.
     * 
     * @return boolean
     */
    @SuppressWarnings("unused")
    private static boolean hasVibrator()
    {
        Vibrator vib = (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);
        return vib.hasVibrator();
    }

    /**
     * This static method is used in native mkxp-z. (see android-binding.cpp)
     * This method makes device vibrating with given milliseconds duration.
     * 
     * @param duration milliseconds duration of vibration
     */
    @SuppressWarnings("unused")
    private static void vibrate(int duration)
    {
        Vibrator vib = (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            vib.vibrate(VibrationEffect.createOneShot(duration, VibrationEffect.EFFECT_HEAVY_CLICK));
        } else {
            vib.vibrate(duration);
        }
    }

    /**
     * This static method is used in native mkxp-z. (see android-binding.cpp)
     * This method turns off the current device vibration.
     */
    @SuppressWarnings("unused")
    private static void vibrateStop()
    {
        Vibrator vib = (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);
        vib.cancel();
    }

    /**
     * This static method is used in native mkxp-z. (see android-binding.cpp)
     * This method returns a boolean indicating the app is in multi window mode or not.
     * (Multi-window mode supports from Android 7.0 Nougat (API 24) and higher.)
     * 
     * @param activity current MainActivity instance
     * @return boolean
     */
    @SuppressWarnings("unused")
    private static boolean inMultiWindow(Activity activity)
    {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && activity.isInMultiWindowMode();
    }
}