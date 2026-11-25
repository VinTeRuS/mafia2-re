#include <algorithm>
#include <chrono>
#include <array>
#include <deque>
#include <mutex>
// SDL input -> window messages in the game window's queue (user32.cpp PostWindowMessage), as Windows delivers them:
//   WM_KEYDOWN/WM_KEYUP (wParam = virtual key, lParam = repeat | scan << 16 | extended << 24 | up bits),
//   WM_CHAR for typed text (UTF-16 units),
//   WM_MOUSEMOVE..WM_MOUSEWHEEL (wParam = MK_* button state, lParam = y << 16 | x in client coordinates).
#include <SDL2/SDL.h>

#include <atomic>
#include <vector>

#include "guest.h"

namespace native {
bool PresentRect(int* x, int* y, int* w, int* h);  // user32.cpp

void ClientSize(int* w, int* h);  // user32.cpp
void PostWindowMessage(uint32_t msg, uint32_t wparam, uint32_t lparam);  // user32.cpp: to the main window

namespace {

constexpr uint32_t kWmKeyDown = 0x100, kWmKeyUp = 0x101, kWmChar = 0x102;
constexpr uint32_t kWmMouseMove = 0x200, kWmLButtonDown = 0x201, kWmLButtonUp = 0x202, kWmRButtonDown = 0x204,
                   kWmRButtonUp = 0x205, kWmMButtonDown = 0x207, kWmMButtonUp = 0x208, kWmMouseWheel = 0x20a;

struct KeyMap {
    SDL_Scancode sdl;
    uint8_t vk;
    uint8_t scan;      // PC/AT set 1
    bool extended;
};

// Scancodes, not keycodes: the game binds physical keys, so a Russian layout still gives WASD.
const KeyMap kKeys[] = {
    {SDL_SCANCODE_ESCAPE, 0x1b, 0x01, false}, {SDL_SCANCODE_1, '1', 0x02, false}, {SDL_SCANCODE_2, '2', 0x03, false},
    {SDL_SCANCODE_3, '3', 0x04, false}, {SDL_SCANCODE_4, '4', 0x05, false}, {SDL_SCANCODE_5, '5', 0x06, false},
    {SDL_SCANCODE_6, '6', 0x07, false}, {SDL_SCANCODE_7, '7', 0x08, false}, {SDL_SCANCODE_8, '8', 0x09, false},
    {SDL_SCANCODE_9, '9', 0x0a, false}, {SDL_SCANCODE_0, '0', 0x0b, false}, {SDL_SCANCODE_MINUS, 0xbd, 0x0c, false},
    {SDL_SCANCODE_EQUALS, 0xbb, 0x0d, false}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0e, false},
    {SDL_SCANCODE_TAB, 0x09, 0x0f, false}, {SDL_SCANCODE_Q, 'Q', 0x10, false}, {SDL_SCANCODE_W, 'W', 0x11, false},
    {SDL_SCANCODE_E, 'E', 0x12, false}, {SDL_SCANCODE_R, 'R', 0x13, false}, {SDL_SCANCODE_T, 'T', 0x14, false},
    {SDL_SCANCODE_Y, 'Y', 0x15, false}, {SDL_SCANCODE_U, 'U', 0x16, false}, {SDL_SCANCODE_I, 'I', 0x17, false},
    {SDL_SCANCODE_O, 'O', 0x18, false}, {SDL_SCANCODE_P, 'P', 0x19, false},
    {SDL_SCANCODE_LEFTBRACKET, 0xdb, 0x1a, false}, {SDL_SCANCODE_RIGHTBRACKET, 0xdd, 0x1b, false},
    {SDL_SCANCODE_RETURN, 0x0d, 0x1c, false}, {SDL_SCANCODE_LCTRL, 0x11, 0x1d, false},
    {SDL_SCANCODE_A, 'A', 0x1e, false}, {SDL_SCANCODE_S, 'S', 0x1f, false}, {SDL_SCANCODE_D, 'D', 0x20, false},
    {SDL_SCANCODE_F, 'F', 0x21, false}, {SDL_SCANCODE_G, 'G', 0x22, false}, {SDL_SCANCODE_H, 'H', 0x23, false},
    {SDL_SCANCODE_J, 'J', 0x24, false}, {SDL_SCANCODE_K, 'K', 0x25, false}, {SDL_SCANCODE_L, 'L', 0x26, false},
    {SDL_SCANCODE_SEMICOLON, 0xba, 0x27, false}, {SDL_SCANCODE_APOSTROPHE, 0xde, 0x28, false},
    {SDL_SCANCODE_GRAVE, 0xc0, 0x29, false}, {SDL_SCANCODE_LSHIFT, 0x10, 0x2a, false},
    {SDL_SCANCODE_BACKSLASH, 0xdc, 0x2b, false}, {SDL_SCANCODE_Z, 'Z', 0x2c, false},
    {SDL_SCANCODE_X, 'X', 0x2d, false}, {SDL_SCANCODE_C, 'C', 0x2e, false}, {SDL_SCANCODE_V, 'V', 0x2f, false},
    {SDL_SCANCODE_B, 'B', 0x30, false}, {SDL_SCANCODE_N, 'N', 0x31, false}, {SDL_SCANCODE_M, 'M', 0x32, false},
    {SDL_SCANCODE_COMMA, 0xbc, 0x33, false}, {SDL_SCANCODE_PERIOD, 0xbe, 0x34, false},
    {SDL_SCANCODE_SLASH, 0xbf, 0x35, false}, {SDL_SCANCODE_RSHIFT, 0x10, 0x36, false},
    {SDL_SCANCODE_KP_MULTIPLY, 0x6a, 0x37, false}, {SDL_SCANCODE_LALT, 0x12, 0x38, false},
    {SDL_SCANCODE_SPACE, 0x20, 0x39, false}, {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3a, false},
    {SDL_SCANCODE_F1, 0x70, 0x3b, false}, {SDL_SCANCODE_F2, 0x71, 0x3c, false}, {SDL_SCANCODE_F3, 0x72, 0x3d, false},
    {SDL_SCANCODE_F4, 0x73, 0x3e, false}, {SDL_SCANCODE_F5, 0x74, 0x3f, false}, {SDL_SCANCODE_F6, 0x75, 0x40, false},
    {SDL_SCANCODE_F7, 0x76, 0x41, false}, {SDL_SCANCODE_F8, 0x77, 0x42, false}, {SDL_SCANCODE_F9, 0x78, 0x43, false},
    {SDL_SCANCODE_F10, 0x79, 0x44, false}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45, false},
    {SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46, false}, {SDL_SCANCODE_KP_7, 0x67, 0x47, false},
    {SDL_SCANCODE_KP_8, 0x68, 0x48, false}, {SDL_SCANCODE_KP_9, 0x69, 0x49, false},
    {SDL_SCANCODE_KP_MINUS, 0x6d, 0x4a, false}, {SDL_SCANCODE_KP_4, 0x64, 0x4b, false},
    {SDL_SCANCODE_KP_5, 0x65, 0x4c, false}, {SDL_SCANCODE_KP_6, 0x66, 0x4d, false},
    {SDL_SCANCODE_KP_PLUS, 0x6b, 0x4e, false}, {SDL_SCANCODE_KP_1, 0x61, 0x4f, false},
    {SDL_SCANCODE_KP_2, 0x62, 0x50, false}, {SDL_SCANCODE_KP_3, 0x63, 0x51, false},
    {SDL_SCANCODE_KP_0, 0x60, 0x52, false}, {SDL_SCANCODE_KP_PERIOD, 0x6e, 0x53, false},
    {SDL_SCANCODE_F11, 0x7a, 0x57, false}, {SDL_SCANCODE_F12, 0x7b, 0x58, false},
    {SDL_SCANCODE_KP_ENTER, 0x0d, 0x1c, true}, {SDL_SCANCODE_RCTRL, 0x11, 0x1d, true},
    {SDL_SCANCODE_KP_DIVIDE, 0x6f, 0x35, true}, {SDL_SCANCODE_RALT, 0x12, 0x38, true},
    {SDL_SCANCODE_HOME, 0x24, 0x47, true}, {SDL_SCANCODE_UP, 0x26, 0x48, true}, {SDL_SCANCODE_PAGEUP, 0x21, 0x49, true},
    {SDL_SCANCODE_LEFT, 0x25, 0x4b, true}, {SDL_SCANCODE_RIGHT, 0x27, 0x4d, true}, {SDL_SCANCODE_END, 0x23, 0x4f, true},
    {SDL_SCANCODE_DOWN, 0x28, 0x50, true}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, true},
    {SDL_SCANCODE_INSERT, 0x2d, 0x52, true}, {SDL_SCANCODE_DELETE, 0x2e, 0x53, true},
    {SDL_SCANCODE_PAUSE, 0x13, 0x45, false},
};

const KeyMap* FindKey(SDL_Scancode s) {
    for (const KeyMap& k : kKeys) {
        if (k.sdl == s) return &k;
    }
    return nullptr;
}

// Mouse buttons as the game has been told so far (bit 0 left, 1 right, 2 middle). On Android a press waits until the
// cursor has reached the finger, so this can lag behind SDL's state; every way the game reads the buttons uses it.
std::atomic<uint32_t> g_btn_delivered{0};

uint32_t ButtonState() {
    uint32_t b = g_btn_delivered.load();
    uint32_t mk = 0;
    if (b & 1) mk |= 0x01;
    if (b & 2) mk |= 0x02;
    if (b & 4) mk |= 0x10;
    SDL_Keymod mod = SDL_GetModState();
    if (mod & KMOD_SHIFT) mk |= 0x04;
    if (mod & KMOD_CTRL) mk |= 0x08;
    return mk;
}

// Window pixels -> the client coordinates the engine believes in.
uint32_t PackPoint(SDL_Window* w, int x, int y) {
    int ww = 1, wh = 1, cw, ch;
    SDL_GetWindowSize(w, &ww, &wh);
    ClientSize(&cw, &ch);
    int px = 0, py = 0, pw = ww, ph = wh;
    if (PresentRect(&px, &py, &pw, &ph) && pw > 0 && ph > 0) {  // the frame sits in the middle of the window
        x = std::min(std::max(x - px, 0), pw - 1);
        y = std::min(std::max(y - py, 0), ph - 1);
        ww = pw;
        wh = ph;
    }
    uint32_t cx = uint32_t(x * cw / (ww ? ww : 1)) & 0xffff;
    uint32_t cy = uint32_t(y * ch / (wh ? wh : 1)) & 0xffff;
    return cy << 16 | cx;
}

std::atomic<int32_t> g_mouse_dx{0}, g_mouse_dy{0}, g_mouse_dz{0};

// The engine accelerates its cursor with the speed of the mouse, so a touch that jumps across the screen would land
// anywhere: with mouse_step set (Android) a jump is fed in small steps, one per DirectInput read, at a speed with
// constant gain.
int MouseStep() {
#ifdef __ANDROID__
    static const int step = SettingInt("mouse_step", 28);
#else
    static const int step = SettingInt("mouse_step", 0);
#endif
    return step;
}

// Button changes wait, in order, until the cursor has been moved to where the finger is (TakeMouseMotion).
std::mutex g_btn_mutex;
std::deque<std::array<uint32_t, 3>> g_btn_queue;
std::chrono::steady_clock::time_point g_last_press, g_last_motion;

uint32_t ButtonBit(uint32_t msg) {
    return msg == kWmLButtonDown || msg == kWmLButtonUp ? 1u : msg == kWmRButtonDown || msg == kWmRButtonUp ? 2u
           : msg == kWmMButtonDown || msg == kWmMButtonUp ? 4u : 0u;
}
bool IsRelease(uint32_t msg) { return msg == kWmLButtonUp || msg == kWmRButtonUp || msg == kWmMButtonUp; }

void Deliver(uint32_t msg, uint32_t lparam) {
    uint32_t bit = ButtonBit(msg);
    if (IsRelease(msg)) g_btn_delivered &= ~bit;
    else g_btn_delivered |= bit;
    PostWindowMessage(msg, ButtonState(), lparam);
}

void FlushButtons() {
    std::lock_guard<std::mutex> l(g_btn_mutex);
    while (!g_btn_queue.empty()) {
        auto m = g_btn_queue.front();
        // A press and its release in the same instant would never be seen by a game that polls the buttons per frame.
        bool release = IsRelease(m[0]);
        if (release && std::chrono::steady_clock::now() - g_last_press < std::chrono::milliseconds(120)) break;
        // The game finds what is under the cursor a frame after it moves: a press has to come after that.
        if (!release && std::chrono::steady_clock::now() - g_last_motion < std::chrono::milliseconds(70)) break;
        if (!release) g_last_press = std::chrono::steady_clock::now();
        g_btn_queue.pop_front();
        Deliver(m[0], m[2]);
    }
}

void Mouse(uint32_t msg, uint32_t wparam, uint32_t lparam) {
    if (!ButtonBit(msg)) {
        PostWindowMessage(msg, wparam, lparam);
        return;
    }
    if (MouseStep() <= 0) {
        Deliver(msg, lparam);
        return;
    }
    {
        std::lock_guard<std::mutex> l(g_btn_mutex);
        g_btn_queue.push_back({msg, wparam, lparam});
    }
    if (g_mouse_dx.load() == 0 && g_mouse_dy.load() == 0) FlushButtons();
}

// Keys held by InjectKey (test scripts): SDL's own keyboard state only follows real devices.
bool g_injected[SDL_NUM_SCANCODES];


}  // namespace

// ---- state for the DirectInput devices (dinput.cpp) ----
// DirectInput key codes (DIK_*) are set-1 scan codes, with 0x80 for the extended (E0-prefixed) keys.
std::vector<uint8_t> KeyboardDiks() {
    std::vector<uint8_t> out;
    for (const KeyMap& k : kKeys) out.push_back(uint8_t(k.scan | (k.extended ? 0x80 : 0)));
    return out;
}

bool DikDown(uint8_t dik) {
    const Uint8* state = SDL_GetKeyboardState(nullptr);
    for (const KeyMap& k : kKeys) {
        if (uint8_t(k.scan | (k.extended ? 0x80 : 0)) == dik && (state[k.sdl] || g_injected[k.sdl])) return true;
    }
    return false;
}

void TakeMouseMotion(int32_t* dx, int32_t* dy, int32_t* dz) {
    int step = MouseStep();
    if (step > 0) {
        int32_t px = g_mouse_dx.load(), py = g_mouse_dy.load();
        *dx = std::max(-step, std::min(step, px));
        *dy = std::max(-step, std::min(step, py));
        g_mouse_dx -= *dx;
        g_mouse_dy -= *dy;
        if (*dx || *dy) g_last_motion = std::chrono::steady_clock::now();
    } else {
        *dx = g_mouse_dx.exchange(0);
        *dy = g_mouse_dy.exchange(0);
    }
    *dz = g_mouse_dz.exchange(0);
    if (g_mouse_dx.load() == 0 && g_mouse_dy.load() == 0) FlushButtons();
}

// MapVirtualKey over the key table: MAPVK_VK_TO_VSC 0, VSC_TO_VK 1, VK_TO_CHAR 2, VSC_TO_VK_EX 3.
uint32_t MapVirtualKeyHost(uint32_t code, uint32_t type) {
    for (const KeyMap& k : kKeys) {
        if ((type == 1 || type == 3) && !k.extended && k.scan == code) return k.vk;
        if (type == 0 && k.vk == code) return k.scan;
        if (type == 2 && k.vk == code) {
            if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || code == ' ') return code;
            return 0;
        }
    }
    return 0;
}

// Debug console: moves the engine's cursor to client coordinates through the same path as a touch, then clicks.
void DebugMouse(int x, int y, bool click) {
    static int cx = -1, cy = -1;
    if (cx < 0) {  // first use: run into the top left corner to learn where the cursor is
        g_mouse_dx -= 4000;
        g_mouse_dy -= 4000;
        cx = cy = 0;
    }
    g_mouse_dx += x - cx;
    g_mouse_dy += y - cy;
    cx = x;
    cy = y;
    uint32_t lp = uint32_t(y) << 16 | uint32_t(x & 0xffff);
    if (click) {
        Mouse(kWmLButtonDown, 1, lp);
        Mouse(kWmLButtonUp, 0, lp);
    }
}

// Windows virtual key held down (GetKeyState & co., user32.cpp): mouse buttons, the generic modifiers and every key
// of the table above.
bool VkDown(uint32_t vk) {
    uint32_t b = g_btn_delivered.load();
    if (vk == 0x01) return b & 1;
    if (vk == 0x02) return b & 2;
    if (vk == 0x04) return b & 4;
    const Uint8* state = SDL_GetKeyboardState(nullptr);
    auto held = [&](SDL_Scancode s) { return state[s] || g_injected[s]; };
    switch (vk) {
        case 0xa0: return held(SDL_SCANCODE_LSHIFT);
        case 0xa1: return held(SDL_SCANCODE_RSHIFT);
        case 0xa2: return held(SDL_SCANCODE_LCTRL);
        case 0xa3: return held(SDL_SCANCODE_RCTRL);
        case 0xa4: return held(SDL_SCANCODE_LALT);
        case 0xa5: return held(SDL_SCANCODE_RALT);
        default: break;
    }
    for (const KeyMap& k : kKeys) {
        if (k.vk == vk && held(k.sdl)) return true;
    }
    return false;
}

// Bit i: DirectInput mouse button i (0 left, 1 right, 2 middle, 3/4 extra).
uint32_t MouseButtonMask() {
    uint32_t b = SDL_GetMouseState(nullptr, nullptr);
    return g_btn_delivered.load() | (b & SDL_BUTTON_X1MASK ? 8u : 0u) | (b & SDL_BUTTON_X2MASK ? 16u : 0u);
}

// Windows sends WM_MOUSEMOVE as soon as the window is under the cursor.
void InitInput() {
    int cw, ch;
    ClientSize(&cw, &ch);
    Mouse(kWmMouseMove, 0, uint32_t(ch / 2) << 16 | uint32_t(cw / 2));
}

void ForwardInput(const SDL_Event& e) {
    SDL_Window* w = SDL_GetWindowFromID(e.type == SDL_KEYDOWN || e.type == SDL_KEYUP ? e.key.windowID
                                        : e.type == SDL_MOUSEMOTION                  ? e.motion.windowID
                                        : e.type == SDL_MOUSEWHEEL                   ? e.wheel.windowID
                                                                                     : e.button.windowID);
    switch (e.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const KeyMap* k = FindKey(e.key.keysym.scancode);
            if (!k) return;
            bool up = e.type == SDL_KEYUP;
            uint32_t lparam = 1u | uint32_t(k->scan) << 16 | (k->extended ? 1u << 24 : 0u);
            if (up) lparam |= 3u << 30;            // previous state down, transition up
            else if (e.key.repeat) lparam |= 1u << 30;
            PostWindowMessage(up ? kWmKeyUp : kWmKeyDown, k->vk, lparam);
            // Keys that type a control character (TranslateMessage would make WM_CHAR from them).
            if (!up && (k->vk == 0x08 || k->vk == 0x09 || k->vk == 0x0d || k->vk == 0x1b))
                PostWindowMessage(kWmChar, k->vk, lparam);
            break;
        }
        case SDL_TEXTINPUT: {
            // UTF-8 -> UTF-16 code units, one WM_CHAR each.
            const unsigned char* t = reinterpret_cast<const unsigned char*>(e.text.text);
            while (*t) {
                uint32_t cp = *t++;
                int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
                cp &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
                for (; extra && (*t & 0xc0) == 0x80; --extra) cp = cp << 6 | (*t++ & 0x3f);
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    PostWindowMessage(kWmChar, 0xd800 + (cp >> 10), 1);
                    PostWindowMessage(kWmChar, 0xdc00 + (cp & 0x3ff), 1);
                } else {
                    PostWindowMessage(kWmChar, cp, 1);
                }
            }
            break;
        }
        case SDL_MOUSEMOTION:
            {   // The engine's cursor follows DirectInput deltas, not window messages, and moves by a fixed fraction of
                // them (about 0.4 client pixels per delta pixel at 1200x540): feed deltas in client pixels, divided
                // by that gain, so the cursor lands where the finger is (setting mouse_gain, 0 = raw deltas).
                #ifdef __ANDROID__
                static const double gain = SettingInt("mouse_gain_pct", 100) / 100.0;
#else
                static const double gain = SettingInt("mouse_gain_pct", 0) / 100.0;
#endif
                static double rx = 0, ry = 0;
                int ww = 1, wh = 1, cw = 1, ch = 1;
                if (w) SDL_GetWindowSize(w, &ww, &wh);
                ClientSize(&cw, &ch);
                double sx = gain > 0 && ww > 0 ? double(cw) / ww / gain : 1.0, sy = gain > 0 && wh > 0 ? double(ch) / wh / gain : 1.0;
                rx += e.motion.xrel * sx;
                ry += e.motion.yrel * sy;
                int ix = int(rx), iy = int(ry);
                rx -= ix;
                ry -= iy;
                g_mouse_dx += ix;
                g_mouse_dy += iy;
            }
            if (w) Mouse(kWmMouseMove, ButtonState(), PackPoint(w, e.motion.x, e.motion.y));
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            bool down = e.type == SDL_MOUSEBUTTONDOWN;
            uint32_t msg = e.button.button == SDL_BUTTON_LEFT    ? (down ? kWmLButtonDown : kWmLButtonUp)
                           : e.button.button == SDL_BUTTON_RIGHT ? (down ? kWmRButtonDown : kWmRButtonUp)
                           : e.button.button == SDL_BUTTON_MIDDLE ? (down ? kWmMButtonDown : kWmMButtonUp)
                                                                  : 0;
            if (msg && w) Mouse(msg, ButtonState(), PackPoint(w, e.button.x, e.button.y));
            break;
        }
        case SDL_MOUSEWHEEL: {
            int x = 0, y = 0;
            SDL_GetMouseState(&x, &y);
            uint32_t delta = uint32_t(int16_t(e.wheel.y * 120)) & 0xffff;
            g_mouse_dz += e.wheel.y * 120;
            if (w) Mouse(kWmMouseWheel, delta << 16 | ButtonState(), PackPoint(w, x, y));
            break;
        }
        default:
            break;
    }
}

// Presses or releases a key as if it came from the keyboard: window message and DirectInput state.
void InjectKey(SDL_Scancode sc, bool down) {
    g_injected[sc] = down;
    SDL_Event e{};
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.scancode = sc;
    ForwardInput(e);
}

// Scripted mouse (autoplay.cpp): a relative move, or a button (SDL_BUTTON_LEFT/RIGHT) at the window centre.
extern SDL_Window* g_sdl_window;
void InjectMouseMove(int dx, int dy) {
    SDL_Event e{};
    e.type = SDL_MOUSEMOTION;
    e.motion.windowID = g_sdl_window ? SDL_GetWindowID(g_sdl_window) : 0;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    int w = 0, h = 0;
    if (g_sdl_window) SDL_GetWindowSize(g_sdl_window, &w, &h);
    e.motion.x = w / 2;
    e.motion.y = h / 2;
    ForwardInput(e);
}
void InjectMouseButton(int button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.windowID = g_sdl_window ? SDL_GetWindowID(g_sdl_window) : 0;
    e.button.button = uint8_t(button);
    int w = 0, h = 0;
    if (g_sdl_window) SDL_GetWindowSize(g_sdl_window, &w, &h);
    e.button.x = w / 2;
    e.button.y = h / 2;
    ForwardInput(e);
}

}  // namespace native
