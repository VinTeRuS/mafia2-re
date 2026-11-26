package com.mafia2re.game;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BlurMaskFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.view.HapticFeedbackConstants;
import android.os.Build;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * On-screen controls drawn over the game in the style of the Prototype port, in Mafia II's colours (cream rings on black, red when pressed,
 * bracket corners). Everything goes into SDL as real keyboard and mouse input, so the engine sees it both as window
 * messages and through DirectInput.
 *
 * Modes (button in the top-left corner):
 *  - menu: a touch is a left mouse click at that point (menus are mouse-driven), plus Enter and Esc;
 *  - game: a stick on the left (WASD; optionally sprints at the rim), the action buttons in an arc around the
 *    big attack button under the right thumb, dragging anywhere else turns the camera.
 * The gear opens the settings panel; from there the layout editor lets the player drag every control to where
 * their thumbs rest. The performance overlay shows FPS, frame time and memory.
 */
public class TouchControls extends View implements InputManager.InputDeviceListener {
    // Performance counters and the frame limiter of the native host (main.cpp).
    static native float nativeFps();
    static native float nativeFrameMs();
    static native int nativeMemoryMiB();
    static native void nativeSetFpsLimit(int fps);
    // m2_script.cpp: 1 a player exists, 2 in a vehicle, 4 a movie plays, 8 the player state is known
    static native int nativeGameState();

    private static final int MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_MIDDLE = 4;
    private static final int RED = 0xffe6dcc6, RED_DIM = 0xff6e6658, INK = 0xff0b0b0b;

    /**
     * A hexagonal button: a key (Android keycode) or a mouse button. Its default place is a dp offset from a corner
     * (right/bottom when the flags say so), scaled with the buttons so a cluster stays tight; the layout editor
     * replaces it with a position in fractions of the view.
     */
    private static final class Button {
        final String label;
        final int keycode, mouseButton;
        final boolean fromRight, fromBottom;
        final float dx, dy, sizeDp;
        final String icon;  // drawIcon name
        final int ctx;      // the situations it shows in: FOOT, CAR, MENU, MORE (FOOT with the "more" row open)
        float customX = -1, customY = -1;
        float size = 1;  // the layout editor's per-button size factor
        float fx, fy;
        float cx, cy, r;
        int pointer = -1;

        Button(String label, String icon, int keycode, int mouseButton, boolean fromRight, float dx, boolean fromBottom,
                float dy, float sizeDp, int ctx) {
            this.label = label;
            this.icon = icon;
            this.keycode = keycode;
            this.mouseButton = mouseButton;
            this.fromRight = fromRight;
            this.dx = dx;
            this.fromBottom = fromBottom;
            this.dy = dy;
            this.sizeDp = sizeDp;
            this.ctx = ctx;
        }

        boolean hit(float x, float y) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r * 1.25f;
        }
    }

    // Which buttons show depends on the situation, read from the game (nativeGameState): on foot, in a car, in the
    // menus. Right thumb: FIRE in the corner, an inner arc (~100 dp around it) for the frequent keys and an outer arc
    // (~180 dp) for the rarer ones; small system keys along the top right edge, left of the gear. Keys from the game's
    // manual (MANUAL.PDF, "Управление": car F enter/exit, Space handbrake, E horn, L speed limiter).
    private static final int FOOT = 1, CAR = 2, MENU = 4, MORE = 8;
    private final Button[] buttons = {
        new Button("ПАУЗА", "pause", KeyEvent.KEYCODE_ESCAPE, 0, true, 92, false, 34, 17, FOOT | CAR | MENU),
        new Button("ОК", "ok", KeyEvent.KEYCODE_ENTER, 0, true, 136, false, 34, 17, MENU),
        new Button("КАРТА", "map", KeyEvent.KEYCODE_M, 0, true, 136, false, 34, 17, FOOT | CAR),
        new Button("ОРУЖИЕ", "weapon", KeyEvent.KEYCODE_NUMPAD_8, 0, true, 180, false, 34, 17, FOOT),
        new Button("ЕЩЁ", "more", 0, 0, true, 224, false, 34, 17, FOOT),
        new Button("ГРАНАТА", "grenade", KeyEvent.KEYCODE_1, 0, true, 224, false, 78, 17, MORE),
        new Button("ПРИСЕД", "crouch", KeyEvent.KEYCODE_C, 0, true, 180, false, 78, 17, MORE),
        new Button("БЕГ", "run", KeyEvent.KEYCODE_SHIFT_LEFT, 0, true, 136, false, 78, 17, MORE),
        // on foot
        new Button("ОГОНЬ", "fire", 0, MOUSE_LEFT, true, 66, true, 66, 34, FOOT | CAR),
        new Button("ПРИЦЕЛ", "aim", 0, MOUSE_RIGHT, true, 164, true, 60, 24, FOOT | CAR),
        new Button("ДЕЙСТВИЕ", "hand", KeyEvent.KEYCODE_E, 0, true, 136, true, 136, 22, FOOT),
        new Button("ПРЫЖОК", "jump", KeyEvent.KEYCODE_SPACE, 0, true, 62, true, 162, 22, FOOT),
        new Button("СЕСТЬ", "car", KeyEvent.KEYCODE_F, 0, true, 240, true, 104, 19, FOOT),
        new Button("УКРЫТИЕ", "cover", KeyEvent.KEYCODE_CTRL_LEFT, 0, true, 182, true, 200, 19, FOOT),
        new Button("ОБОЙМА", "reload", KeyEvent.KEYCODE_R, 0, true, 96, true, 236, 17, FOOT),
        new Button("УДАР", "fist", KeyEvent.KEYCODE_Q, 0, true, 238, true, 32, 19, FOOT),
        // in a car
        new Button("РУЧНИК", "brake", KeyEvent.KEYCODE_SPACE, 0, true, 136, true, 136, 24, CAR),
        new Button("ВЫЙТИ", "exit", KeyEvent.KEYCODE_F, 0, true, 62, true, 162, 20, CAR),
        new Button("СИГНАЛ", "horn", KeyEvent.KEYCODE_E, 0, true, 240, true, 104, 17, CAR),
        new Button("ЛИМИТ", "limiter", KeyEvent.KEYCODE_L, 0, true, 182, true, 200, 17, CAR),
    };

    // ---- settings ----
    private final SharedPreferences prefs;
    private float opacity, scale;
    private boolean vibrate, showFps, hideWithPad, autoRun, invertY;
    // the situation: in a car (from the game, or toggled by F when the game cannot tell), the "more" row, a movie
    private boolean inCar, manualCar, stateKnown, lastPlayer, moreOpen, movie;
    private long showDuringMovieUntil;
    private float camGain;
    private boolean padConnected;

    private static final class Setting {
        final String label, key;     // key: preference name; engine settings also go to mafia2re.ini
        final String[] names;
        final int[] values;
        final boolean engine;        // read by the engine at start: needs a restart
        int index;

        Setting(String label, String key, String[] names, int[] values, boolean engine) {
            this.label = label;
            this.key = key;
            this.names = names;
            this.values = values;
            this.engine = engine;
        }

        int value() {
            return values[index];
        }
    }

    private final Setting[] settings = {
        new Setting("Разрешение", "render_height", new String[] {"540p", "720p", "900p", "1080p"},
                new int[] {540, 720, 900, 1080}, true),
        new Setting("Текстуры", "texture_skip_mips", new String[] {"Полные", "Средние", "Низкие"},
                new int[] {0, 1, 2}, true),
        new Setting("Лимит FPS", "fps_limit", new String[] {"Нет", "30", "45", "60"}, new int[] {0, 30, 45, 60},
                true),
        new Setting("Экономия батареи", "battery_saver", new String[] {"Выкл", "Вкл"}, new int[] {0, 1}, true),
        new Setting("Прозрачность", "ui_opacity", new String[] {"40%", "60%", "80%", "100%"},
                new int[] {40, 60, 80, 100}, false),
        new Setting("Размер кнопок", "ui_scale", new String[] {"80%", "100%", "120%", "140%"},
                new int[] {80, 100, 120, 140}, false),
        new Setting("Вибрация", "ui_vibrate", new String[] {"Выкл", "Вкл"}, new int[] {0, 1}, false),
        new Setting("Показывать FPS", "ui_fps", new String[] {"Выкл", "Вкл"}, new int[] {0, 1}, false),
        new Setting("Скрывать с геймпадом", "ui_pad_hide", new String[] {"Выкл", "Вкл"}, new int[] {0, 1}, false),
        new Setting("Бег от края стика", "ui_stick_run", new String[] {"Выкл", "Вкл"}, new int[] {0, 1}, false),
        new Setting("Скорость камеры", "ui_cam", new String[] {"50%", "75%", "100%", "150%", "200%"},
                new int[] {50, 75, 100, 150, 200}, false),
        new Setting("Инверсия Y", "ui_invert", new String[] {"Выкл", "Вкл"}, new int[] {0, 1}, false),
        new Setting("Графика игры", "game_detail", new String[] {"Низкая", "Средняя", "Высокая"},
                new int[] {0, 1, 2}, true),
        new Setting("Язык", "ui_lang", new String[] {"Русский", "English"}, new int[] {0, 1}, false),
        new Setting("Озвучка", "game_voice", new String[] {"Русская", "Английская"}, new int[] {0, 1}, true),
    };
    private static final int[] DEFAULTS = {0, 2, 0, 0, 2, 1, 1, 0, 0, 1, 2, 0, 0, -1, 0};  // -1: from the phone's language
    private static final int LANG = 13;

    // ---- interface language: strings are written in Russian and looked up here for English ----
    private static boolean english;
    private static final java.util.Map<String, String> EN = new java.util.HashMap<>();
    static {
        String[] pairs = {
            "Разрешение", "Resolution", "Текстуры", "Textures", "Полные", "Full", "Средние", "Medium",
            "Низкие", "Low", "Лимит FPS", "FPS limit", "Нет", "None", "Экономия батареи", "Battery saver",
            "Выкл", "Off", "Вкл", "On", "Прозрачность", "Opacity", "Размер кнопок", "Button size",
            "Вибрация", "Vibration", "Показывать FPS", "Show FPS", "Скрывать с геймпадом", "Hide with a gamepad",
            "Бег от края стика", "Run at the stick's edge", "Скорость камеры", "Camera speed",
            "Инверсия Y", "Invert Y", "Графика игры", "Game graphics", "Низкая", "Low", "Средняя", "Medium",
            "Высокая", "High", "Язык", "Language", "Озвучка", "Voice", "Русская", "Russian", "Английская", "English",
            "НАСТРОЙКИ", "SETTINGS", "РАСКЛАДКА", "LAYOUT", "ЗАКРЫТЬ", "CLOSE", "ПЕРЕЗАПУСК", "RESTART",
            "* после перезапуска", "* applies after restart", "СБРОС", "RESET", "ГОТОВО", "DONE", "СТИК", "STICK",
            "Перетаскивайте кнопки и стик, нажмите на кнопку, чтобы изменить её размер",
            "Drag the buttons and the stick, tap one to resize it",
            "Поддержать на Patreon", "Support on Patreon", "АВТО", "CAR", "ИГРА", "GAME", "МЕНЮ", "MENU",
            "ПАУЗА", "PAUSE", "ОК", "OK", "КАРТА", "MAP", "ОРУЖИЕ", "WEAPON", "ЕЩЁ", "MORE", "ГРАНАТА", "GRENADE",
            "ПРИСЕД", "CROUCH", "БЕГ", "RUN", "ОГОНЬ", "FIRE", "ПРИЦЕЛ", "AIM", "ДЕЙСТВИЕ", "ACTION",
            "ПРЫЖОК", "JUMP", "СЕСТЬ", "ENTER CAR", "УКРЫТИЕ", "COVER", "ОБОЙМА", "RELOAD", "УДАР", "PUNCH",
            "РУЧНИК", "HANDBRAKE", "ВЫЙТИ", "EXIT CAR", "СИГНАЛ", "HORN", "ЛИМИТ", "LIMITER",
        };
        for (int i = 0; i + 1 < pairs.length; i += 2) EN.put(pairs[i], pairs[i + 1]);
    }
    private static String tr(String ru) {
        if (!english) return ru;
        String en = EN.get(ru);
        return en != null ? en : ru;
    }
    private boolean panelOpen = false, restartNeeded = false;
    private final RectF panel = new RectF(), restartRect = new RectF(), closeRect = new RectF(),
            layoutRect = new RectF();

    // ---- layout editor ----
    private boolean editing = false;
    private final RectF doneRect = new RectF(), resetRect = new RectF();
    private final RectF smallerRect = new RectF(), biggerRect = new RectF();
    private final RectF supportRect = new RectF();  // the "Support on Patreon" link in the settings panel
    private Button selected;          // the button (or stick) the size buttons act on
    private boolean selectedStick;
    private float stickSize = 1;
    private Button dragButton;
    private boolean dragStick;
    private int dragPointer = -1;
    private float dragDx, dragDy;
    private float stickCustomX = -1, stickCustomY = -1;
    private final List<RectF> leftArrows = new ArrayList<>(), rightArrows = new ArrayList<>();

    // ---- drawing ----
    private final float density;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG),
            glow = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private boolean gameMode = false;
    private final RectF modeRect = new RectF(), gearRect = new RectF();
    private int modePointer = -1, gearPointer = -1;

    // stick
    private float stickHomeX, stickHomeY, stickX, stickY, stickR, knobX, knobY;
    private int stickPointer = -1;
    private final boolean[] dirDown = new boolean[4];  // W A S D
    private boolean stickRun, runButtonHeld, shiftDown;  // Shift comes from the rim of the stick or SPRINT
    private static final int[] DIR_KEYS = {KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_D};

    // camera / menu pointer
    private int lookPointer = -1;
    private float lookX, lookY, lastMouseX, lastMouseY;
    private int mouseState = 0;
    private final Map<Integer, Button> pointerButton = new HashMap<>();

    public TouchControls(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        prefs = context.getSharedPreferences("controls", Context.MODE_PRIVATE);
        for (int i = 0; i < settings.length; i++) {
            Setting s = settings[i];
            int def = DEFAULTS[i];
            if (i == LANG) {
                String l = java.util.Locale.getDefault().getLanguage();
                def = l.equals("ru") || l.equals("uk") || l.equals("be") ? 0 : 1;
            }
            s.index = Math.max(0, Math.min(s.values.length - 1, prefs.getInt(s.key, def)));
        }
        loadLayout();
        applyUiSettings();
        Typeface face = Typeface.create(Typeface.SERIF, Typeface.BOLD);
        text.setTypeface(face);
        text.setTextAlign(Paint.Align.CENTER);
        text.setLetterSpacing(0.08f);
        edge.setStyle(Paint.Style.STROKE);
        glow.setStyle(Paint.Style.STROKE);
        glow.setMaskFilter(new BlurMaskFilter(6 * density, BlurMaskFilter.Blur.NORMAL));
        setHapticFeedbackEnabled(true);
        InputManager im = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
        if (im != null) {
            im.registerInputDeviceListener(this, null);
            padConnected = findGamepad(im);
        }
        // Follow the game's situation four times a second; refresh the performance overlay with it.
        postDelayed(new Runnable() {
            @Override
            public void run() {
                if (pollState() || showFps) invalidate();
                postDelayed(this, 250);
            }
        }, 250);
    }

    private int context() {
        if (!gameMode) return MENU;
        return inCar ? CAR : FOOT;
    }

    // Reads nativeGameState; returns true when what is shown changes.
    private boolean pollState() {
        int s;
        try {
            s = nativeGameState();
        } catch (UnsatisfiedLinkError e) {
            s = 0;
        }
        boolean was = movieHidden();
        int before = context();
        stateKnown = (s & 8) != 0;
        boolean player = (s & 1) != 0;
        if (stateKnown && player != lastPlayer) {
            lastPlayer = player;
            if (!editing && !panelOpen) gameMode = player;  // into a mission: game controls; back to the menus: menu
        }
        inCar = stateKnown ? (s & 2) != 0 : manualCar;
        movie = (s & 4) != 0;
        if (context() != before) {
            moreOpen = false;
            // let go of what the new situation does not show; the stick and the camera stay held
            for (Button b : buttons) if (b.pointer >= 0 && !visible(b)) release(b);
            if (!gameMode && stickPointer >= 0) releaseStick();
        }
        return context() != before || was != movieHidden();
    }

    // During a movie the controls fade out; a touch brings them back for a few seconds.
    private boolean movieHidden() {
        return movie && !editing && !panelOpen && System.currentTimeMillis() > showDuringMovieUntil;
    }

    // Glowing shapes are rendered once into bitmaps (blur needs the software pipeline) and then only blitted.
    private final Map<String, Bitmap> cache = new HashMap<>();
    private final Paint blit = new Paint(Paint.FILTER_BITMAP_FLAG);

    private interface Painter {
        void paint(Canvas c, float cx, float cy);
    }

    private Bitmap cached(String key, float halfSize, Painter painter) {
        Bitmap b = cache.get(key);
        if (b == null) {
            int n = (int) Math.ceil(halfSize * 2);
            b = Bitmap.createBitmap(n, n, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(b);
            float saved = opacity;
            opacity = 1f;  // opacity is applied when blitting
            painter.paint(c, n / 2f, n / 2f);
            opacity = saved;
            cache.put(key, b);
        }
        return b;
    }

    private void blitCentered(Canvas c, Bitmap b, float cx, float cy) {
        blit.setAlpha(Math.round(255 * opacity));
        c.drawBitmap(b, cx - b.getWidth() / 2f, cy - b.getHeight() / 2f, blit);
    }

    private void applyUiSettings() {
        cache.clear();
        opacity = settings[4].value() / 100f;
        scale = settings[5].value() / 100f;
        vibrate = settings[6].value() != 0;
        showFps = settings[7].value() != 0;
        hideWithPad = settings[8].value() != 0;
        autoRun = settings[9].value() != 0;
        camGain = settings[10].value() / 100f;
        invertY = settings[11].value() != 0;
        english = settings[LANG].value() == 1;
        layoutButtons(getWidth(), getHeight());
    }

    private void loadLayout() {
        for (int i = 0; i < buttons.length; i++) {
            buttons[i].customX = prefs.getFloat("m3pos" + buttons[i].label + "x", -1);
            buttons[i].customY = prefs.getFloat("m3pos" + buttons[i].label + "y", -1);
            buttons[i].size = prefs.getFloat("m3size" + buttons[i].label, 1);
        }
        stickCustomX = prefs.getFloat("stick_x", -1);
        stickCustomY = prefs.getFloat("stick_y", -1);
        stickSize = prefs.getFloat("stick_size", 1);
    }

    private void saveLayout() {
        int w = Math.max(1, getWidth()), h = Math.max(1, getHeight());
        SharedPreferences.Editor ed = prefs.edit();
        for (int i = 0; i < buttons.length; i++) {
            Button b = buttons[i];
            ed.putFloat("m3size" + buttons[i].label, b.size);
            if (b.customX < 0) continue;
            ed.putFloat("m3pos" + buttons[i].label + "x", b.cx / w).putFloat("m3pos" + buttons[i].label + "y", b.cy / h);
        }
        ed.putFloat("stick_size", stickSize);
        if (stickCustomX >= 0) ed.putFloat("stick_x", stickHomeX / w).putFloat("stick_y", stickHomeY / h);
        ed.apply();
    }

    private void resetLayout() {
        SharedPreferences.Editor ed = prefs.edit();
        for (int i = 0; i < buttons.length; i++) {
            buttons[i].customX = buttons[i].customY = -1;
            buttons[i].size = 1;
            ed.remove("m3pos" + buttons[i].label + "x").remove("m3pos" + buttons[i].label + "y").remove("m3size" + buttons[i].label);
        }
        stickCustomX = stickCustomY = -1;
        stickSize = 1;
        selected = null;
        selectedStick = false;
        ed.remove("stick_x").remove("stick_y").remove("stick_size").apply();
        layoutButtons(getWidth(), getHeight());
    }

    // ---- gamepads ----
    // A real controller has gamepad buttons and a stick (joystick axes). Phones list built-in devices that claim
    // SOURCE_GAMEPAD too (Xiaomi: fingerprint sensor, side keys), which used to hide the touch controls with no
    // controller anywhere; those have no joystick axes, and on Android 10+ they are not external.
    private static boolean findGamepad(InputManager im) {
        for (int id : im.getInputDeviceIds()) {
            InputDevice d = im.getInputDevice(id);
            if (d == null || d.isVirtual()) continue;
            int sources = d.getSources();
            if ((sources & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD
                    || (sources & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) continue;
            if (d.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) == null
                    || d.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) == null) continue;
            if (Build.VERSION.SDK_INT >= 29 && !d.isExternal()) continue;
            return true;
        }
        return false;
    }

    private void updatePads() {
        InputManager im = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
        if (im != null) padConnected = findGamepad(im);
        invalidate();
    }

    @Override public void onInputDeviceAdded(int id) { updatePads(); }
    @Override public void onInputDeviceRemoved(int id) { updatePads(); }
    @Override public void onInputDeviceChanged(int id) { updatePads(); }

    private boolean controlsHidden() {
        return hideWithPad && padConnected;
    }

    // ---- layout ----
    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        layoutButtons(w, h);
        lastMouseX = w / 2f;
        lastMouseY = h / 2f;
    }

    private void layoutButtons(int w, int h) {
        final float u = density * scale;
        for (Button b : buttons) {
            b.r = b.sizeDp * u * b.size;
            if (b.customX >= 0) {
                b.cx = b.customX * w;
                b.cy = b.customY * h;
            } else {
                b.cx = b.fromRight ? w - b.dx * u : b.dx * u;
                b.cy = b.fromBottom ? h - b.dy * u : b.dy * u;
            }
        }
        stickR = 62 * u * stickSize;
        if (stickCustomX >= 0) {
            stickHomeX = stickCustomX * w;
            stickHomeY = stickCustomY * h;
        } else {
            stickHomeX = 225 * u;  // right of the minimap
            stickHomeY = h - 112 * u;
        }
        stickX = knobX = stickHomeX;
        stickY = knobY = stickHomeY;
        modeRect.set(14 * density, 14 * density, 110 * density, 52 * density);
        gearRect.set(w - 58 * density, 12 * density, w - 14 * density, 56 * density);
        float pw = Math.min(w * 0.92f, 860 * density), ph = Math.min(h * 0.94f, 500 * density);
        panel.set((w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2);
    }

    // ---- input to SDL ----
    private void key(int keycode, boolean down) {
        if (down) SDLActivity.onNativeKeyDown(keycode);
        else SDLActivity.onNativeKeyUp(keycode);
    }

    private void mouseButton(int bit, boolean down, float x, float y) {
        if (down) mouseState |= bit;
        else mouseState &= ~bit;
        int action = down ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
        if (gameMode) {
            // SDL sends a motion with every button: in game mode a zero relative one, as an absolute position would
            // jerk the camera (the cursor is wherever the relative camera drags left it)
            SDLActivity.onNativeMouse(mouseState, action, 0, 0, true);
            return;
        }
        lastMouseX = x;
        lastMouseY = y;
        SDLActivity.onNativeMouse(mouseState, action, x, y, false);
    }

    private void mouseMoveTo(float x, float y) {
        lastMouseX = x;
        lastMouseY = y;
        SDLActivity.onNativeMouse(mouseState, MotionEvent.ACTION_MOVE, x, y, false);
    }

    private float moveRestX, moveRestY;  // SDL takes whole pixels: carry the fractions

    private void mouseMoveBy(float dx, float dy) {
        dx += moveRestX;
        dy += moveRestY;
        int ix = (int) dx, iy = (int) dy;
        moveRestX = dx - ix;
        moveRestY = dy - iy;
        if (ix != 0 || iy != 0) SDLActivity.onNativeMouse(mouseState, MotionEvent.ACTION_MOVE, ix, iy, true);
    }

    private void buzz() {
        if (vibrate) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
    }

    private void press(Button b, int pointer) {
        b.pointer = pointer;
        pointerButton.put(pointer, b);
        buzz();
        if (b.keycode == 0 && b.mouseButton == 0) {  // ЕЩЁ: the row of rarer keys
            moreOpen = !moreOpen;
            return;
        }
        if (b.keycode == KeyEvent.KEYCODE_F && !stateKnown) manualCar = !manualCar;  // the game cannot tell: guess
        if (b.mouseButton != 0) mouseButton(b.mouseButton, true, lastMouseX, lastMouseY);
        else if (b.keycode == KeyEvent.KEYCODE_SHIFT_LEFT) setRun(stickRun, true);
        else key(b.keycode, true);
    }

    private void release(Button b) {
        pointerButton.remove(b.pointer);
        b.pointer = -1;
        if (b.keycode == 0 && b.mouseButton == 0) return;
        if (b.keycode == KeyEvent.KEYCODE_ESCAPE && gameMode) {  // the pause menu opens: menu controls
            key(b.keycode, false);
            releaseAll();
            gameMode = false;
            return;
        }
        if (b.keycode == KeyEvent.KEYCODE_F && !stateKnown) inCar = manualCar;
        if (b.mouseButton != 0) mouseButton(b.mouseButton, false, lastMouseX, lastMouseY);
        else if (b.keycode == KeyEvent.KEYCODE_SHIFT_LEFT) setRun(stickRun, false);
        else key(b.keycode, false);
    }

    private void setRun(boolean fromStick, boolean fromButton) {
        stickRun = fromStick;
        runButtonHeld = fromButton;
        boolean want = stickRun || runButtonHeld;
        if (want != shiftDown) {
            shiftDown = want;
            key(KeyEvent.KEYCODE_SHIFT_LEFT, want);
        }
    }

    private void updateStick(float x, float y) {
        float dx = x - stickX, dy = y - stickY;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > stickR) {
            dx *= stickR / len;
            dy *= stickR / len;
        }
        knobX = stickX + dx;
        knobY = stickY + dy;
        float dead = stickR * 0.3f;
        boolean[] want = {dy < -dead, dx < -dead, dy > dead, dx > dead};
        for (int i = 0; i < 4; i++) {
            if (want[i] != dirDown[i]) {
                dirDown[i] = want[i];
                key(DIR_KEYS[i], want[i]);
            }
        }
        // Pushed to the rim (with hysteresis): sprint, as with Shift held.
        boolean run = autoRun && stickPointer >= 0 && len > stickR * (stickRun ? 0.85f : 0.95f);
        if (run != stickRun) setRun(run, runButtonHeld);
    }

    private void releaseStick() {
        stickPointer = -1;
        updateStick(stickX, stickY);
        stickX = knobX = stickHomeX;
        stickY = knobY = stickHomeY;
    }

    private void releaseAll() {
        for (Button b : buttons) if (b.pointer >= 0) release(b);
        if (stickPointer >= 0) releaseStick();
        setRun(false, false);
        if (lookPointer >= 0 && !gameMode) mouseButton(MOUSE_LEFT, false, lookX, lookY);
        lookPointer = -1;
    }

    private boolean visible(Button b) {
        int c = context();
        boolean here = (b.ctx & c) != 0 || ((b.ctx & MORE) != 0 && c == FOOT && (moreOpen || editing));
        if (editing) return here;
        return here && !controlsHidden() && !movieHidden();
    }

    // ---- layout editor ----
    private void startEditing() {
        releaseAll();
        panelOpen = false;
        editing = true;
    }

    private void editTouch(MotionEvent e, int action, int id, float x, float y) {
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                if (dragPointer >= 0) break;
                if (doneRect.contains(x, y)) {
                    saveLayout();
                    editing = false;
                    buzz();
                    break;
                }
                if (resetRect.contains(x, y)) {
                    resetLayout();
                    buzz();
                    break;
                }
                if ((selected != null || selectedStick) && (smallerRect.contains(x, y) || biggerRect.contains(x, y))) {
                    float step = biggerRect.contains(x, y) ? 0.1f : -0.1f;
                    if (selected != null) {
                        selected.size = Math.max(0.6f, Math.min(2f, selected.size + step));
                    } else {
                        stickSize = Math.max(0.6f, Math.min(2f, stickSize + step));
                    }
                    float sx = stickHomeX, sy = stickHomeY;
                    layoutButtons(getWidth(), getHeight());
                    if (stickCustomX < 0) {  // keep a stick that was not moved where it is drawn
                        stickHomeX = stickX = knobX = sx;
                        stickHomeY = stickY = knobY = sy;
                    }
                    buzz();
                    break;
                }
                for (Button b : buttons) {
                    if (visible(b) && b.hit(x, y)) {
                        dragButton = b;
                        dragDx = b.cx - x;
                        dragDy = b.cy - y;
                        break;
                    }
                }
                if (dragButton == null) {
                    float dx = x - stickHomeX, dy = y - stickHomeY;
                    if (dx * dx + dy * dy <= stickR * stickR) {
                        dragStick = true;
                        dragDx = stickHomeX - x;
                        dragDy = stickHomeY - y;
                    }
                }
                if (dragButton != null || dragStick) {
                    dragPointer = id;
                    selected = dragButton;
                    selectedStick = dragStick;
                    buzz();
                }
                break;
            case MotionEvent.ACTION_MOVE: {
                int i = e.findPointerIndex(dragPointer);
                if (i < 0) break;
                float w = getWidth(), h = getHeight();
                float nx = Math.max(0, Math.min(w, e.getX(i) + dragDx));
                float ny = Math.max(0, Math.min(h, e.getY(i) + dragDy));
                if (dragButton != null) {
                    dragButton.cx = nx;
                    dragButton.cy = ny;
                    dragButton.customX = nx / w;
                    dragButton.customY = ny / h;
                } else if (dragStick) {
                    stickHomeX = stickX = knobX = nx;
                    stickHomeY = stickY = knobY = ny;
                    stickCustomX = nx / w;
                    stickCustomY = ny / h;
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (action == MotionEvent.ACTION_CANCEL || id == dragPointer) {
                    dragPointer = -1;
                    dragButton = null;
                    dragStick = false;
                }
                break;
            default:
                break;
        }
    }

    // ---- touch ----
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        float x = e.getX(index), y = e.getY(index);
        if (panelOpen) {
            if (action == MotionEvent.ACTION_UP) panelTap(x, y);
            invalidate();
            return true;
        }
        if (editing) {
            editTouch(e, action, id, x, y);
            invalidate();
            return true;
        }
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                if (gearRect.contains(x, y)) {
                    gearPointer = id;
                    break;
                }
                if (!controlsHidden() && modeRect.contains(x, y)) {
                    modePointer = id;
                    break;
                }
                Button hit = null;
                boolean wasHidden = movieHidden();
                if (movie) showDuringMovieUntil = System.currentTimeMillis() + 4000;
                for (Button b : buttons) {
                    if (!wasHidden && visible(b) && b.pointer < 0 && b.hit(x, y)) {
                        hit = b;
                        break;
                    }
                }
                if (hit != null) {
                    hit.fx = x;
                    hit.fy = y;
                    press(hit, id);
                } else if (gameMode && !controlsHidden() && stickPointer < 0 && x < getWidth() * 0.4f
                        && y > getHeight() * 0.35f) {
                    stickPointer = id;
                    stickX = x;  // the stick centres where the thumb lands
                    stickY = y;
                    updateStick(x, y);
                } else if (lookPointer < 0) {
                    lookPointer = id;
                    lookX = x;
                    lookY = y;
                    if (!gameMode) {
                        mouseMoveTo(x, y);
                        mouseButton(MOUSE_LEFT, true, x, y);
                    }
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int pid = e.getPointerId(i);
                    float px = e.getX(i), py = e.getY(i);
                    Button held = pointerButton.get(pid);
                    if (held != null) {
                        held.fx = px;
                        held.fy = py;
                    } else if (pid == stickPointer) {
                        updateStick(px, py);
                    } else if (pid == lookPointer) {
                        float g = 1.5f * camGain;
                        if (gameMode) mouseMoveBy((px - lookX) * g, (py - lookY) * g * (invertY ? -1 : 1));
                        else mouseMoveTo(px, py);
                        lookX = px;
                        lookY = py;
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                if (id == gearPointer) {
                    gearPointer = -1;
                    if (gearRect.contains(x, y)) {
                        releaseAll();
                        panelOpen = true;
                        buzz();
                    }
                } else if (id == modePointer) {
                    modePointer = -1;
                    if (modeRect.contains(x, y)) {
                        releaseAll();
                        gameMode = !gameMode;
                        buzz();
                    }
                } else if (pointerButton.containsKey(id)) {
                    release(pointerButton.get(id));
                } else if (id == stickPointer) {
                    releaseStick();
                } else if (id == lookPointer) {
                    lookPointer = -1;
                    if (!gameMode) mouseButton(MOUSE_LEFT, false, x, y);
                }
                break;
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                modePointer = gearPointer = -1;
                break;
            default:
                break;
        }
        invalidate();
        return true;
    }

    // ---- settings panel ----
    private void panelTap(float x, float y) {
        if (closeRect.contains(x, y) || !panel.contains(x, y)) {
            panelOpen = false;
            return;
        }
        if (restartNeeded && restartRect.contains(x, y)) {
            restartApp();
            return;
        }
        if (layoutRect.contains(x, y)) {
            startEditing();
            return;
        }
        if (supportRect.contains(x, y)) {
            SetupActivity.openSupport(getContext());
            return;
        }
        for (int i = 0; i < settings.length; i++) {
            int delta = leftArrows.get(i).contains(x, y) ? -1 : rightArrows.get(i).contains(x, y) ? 1 : 0;
            if (delta == 0) continue;
            Setting s = settings[i];
            s.index = (s.index + delta + s.values.length) % s.values.length;
            prefs.edit().putInt(s.key, s.index).apply();
            buzz();
            if (s.key.equals("fps_limit")) nativeSetFpsLimit(s.value());  // applies right away
            else if (s.engine) restartNeeded = true;
            if (s.engine) writeEngineSettings();
            applyUiSettings();
            return;
        }
    }

    /** Rewrites the engine keys in mafia2re.ini (next to game/), keeping every other line. */
    private void writeEngineSettings() {
        File dir = getContext().getExternalFilesDir(null);
        if (dir == null) return;
        File ini = new File(dir, "mafia2re.ini");
        StringBuilder out = new StringBuilder();
        try {
            if (ini.exists()) {
                for (String line : new String(Files.readAllBytes(ini.toPath()), StandardCharsets.UTF_8).split("\n")) {
                    String k = line.split("=", 2)[0].trim();
                    boolean ours = false;
                    for (Setting s : settings) ours |= s.engine && s.key.equals(k);
                    if (!ours && !line.trim().isEmpty()) out.append(line).append('\n');
                }
            }
            for (Setting s : settings) {
                if (s.engine) out.append(s.key).append(" = ").append(s.value()).append('\n');
            }
            try (FileWriter w = new FileWriter(ini)) {
                w.write(out.toString());
            }
        } catch (Exception ignored) {
            // settings stay in effect for the UI; the engine keeps its previous values
        }
    }

    private void restartApp() {
        Context ctx = getContext();
        Intent intent = new Intent(ctx, GameActivity.class);
        PendingIntent pi = PendingIntent.getActivity(ctx, 0, intent,
                PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        AlarmManager am = (AlarmManager) ctx.getSystemService(Context.ALARM_SERVICE);
        if (am != null) am.set(AlarmManager.RTC, System.currentTimeMillis() + 400, pi);
        if (ctx instanceof Activity) ((Activity) ctx).finishAffinity();
        System.exit(0);
    }

    // ---- drawing ----
    private int alpha(int color, float a) {
        return Color.argb(Math.round(Color.alpha(color) * a * opacity), Color.red(color), Color.green(color),
                Color.blue(color));
    }

    /** The button outline: a plain circle, the double ring comes from drawing it twice. */
    private void hexagon(float cx, float cy, float r) {
        path.reset();
        path.addCircle(cx, cy, r, android.graphics.Path.Direction.CW);
    }

    /** Prototype's bracket corners around a rectangle. */
    private void brackets(Canvas c, RectF r, float len, Paint p) {
        c.drawLine(r.left, r.top, r.left + len, r.top, p);
        c.drawLine(r.left, r.top, r.left, r.top + len, p);
        c.drawLine(r.right, r.top, r.right - len, r.top, p);
        c.drawLine(r.right, r.top, r.right, r.top + len, p);
        c.drawLine(r.left, r.bottom, r.left + len, r.bottom, p);
        c.drawLine(r.left, r.bottom, r.left, r.bottom - len, p);
        c.drawLine(r.right, r.bottom, r.right - len, r.bottom, p);
        c.drawLine(r.right, r.bottom, r.right, r.bottom - len, p);
    }

    private void drawButton(Canvas c, Button b) {
        final boolean down = b.pointer >= 0 || ("more".equals(b.icon) && moreOpen);
        Bitmap bmp = cached(b.label + (down ? ":1:" : ":0:") + b.r, b.r + 12 * density,
                (cc, x, y) -> paintButton(cc, b, x, y, down));
        blitCentered(c, bmp, b.cx, b.cy);
    }

    private void paintButton(Canvas c, Button b, float bx, float by, boolean down) {
        hexagon(bx, by, b.r);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(down ? 0xd08c1414 : 0xa00b0b0b, 1f));
        c.drawPath(path, fill);
        glow.setStrokeWidth(4 * density);
        glow.setColor(alpha(down ? 0xffc02020 : RED, down ? 1f : 0.35f));
        c.drawPath(path, glow);
        edge.setStrokeWidth(1.8f * density);
        edge.setColor(alpha(down ? 0xffffe8dc : RED, 1f));
        c.drawPath(path, edge);
        hexagon(bx, by, b.r * 0.78f);
        edge.setStrokeWidth(0.8f * density);
        edge.setColor(alpha(RED_DIM, 0.9f));
        c.drawPath(path, edge);
        drawIcon(c, b.icon, bx, by, b.r * 0.46f, down ? 0xffffffff : 0xfff5e6c0);
    }

    private final Path ip = new Path();
    private final RectF ir = new RectF();

    // Line icons in a box of +-s around (x, y).
    private void drawIcon(Canvas c, String icon, float x, float y, float s, int color) {
        Paint p = edge;
        p.setColor(alpha(color, 1f));
        p.setStrokeWidth(Math.max(1.5f * density, s * 0.16f));
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(color, 1f));
        ip.reset();
        switch (icon) {
            case "pause":
                c.drawRect(x - s * 0.6f, y - s * 0.7f, x - s * 0.2f, y + s * 0.7f, fill);
                c.drawRect(x + s * 0.2f, y - s * 0.7f, x + s * 0.6f, y + s * 0.7f, fill);
                break;
            case "ok":
                ip.moveTo(x - s * 0.7f, y);
                ip.lineTo(x - s * 0.2f, y + s * 0.55f);
                ip.lineTo(x + s * 0.75f, y - s * 0.6f);
                c.drawPath(ip, p);
                break;
            case "map":
                ip.moveTo(x - s, y - s * 0.6f);
                ip.lineTo(x - s * 0.33f, y - s * 0.85f);
                ip.lineTo(x + s * 0.33f, y - s * 0.6f);
                ip.lineTo(x + s, y - s * 0.85f);
                ip.lineTo(x + s, y + s * 0.6f);
                ip.lineTo(x + s * 0.33f, y + s * 0.85f);
                ip.lineTo(x - s * 0.33f, y + s * 0.6f);
                ip.lineTo(x - s, y + s * 0.85f);
                ip.close();
                c.drawPath(ip, p);
                c.drawLine(x - s * 0.33f, y - s * 0.85f, x - s * 0.33f, y + s * 0.6f, p);
                c.drawLine(x + s * 0.33f, y - s * 0.6f, x + s * 0.33f, y + s * 0.85f, p);
                break;
            case "weapon":  // pistol, barrel to the right
                ip.moveTo(x - s * 0.9f, y - s * 0.55f);
                ip.lineTo(x + s * 0.95f, y - s * 0.55f);
                ip.lineTo(x + s * 0.95f, y - s * 0.1f);
                ip.lineTo(x - s * 0.2f, y - s * 0.1f);
                ip.lineTo(x - s * 0.35f, y + s * 0.8f);
                ip.lineTo(x - s * 0.9f, y + s * 0.8f);
                ip.lineTo(x - s * 0.65f, y - s * 0.1f);
                ip.close();
                c.drawPath(ip, fill);
                break;
            case "more":
                for (int i = -1; i <= 1; i++) c.drawCircle(x + i * s * 0.6f, y, s * 0.17f, fill);
                break;
            case "grenade":
                c.drawCircle(x, y + s * 0.25f, s * 0.6f, p);
                c.drawRect(x - s * 0.25f, y - s * 0.65f, x + s * 0.25f, y - s * 0.35f, fill);
                c.drawCircle(x + s * 0.5f, y - s * 0.65f, s * 0.22f, p);
                break;
            case "crouch":
            case "jump": {
                float d = icon.equals("jump") ? -1 : 1;  // arrow up for jump, down for crouch
                ip.moveTo(x - s * 0.6f, y - d * s * 0.05f);
                ip.lineTo(x, y + d * s * 0.6f);
                ip.lineTo(x + s * 0.6f, y - d * s * 0.05f);
                c.drawPath(ip, p);
                c.drawLine(x, y + d * s * 0.6f, x, y - d * s * 0.6f, p);
                c.drawLine(x - s * 0.7f, y - d * s * 0.85f, x + s * 0.7f, y - d * s * 0.85f, p);
                break;
            }
            case "run":
                for (int i = 0; i < 2; i++) {
                    float o = (i - 0.5f) * s * 0.7f;
                    ip.moveTo(x + o - s * 0.3f, y - s * 0.6f);
                    ip.lineTo(x + o + s * 0.3f, y);
                    ip.lineTo(x + o - s * 0.3f, y + s * 0.6f);
                }
                c.drawPath(ip, p);
                break;
            case "fire":  // a bullet, nose up
                ir.set(x - s * 0.32f, y - s * 0.95f, x + s * 0.32f, y - s * 0.3f);
                ip.addArc(ir, 180, 180);
                ip.lineTo(x + s * 0.32f, y + s * 0.6f);
                ip.lineTo(x - s * 0.32f, y + s * 0.6f);
                ip.close();
                c.drawPath(ip, fill);
                c.drawRect(x - s * 0.4f, y + s * 0.72f, x + s * 0.4f, y + s * 0.95f, fill);
                break;
            case "aim":
                c.drawCircle(x, y, s * 0.6f, p);
                c.drawLine(x, y - s, x, y - s * 0.3f, p);
                c.drawLine(x, y + s, x, y + s * 0.3f, p);
                c.drawLine(x - s, y, x - s * 0.3f, y, p);
                c.drawLine(x + s, y, x + s * 0.3f, y, p);
                break;
            case "hand":
                ir.set(x - s * 0.6f, y - s * 0.1f, x + s * 0.55f, y + s * 0.9f);
                c.drawRoundRect(ir, s * 0.3f, s * 0.3f, p);
                for (int i = 0; i < 4; i++) {
                    float fx = x - s * 0.45f + i * s * 0.3f;
                    c.drawLine(fx, y - s * 0.1f, fx, y - s * (i == 0 || i == 3 ? 0.6f : 0.9f), p);
                }
                break;
            case "car":
                ir.set(x - s, y - s * 0.1f, x + s, y + s * 0.5f);
                c.drawRoundRect(ir, s * 0.2f, s * 0.2f, fill);
                ip.moveTo(x - s * 0.55f, y - s * 0.1f);
                ip.lineTo(x - s * 0.3f, y - s * 0.6f);
                ip.lineTo(x + s * 0.35f, y - s * 0.6f);
                ip.lineTo(x + s * 0.6f, y - s * 0.1f);
                c.drawPath(ip, p);
                fill.setColor(alpha(INK, 1f));
                c.drawCircle(x - s * 0.55f, y + s * 0.5f, s * 0.28f, fill);
                c.drawCircle(x + s * 0.55f, y + s * 0.5f, s * 0.28f, fill);
                c.drawCircle(x - s * 0.55f, y + s * 0.5f, s * 0.24f, p);
                c.drawCircle(x + s * 0.55f, y + s * 0.5f, s * 0.24f, p);
                break;
            case "cover":  // a low wall of bricks
                ir.set(x - s * 0.9f, y - s * 0.3f, x + s * 0.9f, y + s * 0.8f);
                c.drawRect(ir, p);
                c.drawLine(ir.left, y + s * 0.25f, ir.right, y + s * 0.25f, p);
                c.drawLine(x - s * 0.3f, ir.top, x - s * 0.3f, y + s * 0.25f, p);
                c.drawLine(x + s * 0.45f, ir.top, x + s * 0.45f, y + s * 0.25f, p);
                c.drawLine(x + s * 0.1f, y + s * 0.25f, x + s * 0.1f, ir.bottom, p);
                c.drawCircle(x, y - s * 0.7f, s * 0.22f, fill);  // a head behind it
                break;
            case "reload": {
                ir.set(x - s * 0.7f, y - s * 0.7f, x + s * 0.7f, y + s * 0.7f);
                ip.addArc(ir, -60, 290);
                c.drawPath(ip, p);
                ip.reset();
                float ax = x + s * 0.35f, ay = y - s * 0.6f;
                ip.moveTo(ax + s * 0.45f, ay - s * 0.25f);
                ip.lineTo(ax, ay);
                ip.lineTo(ax + s * 0.3f, ay + s * 0.42f);
                c.drawPath(ip, p);
                break;
            }
            case "fist": {  // an impact star
                for (int i = 0; i < 16; i++) {
                    double a = Math.PI / 8 * i;
                    float rr = (i % 2 == 0 ? 0.95f : 0.45f) * s;
                    float px = x + (float) Math.cos(a) * rr, py = y + (float) Math.sin(a) * rr;
                    if (i == 0) ip.moveTo(px, py);
                    else ip.lineTo(px, py);
                }
                ip.close();
                c.drawPath(ip, fill);
                break;
            }
            case "brake":
                c.drawCircle(x, y, s * 0.85f, p);
                text.setTextSize(s * 1.15f);
                text.setColor(alpha(color, 1f));
                c.drawText("P", x, y + s * 0.4f, text);
                break;
            case "exit":
                c.drawRect(x - s * 0.8f, y - s * 0.85f, x + s * 0.05f, y + s * 0.85f, p);
                c.drawLine(x - s * 0.25f, y, x + s * 0.95f, y, p);
                ip.moveTo(x + s * 0.55f, y - s * 0.4f);
                ip.lineTo(x + s * 0.95f, y);
                ip.lineTo(x + s * 0.55f, y + s * 0.4f);
                c.drawPath(ip, p);
                break;
            case "horn":
                ip.moveTo(x - s * 0.9f, y - s * 0.3f);
                ip.lineTo(x - s * 0.5f, y - s * 0.3f);
                ip.lineTo(x, y - s * 0.75f);
                ip.lineTo(x, y + s * 0.75f);
                ip.lineTo(x - s * 0.5f, y + s * 0.3f);
                ip.lineTo(x - s * 0.9f, y + s * 0.3f);
                ip.close();
                c.drawPath(ip, fill);
                for (int i = 1; i <= 2; i++) {
                    float rr = s * (0.1f + 0.4f * i);
                    ir.set(x - rr + s * 0.1f, y - rr, x + rr + s * 0.1f, y + rr);
                    c.drawArc(ir, -40, 80, false, p);
                }
                break;
            case "limiter":
                ir.set(x - s * 0.9f, y - s * 0.75f, x + s * 0.9f, y + s * 1.05f);
                c.drawArc(ir, 180, 180, false, p);
                c.drawLine(x, y + s * 0.15f, x + s * 0.55f, y - s * 0.4f, p);
                c.drawLine(x - s * 0.9f, y + s * 0.6f, x + s * 0.9f, y + s * 0.6f, p);
                break;
            default:
                break;
        }
        p.setStrokeCap(Paint.Cap.BUTT);
        p.setStrokeJoin(Paint.Join.MITER);
    }

    private void drawStick(Canvas c) {
        blitCentered(c, cached("stick:" + stickR, stickR + 12 * density, this::paintStickBase), stickX, stickY);
        final boolean held = stickPointer >= 0;
        blitCentered(c, cached("knob:" + held + stickR, stickR * 0.5f + 4 * density,
                (cc, x, y) -> paintKnob(cc, x, y, held)), knobX, knobY);
    }

    private void paintKnob(Canvas c, float x, float y, boolean held) {
        hexagon(x, y, stickR * 0.42f);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(held ? 0xe08c1414 : 0xa0201c18, 1f));
        c.drawPath(path, fill);
        edge.setStrokeWidth(1.6f * density);
        edge.setColor(alpha(0xffe6dcc6, 1f));
        c.drawPath(path, edge);
    }

    private void paintStickBase(Canvas c, float stickX, float stickY) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(0x700b0b0b, 1f));
        c.drawCircle(stickX, stickY, stickR, fill);
        glow.setStrokeWidth(4 * density);
        glow.setColor(alpha(RED, 0.5f));
        c.drawCircle(stickX, stickY, stickR, glow);
        edge.setStrokeWidth(1.6f * density);
        edge.setColor(alpha(RED, 0.9f));
        c.drawCircle(stickX, stickY, stickR, edge);
        // tick marks every 30 degrees, longer on the four directions
        for (int i = 0; i < 12; i++) {
            double a = Math.PI / 6 * i;
            float in = stickR * (i % 3 == 0 ? 0.80f : 0.88f);
            c.drawLine(stickX + (float) Math.cos(a) * in, stickY + (float) Math.sin(a) * in,
                    stickX + (float) Math.cos(a) * stickR, stickY + (float) Math.sin(a) * stickR, edge);
        }
    }

    private void drawPill(Canvas c, RectF r, String label, boolean active) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(active ? 0xc08c1414 : 0xa00b0b0b, 1f));
        c.drawRect(r, fill);
        edge.setStrokeWidth(1.5f * density);
        edge.setColor(alpha(RED, 1f));
        brackets(c, r, 9 * density, edge);
        text.setTextSize(14 * density);
        text.setColor(alpha(0xfff5e6c0, 1f));
        c.drawText(label, r.centerX(), r.centerY() + 5 * density, text);
    }

    private void drawGear(Canvas c) {
        float cx = gearRect.centerX(), cy = gearRect.centerY(), r = gearRect.width() * 0.32f;
        edge.setStrokeWidth(2.2f * density);
        edge.setColor(alpha(RED, 1f));
        for (int i = 0; i < 8; i++) {
            double a = Math.PI / 4 * i;
            c.drawLine(cx + (float) Math.cos(a) * r, cy + (float) Math.sin(a) * r,
                    cx + (float) Math.cos(a) * r * 1.35f, cy + (float) Math.sin(a) * r * 1.35f, edge);
        }
        c.drawCircle(cx, cy, r, edge);
        c.drawCircle(cx, cy, r * 0.4f, edge);
    }

    private void drawPerf(Canvas c) {
        float fps, ms;
        int mem;
        try {
            fps = nativeFps();
            ms = nativeFrameMs();
            mem = nativeMemoryMiB();
        } catch (UnsatisfiedLinkError e) {
            return;
        }
        String line = String.format("%.0f FPS  %.1f MS  %d MB", fps, ms, mem);
        text.setTextSize(13 * density);
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(fps >= 50 ? 0xff7dff7a : fps >= 28 ? 0xffffd060 : 0xffff5a4a);
        c.drawText(line, 16 * density, 74 * density, text);
        text.setTextAlign(Paint.Align.CENTER);
    }

    private void drawPanel(Canvas c) {
        c.drawColor(0x99000000);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0xf00b0b0b);
        c.drawRect(panel, fill);
        edge.setStrokeWidth(3 * density);
        edge.setColor(0x60e6dcc6);
        c.drawRect(panel, edge);
        edge.setStrokeWidth(2 * density);
        edge.setColor(RED);
        brackets(c, panel, 22 * density, edge);
        float pad = 22 * density;
        text.setTextSize(20 * density);
        text.setColor(RED);
        c.drawText(tr("НАСТРОЙКИ"), panel.centerX(), panel.top + 36 * density, text);
        // Credit, in the corners beside the title.
        text.setTextSize(11 * density);
        text.setColor(0xffa09880);
        text.setTextAlign(Paint.Align.LEFT);
        c.drawText(SetupActivity.CREDIT, panel.left + pad, panel.top + 34 * density, text);
        text.setTextAlign(Paint.Align.RIGHT);
        text.setColor(0xffffc04d);
        String support = tr("Поддержать на Patreon");
        c.drawText(support, panel.right - pad, panel.top + 34 * density, text);
        float sw = text.measureText(support);
        supportRect.set(panel.right - pad - sw - 8 * density, panel.top + 14 * density, panel.right - pad + 8 * density,
                panel.top + 44 * density);
        text.setTextAlign(Paint.Align.CENTER);
        // two columns of settings
        int rows = (settings.length + 1) / 2;
        float rowH = (panel.height() - 116 * density) / rows;
        float colW = (panel.width() - pad * 3) / 2;
        float top = panel.top + 54 * density;
        leftArrows.clear();
        rightArrows.clear();
        text.setTextSize(Math.min(15 * density, rowH * 0.42f));
        for (int i = 0; i < settings.length; i++) {
            Setting s = settings[i];
            float colLeft = panel.left + pad + (i / rows) * (colW + pad), colRight = colLeft + colW;
            float y = top + rowH * (i % rows) + rowH / 2;
            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(0xfff0e4c8);
            c.drawText(tr(s.label) + (s.engine && !s.key.equals("fps_limit") ? " *" : ""), colLeft, y + 5 * density,
                    text);
            float vx = colRight - 62 * density;
            RectF l = new RectF(vx - 64 * density, y - rowH / 2, vx - 32 * density, y + rowH / 2);
            RectF r = new RectF(vx + 32 * density, y - rowH / 2, vx + 64 * density, y + rowH / 2);
            leftArrows.add(l);
            rightArrows.add(r);
            text.setTextAlign(Paint.Align.CENTER);
            text.setColor(RED);
            c.drawText("‹", l.centerX(), y + 6 * density, text);
            c.drawText("›", r.centerX(), y + 6 * density, text);
            text.setColor(0xffffffff);
            c.drawText(tr(s.names[s.index]), vx, y + 5 * density, text);
            edge.setStrokeWidth(0.6f * density);
            edge.setColor(0x40e6dcc6);
            c.drawLine(colLeft, y + rowH / 2, colRight, y + rowH / 2, edge);
        }
        text.setTextAlign(Paint.Align.CENTER);
        float by = panel.bottom - 36 * density, pillW = 150 * density, gap = 12 * density;
        float cx = panel.centerX();
        layoutRect.set(cx - pillW * 1.5f - gap, by - 18 * density, cx - pillW * 0.5f - gap, by + 18 * density);
        drawPill(c, layoutRect, tr("РАСКЛАДКА"), false);
        closeRect.set(cx + pillW * 0.5f + gap, by - 18 * density, cx + pillW * 1.5f + gap, by + 18 * density);
        drawPill(c, closeRect, tr("ЗАКРЫТЬ"), false);
        restartRect.set(cx - pillW * 0.5f, by - 18 * density, cx + pillW * 0.5f, by + 18 * density);
        if (restartNeeded) {
            drawPill(c, restartRect, tr("ПЕРЕЗАПУСК"), true);
        } else {
            text.setTextSize(11 * density);
            text.setColor(0xffa09880);
            c.drawText(tr("* после перезапуска"), restartRect.centerX(), by + 4 * density, text);
        }
    }

    private void drawEditor(Canvas c) {
        c.drawColor(0x66000000);
        drawStick(c);
        for (Button b : buttons) {
            if (visible(b)) drawButton(c, b);
        }
        float cx = getWidth() * 0.38f, top = 16 * density;  // left of centre: clear of the buttons on the top right
        text.setTextSize(15 * density);
        text.setColor(0xfff5e6c0);
        c.drawText(tr("Перетаскивайте кнопки и стик, нажмите на кнопку, чтобы изменить её размер"), cx, top + 70 * density, text);
        resetRect.set(cx - 162 * density, top, cx - 12 * density, top + 38 * density);
        doneRect.set(cx + 12 * density, top, cx + 162 * density, top + 38 * density);
        drawPill(c, resetRect, tr("СБРОС"), false);
        drawPill(c, doneRect, tr("ГОТОВО"), true);
        if (selected != null || selectedStick) {
            float sx = selected != null ? selected.cx : stickHomeX, sy = selected != null ? selected.cy : stickHomeY;
            float sr = selected != null ? selected.r : stickR;
            edge.setStrokeWidth(2.5f * density);
            edge.setColor(0xffffffff);
            c.drawCircle(sx, sy, sr + 6 * density, edge);
            float y = top + 96 * density, size = selected != null ? selected.size : stickSize;
            smallerRect.set(cx - 150 * density, y, cx - 60 * density, y + 38 * density);
            biggerRect.set(cx + 60 * density, y, cx + 150 * density, y + 38 * density);
            drawPill(c, smallerRect, "\u2212", false);
            drawPill(c, biggerRect, "+", false);
            text.setTextSize(15 * density);
            text.setColor(0xfff5e6c0);
            c.drawText((selected != null ? tr(selected.label) : tr("СТИК")) + "  " + Math.round(size * 100) + "%", cx,
                    y + 25 * density, text);
        } else {
            smallerRect.setEmpty();
            biggerRect.setEmpty();
        }
    }

    // Screenshot of this layer for testing (the game's own shot_now trigger only has the game frame): a file named
    // ui_shot_now in the app's files folder makes the controls draw themselves into ui_shot.png there.
    private final Runnable uiShotPoll = new Runnable() {
        @Override public void run() {
            File dir = getContext().getExternalFilesDir(null);
            File trigger = dir == null ? null : new File(dir, "ui_shot_now");
            if (trigger != null && trigger.delete() && getWidth() > 0) {
                Bitmap bmp = Bitmap.createBitmap(getWidth(), getHeight(), Bitmap.Config.ARGB_8888);
                draw(new Canvas(bmp));
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(new File(dir, "ui_shot.png"))) {
                    bmp.compress(Bitmap.CompressFormat.PNG, 100, out);
                } catch (java.io.IOException ignored) {
                    // a test aid only
                }
                bmp.recycle();
            }
            postDelayed(this, 1000);
        }
    };

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        postDelayed(uiShotPoll, 1000);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(uiShotPoll);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (editing) {
            drawEditor(canvas);
            return;
        }
        boolean shown = !controlsHidden() && !movieHidden();
        if (shown) drawPill(canvas, modeRect, tr(gameMode ? (inCar ? "АВТО" : "ИГРА") : "МЕНЮ"), gameMode);
        drawGear(canvas);
        if (showFps) drawPerf(canvas);
        if (gameMode && shown) drawStick(canvas);
        for (Button b : buttons) {
            if (visible(b)) drawButton(canvas, b);
        }
        if (panelOpen) drawPanel(canvas);
    }
}
