// Live debug console (MAFIA2RE_DBG=<prefix>): a thread reads commands from the FIFO <prefix>.cmd and appends the answers to
// <prefix>.out, each answer closed by a line "<<done>>". tools/dbg.sh wraps this. It lets a running game be inspected
// and driven (guest memory, the Lua call stack and tables, keys, clicks, screenshots) without restarting it or gdb.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

#include <SDL2/SDL.h>

#include "guest.h"

namespace native {
extern std::atomic<uint64_t> g_frames;  // d3d9.cpp
extern std::atomic<float> g_fps;       // main.cpp
int GameStateBits();                    // m2_script.cpp

void InjectKey(SDL_Scancode sc, bool down);
void DebugClick(int x, int y);
std::string DebugTexture(uint32_t guest);
void DebugDrawShot(int n);
std::string ArenaUsage();

namespace {

FILE* g_out = nullptr;

bool Safe(uint32_t a, void* buf, size_t n) {
    iovec local{buf, n}, remote{reinterpret_cast<void*>(uintptr_t(kGuestBase) + a), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(n);
}
uint32_t U32(uint32_t a) {
    uint32_t v = 0;
    Safe(a, &v, 4);
    return v;
}
int32_t I32(uint32_t a) { return int32_t(U32(a)); }
std::string CStr(uint32_t a, size_t max = 60) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        char ch = 0;
        if (!Safe(a + uint32_t(i), &ch, 1) || !ch) break;
        s += (ch >= 32 && ch < 127) ? ch : '?';
    }
    return s;
}

// ---- Lua 5.0 as built into mafia2.exe: 8-byte TObject {int tt; 4-byte value (float numbers)}, RK constants >= 128.
std::string LVal(uint32_t a) {
    int tt = I32(a);
    uint32_t v = U32(a + 4);
    char b[96];
    switch (tt) {
        case 0: return "nil";
        case 1: return v ? "true" : "false";
        case 3: {
            float f;
            memcpy(&f, &v, 4);
            snprintf(b, sizeof b, "%g", f);
            return b;
        }
        case 4: return "\"" + CStr(v + 16) + "\"";
        case 5: snprintf(b, sizeof b, "table@%x", v); return b;
        case 6: snprintf(b, sizeof b, "func@%x", v); return b;
        default: snprintf(b, sizeof b, "tt%d@%x", tt, v); return b;
    }
}
void DumpTable(uint32_t t, int max = 80) {
    uint32_t arr = U32(t + 12), node = U32(t + 16);
    int sizearray = I32(t + 28);
    uint8_t lsz = 0;
    Safe(t + 7, &lsz, 1);
    int n = 0;
    for (int i = 0; i < sizearray && n < max; ++i, ++n) fprintf(g_out, "  [%d] = %s\n", i + 1, LVal(arr + 8 * uint32_t(i)).c_str());
    for (int i = 0; i < (1 << lsz) && n < max; ++i) {
        uint32_t nd = node + 20 * uint32_t(i);
        if (I32(nd + 8) != 0) {  // Lua 5.0 Node: key first, then value
            fprintf(g_out, "  %s = %s\n", LVal(nd).c_str(), LVal(nd + 8).c_str());
            ++n;
        }
    }
}
std::string DebugLuaArgsImpl() {
    uint32_t L = U32(0x1d77f68), ci = U32(L + 0x14), base = U32(ci), top = U32(L + 8);
    std::string s;
    for (uint32_t a = base; a < top && a < base + 8 * 12; a += 8) s += LVal(a) + " ";
    return s;
}

void LuaStack(bool tables) {
    uint32_t L = U32(0x1d77f68);
    uint32_t ci = U32(L + 0x14), base_ci = U32(L + 0x28);
    fprintf(g_out, "L=%x\n", L);
    for (int n = 0; ci >= base_ci && n < 30; ++n, ci -= 24) {
        uint32_t base = U32(ci), func = base - 8;
        if (I32(func) != 6) {
            fprintf(g_out, "frame %d: tt=%d\n", n, I32(func));
            continue;
        }
        uint32_t cl = U32(func + 4);
        uint8_t isC = 0;
        Safe(cl + 6, &isC, 1);
        if (isC) {
            fprintf(g_out, "frame %d: C function %x\n", n, U32(cl + 12));
            continue;
        }
        uint32_t pr = U32(cl + 12), k = U32(pr + 8);
        int nk = I32(pr + 40);
        fprintf(g_out, "frame %d: Lua defined at line %d, constants:", n, I32(pr + 60));
        for (int i = 0; i < nk && i < 24; ++i) {
            if (I32(k + 8 * uint32_t(i)) == 4) fprintf(g_out, " %s", CStr(U32(k + 8 * uint32_t(i) + 4) + 16, 32).c_str());
        }
        fprintf(g_out, "\n");
        if (tables && I32(base) == 5) {
            fprintf(g_out, "  R0 (self) table fields:\n");
            DumpTable(U32(base + 4), 60);
        }
    }
}

// Value (address of the TObject) of a global variable: the globals table is the table-typed TObject inside lua_State.
uint32_t FindGlobal(const std::string& name) {
    uint32_t L = U32(0x1d77f68);
    for (uint32_t off = 0x30; off < 0x90; off += 4) {
        if (I32(L + off) != 5) continue;
        uint32_t t = U32(L + off + 4);
        uint8_t lsz = 0;
        if (!Safe(t + 7, &lsz, 1) || lsz > 20) continue;
        uint32_t node = U32(t + 16);
        for (int i = 0; i < (1 << lsz); ++i) {
            uint32_t nd = node + 20 * uint32_t(i);
            if (I32(nd + 8) != 0 && I32(nd) == 4 && CStr(U32(nd + 4) + 16, 80) == name) return nd + 8;
        }
    }
    return 0;
}

// All globals whose names contain `pattern` (with a short value).
void ListGlobals(const std::string& pattern) {
    uint32_t L = U32(0x1d77f68);
    uint32_t t = U32(L + 0x44);
    uint8_t lsz = 0;
    Safe(t + 7, &lsz, 1);
    uint32_t node = U32(t + 16);
    for (int i = 0; i < (1 << lsz); ++i) {
        uint32_t nd = node + 20 * uint32_t(i);
        if (I32(nd + 8) == 0 || I32(nd) != 4) continue;
        std::string k = CStr(U32(nd + 4) + 16, 80);
        if (k.find(pattern) != std::string::npos) fprintf(g_out, "%s = %s\n", k.c_str(), LVal(nd + 8).c_str());
    }
}

void Run(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "u32") {
        uint32_t a = 0, n = 1;
        in >> std::hex >> a;
        in >> std::dec >> n;
        for (uint32_t i = 0; i < n; ++i) fprintf(g_out, "%08x: %08x\n", a + 4 * i, U32(a + 4 * i));
    } else if (cmd == "mem") {
        uint32_t a = 0, n = 64;
        in >> std::hex >> a >> std::dec >> n;
        for (uint32_t o = 0; o < n; o += 16) {
            fprintf(g_out, "%08x:", a + o);
            for (uint32_t i = 0; i < 16 && o + i < n; ++i) {
                uint8_t b = 0;
                Safe(a + o + i, &b, 1);
                fprintf(g_out, " %02x", b);
            }
            fprintf(g_out, "\n");
        }
    } else if (cmd == "str") {
        uint32_t a = 0;
        in >> std::hex >> a;
        fprintf(g_out, "%s\n", CStr(a, 200).c_str());
    } else if (cmd == "lua") {
        LuaStack(false);
    } else if (cmd == "luat") {
        LuaStack(true);
    } else if (cmd == "table") {
        uint32_t a = 0;
        in >> std::hex >> a;
        DumpTable(a, 200);
    } else if (cmd == "global") {  // global NAME: value of a Lua global; tables are dumped
        std::string name;
        in >> name;
        uint32_t nd = FindGlobal(name);
        if (!nd) {
            fprintf(g_out, "no global '%s'\n", name.c_str());
        } else {
            fprintf(g_out, "%s = %s\n", name.c_str(), LVal(nd).c_str());
            if (I32(nd) == 5) DumpTable(U32(nd + 4), 120);
        }
    } else if (cmd == "globals") {  // globals PATTERN: global names containing PATTERN
        std::string p;
        in >> p;
        ListGlobals(p);
    } else if (cmd == "key") {
        std::string name;
        in >> name;
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) {
            fprintf(g_out, "unknown key '%s'\n", name.c_str());
        } else {
            InjectKey(sc, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            InjectKey(sc, false);
            fprintf(g_out, "pressed %s\n", name.c_str());
        }
    } else if (cmd == "click") {
        int x = 0, y = 0;
        in >> x >> y;
        DebugClick(x, y);
        fprintf(g_out, "clicked %d %d\n", x, y);
    } else if (cmd == "shot") {
        unlink("shot_now.ppm");
        FILE* f = fopen("shot_now", "w");
        if (f) fclose(f);
        for (int i = 0; i < 100 && access("shot_now.ppm", F_OK) != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(g_out, "%s\n", access("shot_now.ppm", F_OK) == 0 ? "shot_now.ppm written" : "no screenshot");
    } else if (cmd == "trace") {  // trace MS: every import/D3D call for MS milliseconds goes to mafia2-native.log
        int ms = 100;
        in >> ms;
        g_trace_until_ns = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) + uint64_t(ms) * 1000000ull;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms + 50));
        fprintf(g_out, "traced %d ms\n", ms);
    } else if (cmd == "fn") {  // fn add A... | fn dump | fn reset | fn clear
        std::string sub;
        in >> sub;
        if (sub == "add" || sub == "addlua") {  // addlua: also record the Lua arguments (Lua C functions)
            uint32_t a;
            while (in >> std::hex >> a) TraceAdd(a, sub == "addlua");
            fprintf(g_out, "ok\n");
        } else if (sub == "watch") {  // fn watch FUNC ADDR: record the dword at ADDR whenever FUNC is called
            uint32_t f = 0, a = 0;
            in >> std::hex >> f >> a;
            TraceWatch(f, a);
            fprintf(g_out, "ok\n");
        } else if (sub == "log") {  // fn log FUNC...: every call of FUNC is appended to fnlog.txt
            uint32_t a;
            while (in >> std::hex >> a) TraceLog(a);
            fprintf(g_out, "ok\n");
        } else if (sub == "clear") {
            TraceClear();
            fprintf(g_out, "cleared\n");
        } else {
            fprintf(g_out, "%s", TraceDump(sub == "reset").c_str());
        }
    } else if (cmd == "tex") {  // tex ADDR: texture level 0 statistics
        uint32_t a = 0;
        in >> std::hex >> a;
        fprintf(g_out, "%s", DebugTexture(a).c_str());
    } else if (cmd == "drawshot") {  // drawshot N: picture right after the N-th draw call of a frame -> shot_draw.ppm
        int n = 0;
        in >> n;
        DebugDrawShot(n);
        for (int i = 0; i < 100 && access("shot_draw.ppm", F_OK) != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(g_out, "%s\n", access("shot_draw.ppm", F_OK) == 0 ? "shot_draw.ppm written" : "no screenshot");
    } else if (cmd == "heap") {
        fprintf(g_out, "arena %s\n", ArenaUsage().c_str());
        if (FILE* f = fopen("/proc/self/status", "r")) {
            char l[128];
            while (fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS", 5) || !strncmp(l, "VmSize", 6)) fputs(l, g_out);
            fclose(f);
        }
    } else if (cmd == "mouse") {  // mouse X Y [click]: the engine's cursor to client coordinates (needs MAFIA2RE_MOUSE_STEP)
        int x = 0, y = 0;
        std::string how;
        in >> std::dec >> x >> y >> how;
        DebugMouse(x, y, how == "click");
        fprintf(g_out, "ok\n");
    } else if (cmd == "blocks") {
        fprintf(g_out, "%s", ArenaBlocks().c_str());
    } else if (cmd == "memchrtest") {  // the guest CRT's memchr (0x752cb0) on a string with ';' at every position and alignment
        static thread_local GuestThread* gt = AttachThread(1u << 20);
        (void)gt;
        uint32_t buf = HeapAlloc(256);
        int bad = 0;
        for (uint32_t align = 0; align < 4; ++align)
            for (uint32_t len = 1; len <= 40; ++len)
                for (uint32_t pos = 0; pos < len; ++pos) {
                    uint32_t p = (buf + 16 + align);
                    for (uint32_t i = 0; i < len; ++i) W8(p + i, i == pos ? ';' : 'a' + i % 20);
                    uint32_t r = GuestCall(0x752cb0, {p, ';', len});
                    if (r != p + pos) {
                        if (bad++ < 12) fprintf(g_out, "FAIL align %u len %u pos %u -> %x (want %x)\n", align, len, pos, r, p + pos);
                    }
                }
        fprintf(g_out, "memchr failures: %d\n", bad);
    } else if (cmd == "run") {  // run LUA...: a chunk on the game's main thread (lua_cmd.lua), its result
        std::string code;
        std::getline(in, code);
        unlink("lua_out.txt");
        if (FILE* f = fopen("lua_cmd.lua", "w")) {
            fputs(code.c_str(), f);
            fclose(f);
        }
        std::string out = "(no answer in 5 s: is a mission loaded?)";
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (FILE* f = fopen("lua_out.txt", "r")) {
                std::string s;
                char b[1024];
                for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
                fclose(f);
                if (!s.empty()) { out = s; break; }
            }
        }
        fprintf(g_out, "%s\n", out.c_str());
    } else if (cmd == "status") {  // frame, fps, player state bits (autoplay's), resident memory
        long pages = 0, resident = 0;
        if (FILE* f = fopen("/proc/self/statm", "r")) {
            if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
            fclose(f);
        }
        const int s = GameStateBits();
        fprintf(g_out, "frame %llu, %.1f fps, state %d (player %d, vehicle %d, movie %d, menu video %d), %ld MiB resident\n",
                (unsigned long long)g_frames.load(), double(g_fps.load()), s, s & 1, (s >> 1) & 1, (s >> 2) & 1, (s >> 4) & 1,
                resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024);
    } else if (cmd == "logtail") {  // logtail [N]: the last N lines of mafia2-native.log
        int n = 40;
        in >> n;
        std::vector<std::string> lines;
        if (FILE* f = fopen("mafia2-native.log", "r")) {
            char b[2048];
            while (fgets(b, sizeof b, f)) {
                lines.emplace_back(b);
                if (lines.size() > size_t(n)) lines.erase(lines.begin());
            }
            fclose(f);
        }
        for (const std::string& l : lines) fputs(l.c_str(), g_out);
    } else if (cmd == "ping") {
        fprintf(g_out, "pong\n");
    } else {
        fprintf(g_out, "commands: u32 A [N] | mem A N | str A | lua | luat | table A | key NAME | click X Y | shot | trace MS | tex A | global NAME | fn add A.. / dump / reset / clear | ping\n");
    }
}

void Loop(std::string prefix) {
    std::string cmd_path = prefix + ".cmd", out_path = prefix + ".out";
    unlink(cmd_path.c_str());
    mkfifo(cmd_path.c_str(), 0666);
    for (;;) {
        FILE* in = fopen(cmd_path.c_str(), "r");  // blocks until a client writes
        if (!in) return;
        char line[512];
        while (fgets(line, sizeof line, in)) {
            g_out = fopen(out_path.c_str(), "a");
            if (!g_out) continue;
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            Run(l);
            fprintf(g_out, "<<done>>\n");
            fclose(g_out);
        }
        fclose(in);
    }
}

// The same console over TCP on 127.0.0.1 (setting dbg_port): on a phone `adb forward tcp:P tcp:P` reaches it, and
// tools/mafia2_mcp.py speaks it. One client at a time; each command line is answered, then "<<done>>".
void TcpLoop(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 1) != 0) {
        Log("debug console: cannot listen on port %d", port);
        close(s);
        return;
    }
    Log("debug console on 127.0.0.1:%d", port);
    for (;;) {
        int c = accept(s, nullptr, nullptr);
        if (c < 0) continue;
        FILE* in = fdopen(c, "r");
        FILE* out = fdopen(dup(c), "w");
        if (!in || !out) {
            if (in) fclose(in); else close(c);
            if (out) fclose(out);
            continue;
        }
        char line[4096];
        while (fgets(line, sizeof line, in)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            g_out = out;
            Run(l);
            fprintf(out, "<<done>>\n");
            fflush(out);
        }
        fclose(in);
        fclose(out);
    }
}

}  // namespace

std::string DebugLuaArgs() { return DebugLuaArgsImpl(); }

void StartDebugConsole() {
    if (const char* fl = getenv("MAFIA2RE_FNLOG")) {  // hex guest function addresses: every call goes to fnlog.txt
        std::istringstream in(fl);
        uint32_t a;
        while (in >> std::hex >> a) TraceLog(a);
    }
    if (int port = SettingInt("dbg_port", 0)) std::thread(TcpLoop, port).detach();
    const char* p = getenv("MAFIA2RE_DBG");
    if (!p || !*p) return;
    std::thread(Loop, std::string(p)).detach();
    Log("debug console on %s.cmd / %s.out", p, p);
}

}  // namespace native
