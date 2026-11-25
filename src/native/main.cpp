// Native host entry point: replaces Windows' process startup for mafia2.exe. Maps the exe into guest memory at its
// preferred base, gives the main thread its TEB and static TLS, runs the TLS callbacks and then the exe's own entry
// point (the MSVC CRT startup, which calls WinMain). From there the game drives itself: it creates its window,
// runs its message loop (user32.cpp turns SDL events into window messages) and ends through ExitProcess.
#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>  // Android: main() becomes SDL_main, called by SDLActivity
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <jni.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest.h"
#include "win.h"

namespace native {

const char* g_game_dir = nullptr;
#ifdef MAFIA2RE_PGO_GEN
extern "C" void __llvm_profile_set_filename(const char*);
#endif
const char* g_data_dir = nullptr;  // Android: the app's external files folder (logs, ini, saves)

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}  // namespace

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "mafia2re", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

void VulkanDriverAbandon();  // vulkan_driver.cpp
void VulkanDriverCrashed();

namespace {

// Crash report: the fault and the faulting code's library + offset go to the log (and logcat), then the previous
// handler (Android's debuggerd) takes over. Without it a crash on a user's phone just cuts the log off. Formatting
// is not strictly async-signal-safe, which is acceptable for a process that is dying anyway.
struct sigaction g_old_actions[32];

void CrashWrite(const char* text) {
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_ERROR, "mafia2re", text);
#else
    write(2, text, strlen(text));
    write(2, "\n", 1);
#endif
    if (g_log) {
        int fd = fileno(g_log);
        write(fd, text, strlen(text));
        write(fd, "\n", 1);
        fsync(fd);
    }
}

void DescribeAddress(char* out, size_t size, const char* what, uintptr_t address) {
    Dl_info info{};
    if (address && dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fname) {
        const char* lib = strrchr(info.dli_fname, '/');
        snprintf(out, size, "crash: %s %#lx = %s+%#lx (%s)", what, (unsigned long)address, lib ? lib + 1 : info.dli_fname,
                 (unsigned long)(address - reinterpret_cast<uintptr_t>(info.dli_fbase)),
                 info.dli_sname ? info.dli_sname : "?");
    } else {
        snprintf(out, size, "crash: %s %#lx", what, (unsigned long)address);
    }
}

void OnCrash(int sig, siginfo_t* info, void* context) {
    VulkanDriverCrashed();  // a crash before the driver is confirmed: auto mode avoids Turnip next time
    char line[512];
    snprintf(line, sizeof(line), "crash: signal %d (%s) code %d addr %p on tid %d", sig, strsignal(sig), info->si_code,
             info->si_addr, int(gettid()));
    CrashWrite(line);
#if defined(__aarch64__)
    const auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
    DescribeAddress(line, sizeof(line), "pc", mc.pc);
    CrashWrite(line);
    DescribeAddress(line, sizeof(line), "lr", mc.regs[30]);
    CrashWrite(line);
#elif defined(__x86_64__)
    DescribeAddress(line, sizeof(line), "pc", static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
    CrashWrite(line);
#endif
    // The callers: for abort() the pc and lr above are both in libc, the caller that gave up is further up.
    struct Walk {
        char* line;
        size_t size;
        int depth;
    } walk = {line, sizeof(line), 0};
    _Unwind_Backtrace(
        [](_Unwind_Context* ctx, void* arg) -> _Unwind_Reason_Code {
            auto* w = static_cast<Walk*>(arg);
            uintptr_t pc = _Unwind_GetIP(ctx);
            if (!pc || w->depth >= 24) return _URC_END_OF_STACK;
            char what[16];
            snprintf(what, sizeof(what), "#%02d", w->depth++);
            DescribeAddress(w->line, w->size, what, pc);
            CrashWrite(w->line);
            return _URC_NO_REASON;
        },
        &walk);
    sigaction(sig, &g_old_actions[sig], nullptr);
    if (info->si_code <= 0) raise(sig);  // sent by kill/tgkill/abort: deliver again; a real fault re-runs on return
}

void InstallCrashHandler() {
    struct sigaction sa {};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, &g_old_actions[sig]);
}

}  // namespace

std::string ArenaUsage();    // memory.cpp

void Fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("FATAL: %s", buf);
    VulkanDriverAbandon();  // not the driver's fault: the next start keeps trying Turnip
    _exit(70);
}

// A start that cannot go on: tell the user and end the process. Returning from main would let SDL run main again in
// the same process, on top of this run's guest memory.
[[noreturn]] void StartupFailure(const char* what) {
    Log("startup failed: %s", what);
    VulkanDriverAbandon();
    std::string msg = std::string(what) + "\n\nDetails are in mafia2-native.log.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mafia II", msg.c_str(), nullptr);
    _exit(1);
}

uint32_t HostWindow();  // user32.cpp
void InitClientSize(int window_w, int window_h);
void StartDebugConsole();
void StartWatchdog(std::atomic<uint64_t>* frames);  // watchdog.cpp
extern std::atomic<uint64_t> g_frames;  // d3d9.cpp
void InitD3D9Bridge();  // d3d9.cpp
void SelectVulkanDriver();  // vulkan_driver.cpp
void InitInput();  // input.cpp
void StartProfiler();  // profiler.cpp
void ForwardInput(const SDL_Event& e);
void InjectKey(SDL_Scancode sc, bool down);
extern SDL_Window* g_sdl_window;

}  // namespace native


// ---- performance counters and frame limiter (read by the on-screen overlay through JNI) ----
namespace native {
std::atomic<float> g_fps{0}, g_frame_ms{0};
std::atomic<int> g_fps_limit{0};  // 0 = no limit (the swap interval still applies)

}  // namespace native


// big.LITTLE: keep the engine's main thread on the fast cores (cpu_capacity >= half of the largest). The scheduler
// usually gets there by itself, but a frame-bound thread that waits on the GPU looks idle to it now and then and
// lands on a small core for a while. Setting big_cores (Android default 1).
#ifdef __ANDROID__
// The fast cores (cpu_capacity >= half of the largest); false when not big.LITTLE or unknown.
static bool BigCoreSet(cpu_set_t* set, int* big_out, int* n_out) {
    int caps[64] = {}, max_cap = 0, n = 0;
    for (; n < 64; ++n) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", n);
        FILE* f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%d", &caps[n]) != 1) caps[n] = 0;
        fclose(f);
        if (caps[n] > max_cap) max_cap = caps[n];
    }
    if (n == 0 || max_cap == 0) return false;
    CPU_ZERO(set);
    int big = 0;
    for (int i = 0; i < n; ++i) {
        if (caps[i] * 2 >= max_cap) {
            CPU_SET(i, set);
            ++big;
        }
    }
    *big_out = big;
    *n_out = n;
    return big != 0 && big != n;
}
static bool PinTid(int tid, const char* what) {
    cpu_set_t set;
    int big = 0, n = 0;
    if (!BigCoreSet(&set, &big, &n)) return false;
    int r = sched_setaffinity(tid, sizeof(set), &set);
    native::Log("thread %d%s%s pinned to %d of %d cores%s", tid, what ? " " : "", what ? what : "", big, n, r == 0 ? "" : " (failed)");
    return r == 0;
}
static bool BigCoresAllowed() {
    // battery_saver: the scheduler picks the cores (the biggest ones draw several times the power per clock)
    return native::SettingInt("big_cores", 1) && !native::SettingInt("battery_saver", 0);
}
#endif

void PinToBigCores() {
#ifdef __ANDROID__
    if (BigCoresAllowed()) PinTid(int(gettid()), nullptr);
#endif
}

namespace native { void PerfHintAddThread(int tid); }  // perf_hint.cpp
// DXVK's own threads feed the GPU: dxvk-cs records every D3D call the render thread makes into Vulkan commands,
// dxvk-submit/dxvk-queue submit and present. On a small core they starve the GPU between passes (passes of 0-6
// draws that take 0.5-6 ms on the GPU's clock). Called from Present every 2 s until all four are found: each goes on
// the fast cores and into the performance hint session. Setting dxvk_big_cores (Android default 1).
void PinDxvkThreads() {
#ifdef __ANDROID__
    static const bool on = BigCoresAllowed() && native::SettingInt("dxvk_big_cores", 1);
    static int found = 0, scans = 0;
    static std::vector<int> done;
    if (!on || found >= 4 || scans > 60) return;
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2)) return;
    last = now;
    ++scans;
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    while (dirent* e = readdir(d)) {
        int tid = atoi(e->d_name);
        if (tid <= 0 || std::find(done.begin(), done.end(), tid) != done.end()) continue;
        char path[64], name[32] = "";
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        if (FILE* f = fopen(path, "r")) {
            if (!fgets(name, sizeof name, f)) name[0] = 0;
            fclose(f);
        }
        name[strcspn(name, "\n")] = 0;
        if (strcmp(name, "dxvk-cs") && strcmp(name, "dxvk-submit") && strcmp(name, "dxvk-queue") && strcmp(name, "dxvk-frame")) continue;
        done.push_back(tid);
        ++found;
        PinTid(tid, name);
        native::PerfHintAddThread(tid);
    }
    closedir(d);
#endif
}

using namespace native;

#ifdef __ANDROID__
// DXVK and FAudio report errors on stderr, which goes nowhere in an app: forward it to logcat (tag mafia2re-err).
static void RedirectStderrToLogcat() {
    static int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], 2);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([] {
        char buf[1024];
        std::string line;
        for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) > 0;) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_write(ANDROID_LOG_WARN, "mafia2re-err", line.c_str());
                    line.clear();
                } else {
                    line += buf[i];
                }
            }
        }
    }).detach();
}
#endif

// The port's credit. On Android the Java side hands over its own copy (SetupActivity.CREDIT); a build where either
// was removed or changed does not start.
constexpr const char* kCredit = "Mafia II Android port by VinTeRuS";

int main(int argc, char** argv) {
#ifdef __ANDROID__
    RedirectStderrToLogcat();
    setenv("DXVK_LOG_LEVEL", "info", 0);
    if (const char* credit = getenv("MAFIA2RE_CREDIT"); !credit || strcmp(credit, kCredit) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kCredit,
                                 "This build of the Mafia II Android port was modified: the author's credit is missing.\n"
                                 "The port is made by VinTeRuS - patreon.com/cw/VinTeRuS",
                                 nullptr);
        return 1;
    }
#endif
    // The game root: the folder with SporebinEP1/, Data/, DataEP1/ (C:\Game for the guest).
    g_game_dir = argc > 1 ? argv[1] : getenv("MAFIA2RE_GAME");
#ifdef __ANDROID__
    // Logs, mafia2re.ini, saves (native_c) and the DXVK caches live in the app's external files folder
    // (/sdcard/Android/data/com.mafia2re.game/files); the game is <that folder>/game unless SetupActivity found it
    // elsewhere (MAFIA2RE_GAME).
    static std::string android_game;
    const char* base = SDL_AndroidGetExternalStoragePath();
    if (!base) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kCredit, SDL_GetError(), nullptr);
        return 1;
    }
    if (chdir(base) != 0) return 1;
    g_data_dir = base;
#ifdef MAFIA2RE_PGO_GEN
    __llvm_profile_set_filename((std::string(base) + "/mafia2.profraw").c_str());
#endif
    setenv("DXVK_STATE_CACHE_PATH", base, 0);
    setenv("DXVK_LOG_PATH", base, 0);
    if (!g_game_dir) {
        android_game = std::string(base) + "/game";
        g_game_dir = android_game.c_str();
    }
    if (std::string exe = std::string(g_game_dir) + "/pc/mafia2.exe"; access(exe.c_str(), R_OK) != 0) {
        std::string msg = "Cannot read " + exe + " (" + strerror(errno) + ").\nCopy the game files there.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kCredit, msg.c_str(), nullptr);
        return 1;
    }
#endif
    if (!g_game_dir) {
        fprintf(stderr, "usage: %s <game directory>\n", argv[0]);
        return 2;
    }
    rename("mafia2-native.log", "mafia2-native.prev.log");  // the previous run's log survives one restart
    g_log = fopen("mafia2-native.log", "w");
    InstallCrashHandler();
    Log("%s - build %s %s", kCredit, __DATE__, __TIME__);
    InitGuestMemory();
    if (!PageAlloc(kExtraReserve, kPhysxBase)) Fatal("cannot reserve the PhysX address range");
    GuestThread* main_thread = AttachThread(1u << 20);  // the exe's SizeOfStackReserve
    StartProfiler();  // cpu_profile = 1: sampled hot spots in the log
    PinToBigCores();  // the game's main thread runs its whole frame: keep it on the fast cores
    // ADPF: the frame budget from fps_limit (60 when unlimited); Present reports each frame's work (d3d9.cpp).
    {
        int limit = SettingInt("fps_limit", 0);
        PerfHintStart(limit > 0 ? limit : 60);
    }
    InitD3D9Bridge();
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);  // DXVK native: the HWNDs we pass are SDL windows
    SelectVulkanDriver();
    if (SettingInt("present_stats", 0)) setenv("DXVK_PRESENT_STATS", "1", 0);
    if (std::string hud = Setting("dxvk_hud", ""); !hud.empty()) setenv("DXVK_HUD", hud.c_str(), 1);
    if (std::string dc = Setting("dxvk_config", ""); !dc.empty()) {
        for (char& ch : dc) ch = ch == ',' ? ';' : ch;
        setenv("DXVK_CONFIG", dc.c_str(), 0);
    }
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) Fatal("SDL_Init: %s", SDL_GetError());
    // The window exists before the game asks for one: CreateWindowExW hands out this one (user32.cpp).
    g_sdl_window = SDL_CreateWindow("Mafia II - port by VinTeRuS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
#ifdef __ANDROID__
                                        | SDL_WINDOW_FULLSCREEN
#endif
    );
    if (!g_sdl_window) Fatal("SDL_CreateWindow: %s", SDL_GetError());
    int window_w, window_h;
    SDL_GetWindowSize(g_sdl_window, &window_w, &window_h);
#ifdef __ANDROID__
    SDL_DisplayMode mode;  // the window becomes the whole screen; its size is not known yet
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
        window_w = mode.w > mode.h ? mode.w : mode.h;  // landscape
        window_h = mode.w > mode.h ? mode.h : mode.w;
    }
#endif
    InitClientSize(window_w, window_h);
    StartDebugConsole();

    std::string exe = std::string(g_game_dir) + "/pc/mafia2.exe";
    if (!LoadImage(exe.c_str(), &g_engine, kEngineBase)) StartupFailure("Cannot load mafia2.exe.");
    // GOG's build still drives Steam interfaces through common.dll; their thiscall methods pop an unknown number of
    // argument dwords, which a generic stub cannot do. Steam integration is switched off (the flag the game itself checks).
    W8(0x01a95fecu, 0);
    g_engine_lo = g_engine.base;
    g_engine_hi = g_engine.base + g_engine.size;
    SetupStaticTls(main_thread);  // the main thread was attached before the exe (and its TLS template) existed

    RecompLookup(0);  // builds the function table now, while this is the only guest thread
    StartWatchdog(&g_frames);  // counted by Present (d3d9.cpp)
    // TLS callbacks (DLL_PROCESS_ATTACH), as the Windows loader runs them before the entry point.
    for (uint32_t cb = g_static_tls.callbacks; cb && R32(cb); cb += 4) {
        Log("TLS callback %08x", R32(cb));
        GuestCall(R32(cb), {g_engine.base, 1, 0});
    }
    Log("entry point %08x", g_engine.entry);
    uint32_t code = GuestCall(g_engine.entry, {});
    // The CRT startup normally leaves through ExitProcess; returning means it gave up.
    Log("entry point returned %u", code);
    return int(code);
}
