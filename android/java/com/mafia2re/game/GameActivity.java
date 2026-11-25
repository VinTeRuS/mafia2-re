package com.mafia2re.game;

import android.content.Context;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.view.ViewGroup;
import android.view.Gravity;
import android.util.TypedValue;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.util.Arrays;
import java.util.List;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** SDL's activity running the native port (libmain.so: the recompiled engine and its platform layer). */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        boolean v80 = new java.io.File(getApplicationInfo().nativeLibraryDir, "libmain_v80.so").exists();
        if (!modernCpu() && v80) return new String[] {"SDL2", "dxvk_d3d9", "main_v80"};
        // libmain_fast.so (built without the x86 memory-ordering emulation, make ... TSO=0) is no longer offered: the
        // game's lock-free code may race without it (build-apk.sh packs it only with WITH_FAST=1).
        return new String[] {"SDL2", "dxvk_d3d9", "main"};
    }

    /**
     * libmain.so is built for ARMv8.2 with RCpc (LDAPR keeps the emulated x86 memory ordering cheap) and LSE atomics;
     * older cores (Cortex-A53/A73, e.g. Snapdragon 6xx) get libmain_v80.so, built for ARMv8.0. Every core must have
     * both features: big.LITTLE phones list each core's features separately.
     */
    private static boolean modernCpu() {
        int cores = 0;
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/cpuinfo"))) {
            for (String line; (line = r.readLine()) != null;) {
                if (!line.startsWith("Features")) continue;
                List<String> features = Arrays.asList(line.substring(line.indexOf(':') + 1).trim().split("\\s+"));
                if (!features.contains("atomics") || !features.contains("lrcpc")) return false;
                ++cores;
            }
        } catch (IOException e) {
            return false;
        }
        return cores > 0;
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new GameSurface(context);
    }

    /**
     * The engine runs once per process: a new start of the game gets a fresh process. SDL's onDestroy waits on the UI
     * thread for the game thread to return, which the engine's shutdown can make long enough for an ANR, so a
     * finishing activity ends the process before that.
     */
    @Override
    protected void onDestroy() {
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
        super.onDestroy();
    }

    /**
     * Presents wait for the display (FIFO). On a 90 Hz screen a frame then shows for 2 or 3 refreshes (22 or 33 ms)
     * by turns, which judders even at a fair frame rate, and 60 fps cannot be even there. The game's window asks for the
     * 60 Hz mode (60 and 30 fps are whole refreshes); with the 45 fps limit it asks for 90 Hz (45 is every second one).
     */
    private void pickRefreshRate() {
        android.view.Display display = getWindowManager().getDefaultDisplay();
        if (display == null) return;
        // the settings panel stores the index of the choice: Нет, 30, 45, 60
        int limitIndex = getSharedPreferences("controls", Context.MODE_PRIVATE).getInt("fps_limit", 0);
        float want = limitIndex == 2 ? 90f : 60f;
        android.view.Display.Mode current = display.getMode();
        android.view.Display.Mode best = null;
        for (android.view.Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
            if (best == null || Math.abs(m.getRefreshRate() - want) < Math.abs(best.getRefreshRate() - want)) best = m;
        }
        if (best == null) return;
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(lp);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The native side loads the bundled Vulkan driver (Turnip, on Adreno) from the extracted libraries.
        try {
            Os.setenv("MAFIA2RE_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("MAFIA2RE_CREDIT", SetupActivity.CREDIT, true);
            java.io.File files = getExternalFilesDir(null);
            if (files != null) Os.setenv("MAFIA2RE_DATA_DIR", files.getAbsolutePath(), true);
        } catch (ErrnoException ignored) {
            // the system Vulkan driver is used
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);  // no dimming while playing
        pickRefreshRate();
        // On-screen controls above SDL's surface; they feed SDL keyboard and mouse input.
        addContentView(new TouchControls(this), new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // The author's credit: on start, and small in a corner for the whole game (it lets touches through).
        Toast.makeText(this, SetupActivity.CREDIT, Toast.LENGTH_LONG).show();
        TextView mark = new TextView(this);
        mark.setText(SetupActivity.CREDIT);
        mark.setTextSize(TypedValue.COMPLEX_UNIT_SP, 8);
        mark.setTextColor(0x99ffffff);
        mark.setShadowLayer(3, 1, 1, 0xcc000000);
        mark.setClickable(false);
        mark.setFocusable(false);
        int pad = (int) (6 * getResources().getDisplayMetrics().density);
        mark.setPadding(pad, pad, pad, pad);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        addContentView(mark, lp);
    }

    @Override
    public void onBackPressed() {
        // the game's own menu / back, instead of leaving the app
        org.libsdl.app.SDLActivity.onNativeKeyDown(android.view.KeyEvent.KEYCODE_ESCAPE);
        org.libsdl.app.SDLActivity.onNativeKeyUp(android.view.KeyEvent.KEYCODE_ESCAPE);
    }
}
