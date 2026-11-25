// Autoplay: a scenario file played inside the game with no one at the controls (tools/autoplay.sh drives a phone with
// it). autoplay.txt in the working folder (Android: the app's files folder), one step per line, run in order from the
// game's message pump:
//   wait <sec>                      pause
//   wait_player|wait_car|wait_foot|wait_menu [timeout sec]   until the player exists / sits in a vehicle / is on
//                                   foot / the main menu's background video plays
//   key <SDL key name>              tap (0.15 s);  down <key> / up <key>: hold and let go
//   mouse <dx> <dy> [sec]           move the camera, spread over sec
//   click|mdown|mup left|right      mouse button
//   shot <name>                     screenshot (shot_now -> autoplay_<name>.ppm)
//   lua <chunk>                     run Lua in the game (lua_cmd.lua, result in the log)
//   log <text>                      a line in the log
// "autoplay: done" in the log ends it; "autoplay: timeout ..." marks a wait that gave up and went on.
#include <SDL2/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "guest.h"

namespace native {
void InjectKey(SDL_Scancode sc, bool down);
void InjectMouseMove(int dx, int dy);
void InjectMouseButton(int button, bool down);
int GameStateBits();

namespace {
using Clock = std::chrono::steady_clock;

struct Autoplay {
    std::vector<std::string> steps;
    size_t next = 0;
    Clock::time_point until{}, started{};
    bool waiting = false, loaded = false, done = false;
    // a step in progress: a tap to release, a mouse move to spread, a wait on the game state, a screenshot to rename
    SDL_Scancode tap = SDL_SCANCODE_UNKNOWN;
    int click = 0;  // mouse button of a click to release
    int mouse_left_x = 0, mouse_left_y = 0;
    Clock::time_point mouse_end{}, last_tick{};
    int want_state = 0;  // 1 player, 3 car, 5 foot (player, not in a vehicle)
    std::string shot_name;
    Clock::time_point bench_end{};  // a bench segment in progress until then
};

double Secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// Frame times of a bench segment, fed by Present (AutoplayFrame) on the render thread.
std::mutex g_bench_mutex;
std::vector<float> g_bench_ms;
std::string g_bench_name;
std::atomic<bool> g_bench_on{false};
Clock::time_point g_bench_start{};

void BenchStart(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_bench_mutex);
    g_bench_ms.clear();
    g_bench_name = name;
    g_bench_on = true;
    g_bench_start = Clock::now();
}

void BenchEnd() {
    std::vector<float> v;
    std::string name;
    double secs;
    {
        std::lock_guard<std::mutex> lock(g_bench_mutex);
        g_bench_on = false;
        v.swap(g_bench_ms);
        name = g_bench_name;
        secs = Secs(g_bench_start, Clock::now());
    }
    if (v.empty()) {
        Log("bench: %s: no frames", name.c_str());
        return;
    }
    std::sort(v.begin(), v.end());
    const float p99 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.99))];
    size_t over50 = 0;
    for (float ms : v) over50 += ms > 50;
    Log("bench: %s: %.1f fps average, %.1f fps 1%% low, worst frame %.0f ms, %zu frames over 50 ms (%zu frames, %.0f s)",
        name.c_str(), double(v.size()) / secs, 1000.0 / p99, double(v.back()), over50, v.size(), secs);
}
Autoplay g_ap;

Clock::time_point After(double s) {
    return Clock::now() + std::chrono::milliseconds(long(s * 1000));
}

int Button(const std::string& s) { return s == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT; }

bool StateMet(int want) {
    int b = GameStateBits();
    if (want == 1) return b & 1;
    if (want == 3) return (b & 3) == 3;
    if (want == 16) return b & 16;
    return (b & 1) && !(b & 2);
}

// Finishes the step in progress; true while it still runs.
bool Busy(Clock::time_point now) {
    Autoplay& a = g_ap;
    if (a.bench_end != Clock::time_point{}) {
        if (now < a.bench_end) return true;
        a.bench_end = {};
        BenchEnd();
    }
    if (a.tap != SDL_SCANCODE_UNKNOWN) {
        if (now < a.until) return true;
        InjectKey(a.tap, false);
        a.tap = SDL_SCANCODE_UNKNOWN;
    }
    if (a.click) {
        if (now < a.until) return true;
        InjectMouseButton(a.click, false);
        a.click = 0;
    }
    if (a.mouse_left_x || a.mouse_left_y) {
        double left = Secs(now, a.mouse_end), step = Secs(a.last_tick, now);
        a.last_tick = now;
        int dx = a.mouse_left_x, dy = a.mouse_left_y;
        if (left > 0) {
            double f = std::min(1.0, step / (left + step));
            dx = int(a.mouse_left_x * f);
            dy = int(a.mouse_left_y * f);
        }
        if (dx || dy) InjectMouseMove(dx, dy);
        a.mouse_left_x -= dx;
        a.mouse_left_y -= dy;
        if (left > 0) return true;
        a.mouse_left_x = a.mouse_left_y = 0;
    }
    if (a.want_state) {
        if (StateMet(a.want_state)) {
            Log("autoplay: state %d reached after %.1f s", a.want_state, Secs(a.started, now));
        } else if (now < a.until) {
            return true;
        } else {
            Log("autoplay: timeout waiting for state %d (state bits %d)", a.want_state, GameStateBits());
        }
        a.want_state = 0;
    }
    if (!a.shot_name.empty()) {
        if (access("shot_now.ppm", F_OK) != 0 && now < a.until) return true;
        std::string out = "autoplay_" + a.shot_name + ".ppm";
        Log("autoplay: shot %s %s", out.c_str(), rename("shot_now.ppm", out.c_str()) == 0 ? "written" : "missing");
        a.shot_name.clear();
    }
    if (a.waiting) {
        if (now < a.until) return true;
        a.waiting = false;
    }
    return false;
}

void Start(const std::string& line) {
    Autoplay& a = g_ap;
    std::istringstream in(line);
    std::string op, arg;
    in >> op;
    Clock::time_point now = Clock::now();
    a.started = now;
    if (op == "wait") {
        double s = 1;
        in >> s;
        a.waiting = true;
        a.until = After(s);
    } else if (op == "wait_player" || op == "wait_car" || op == "wait_foot" || op == "wait_menu") {
        double s = 600;
        in >> s;
        a.want_state = op == "wait_player" ? 1 : op == "wait_car" ? 3 : op == "wait_menu" ? 16 : 5;
        a.until = After(s);
    } else if (op == "key" || op == "down" || op == "up") {
        std::getline(in, arg);  // SDL names may hold spaces ("Left Shift")
        arg.erase(0, arg.find_first_not_of(" \t"));
        SDL_Scancode sc = SDL_GetScancodeFromName(arg.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) {
            Log("autoplay: unknown key %s", arg.c_str());
            return;
        }
        InjectKey(sc, op != "up");
        if (op == "key") {
            a.tap = sc;
            a.until = After(0.15);
        }
    } else if (op == "mouse") {
        double s = 0;
        in >> a.mouse_left_x >> a.mouse_left_y >> s;
        a.mouse_end = After(s);
        a.last_tick = now;
    } else if (op == "click" || op == "mdown" || op == "mup") {
        in >> arg;
        InjectMouseButton(Button(arg), op != "mup");
        if (op == "click") {  // released by Busy after a moment
            a.click = Button(arg);
            a.until = After(0.15);
        }
    } else if (op == "shot") {
        in >> a.shot_name;
        if (a.shot_name.empty()) a.shot_name = "shot";
        unlink("shot_now.ppm");
        if (FILE* f = fopen("shot_now", "w")) fclose(f);
        a.until = After(5);
    } else if (op == "lua") {
        std::string chunk = line.substr(line.find("lua") + 3);
        if (FILE* f = fopen("lua_cmd.lua", "w")) {
            fputs(chunk.c_str(), f);
            fclose(f);
        }
    } else if (op == "teleport") {  // teleport x y z: the player to a point of the map (Lua SetPos)
        double x = 0, y = 0, z = 0;
        in >> x >> y >> z;
        if (FILE* f = fopen("lua_cmd.lua", "w")) {
            fprintf(f, "local p = game.game:GetActivePlayer() p:SetPos(Math:newVector(%f, %f, %f)) "
                       "local q = p:GetPos() return \"teleport \" .. q.x .. \" \" .. q.y .. \" \" .. q.z", x, y, z);
            fclose(f);
        }
    } else if (op == "pos") {  // the player's position into the log (to write down points for a route)
        if (FILE* f = fopen("lua_cmd.lua", "w")) {
            fputs("local q = game.game:GetActivePlayer():GetPos() return \"pos \" .. q.x .. \" \" .. q.y .. \" \" .. q.z", f);
            fclose(f);
        }
    } else if (op == "bench") {  // bench <name> <sec>: frame time statistics of the next sec seconds
        std::string name;
        double s = 10;
        in >> name >> s;
        BenchStart(name.empty() ? "bench" : name);
        a.bench_end = After(s);
    } else if (op == "log") {
        Log("autoplay: %s", line.size() > 4 ? line.c_str() + 4 : "");
    } else {
        Log("autoplay: unknown step '%s'", line.c_str());
    }
}

void Load() {
    g_ap.loaded = true;
    FILE* f = fopen("autoplay.txt", "r");
    if (!f) return;
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos || s[b] == '#') continue;
        g_ap.steps.push_back(s.substr(b));
    }
    fclose(f);
    Log("autoplay: %zu steps from autoplay.txt", g_ap.steps.size());
}
}  // namespace

// From Present (render thread): one frame's time, kept while a bench segment runs.
void AutoplayFrame(double ms) {
    if (!g_bench_on.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_bench_mutex);
    if (g_bench_on) g_bench_ms.push_back(float(ms));
}

// From the message pump (user32.cpp PumpSdl), on the game's main thread.
void RunAutoplay() {
    Autoplay& a = g_ap;
    if (!a.loaded) Load();
    if (a.done || a.steps.empty()) return;
    Clock::time_point now = Clock::now();
    if (Busy(now)) return;
    if (a.next >= a.steps.size()) {
        a.done = true;
        Log("autoplay: done");
        return;
    }
    const std::string& s = a.steps[a.next++];
    Log("autoplay: step %zu: %s", a.next, s.c_str());
    Start(s);
}
}  // namespace native
