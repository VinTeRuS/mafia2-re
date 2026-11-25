// Host-side Lua: runs a chunk inside the game's own Lua state. The chunk runs from the main thread's game tick, where no
// Lua is on the stack; the state comes from the first lua_pcall. A chunk arrives as the file
// lua_cmd.lua in the working folder and its result (or error) goes to lua_out.txt. Base for the multiplayer layer.
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "guest.h"

namespace native {
extern std::atomic<uint64_t> g_frames;
namespace {
// Lua 5.1 in mafia2.exe (found through the base library's luaL_Reg table; see docs/multiplayer.md)
constexpr uint32_t kGetTop = 0x5d2800, kSetTop = 0x5d2820, kToLString = 0x5d3060, kLoadBuffer = 0x5d5bd0, kPCall = 0x5d3f80;
constexpr uint32_t kBufSize = 1 << 16;

thread_local bool t_inside = false;
uint64_t g_next_check = 0;
uint32_t g_L = 0;

uint32_t Eval(Ctx* c, uint32_t L, const std::string& src, std::string& out) {
    static uint32_t buf = HeapAlloc(kBufSize);
    if (src.size() >= kBufSize - 16) return 1;
    memcpy(G(buf), src.data(), src.size());
    memcpy(G(buf + kBufSize - 16), "=host", 6);
    uint32_t top = GuestCallFrom(c, kGetTop, {L});
    uint32_t status = GuestCallFrom(c, kLoadBuffer, {L, buf, uint32_t(src.size()), buf + kBufSize - 16, 0});
    if (status == 0) status = GuestCallFrom(c, kPCall, {L, 0, 1, 0});
    uint32_t s = GuestCallFrom(c, kToLString, {L, 0xffffffffu, 0});
    out = s ? Str(s) : "(no string result)";
    GuestCallFrom(c, kSetTop, {L, top});
    return status;
}

// Player state for the touch controls: "<bits>|<what GetOwner returned>". Bit 1: a player exists, bit 2: the player
// sits in a vehicle (GetOwner returns something other than the player).
const char kStateChunk[] = R"(local p = game and game.game and game.game:GetActivePlayer()
if not p then return "0|" end
local ok, o = pcall(p.GetOwner, p)
if ok and o ~= nil and o ~= p then return "3|" .. tostring(o) end
return "1|" .. tostring(ok) .. ":" .. tostring(o))";

// Logged once per run when the player first exists: which methods the wrappers have, to check the state chunk.
const char kProbeChunk[] = R"(local p = game.game:GetActivePlayer()
local out = {}
local function names(t)
  local m, mt, n = {}, getmetatable(t), 0
  while mt and n < 5 do
    local idx = rawget(mt, "__index")
    if type(idx) == "table" then for k in pairs(idx) do m[#m + 1] = tostring(k) end end
    mt = type(idx) == "table" and getmetatable(idx) or nil
    n = n + 1
  end
  table.sort(m)
  return table.concat(m, ",")
end
local g = {}
for k in pairs(game) do g[#g + 1] = tostring(k) end
out[#out + 1] = "game: " .. table.concat(g, ",")
out[#out + 1] = "player: " .. names(p)
return table.concat(out, "\n"))";

std::atomic<int> g_state{0};  // bits for nativeGameState: 1 player, 2 in a vehicle, 8 the state chunk works
std::string g_last_state;
bool g_probed = false;

// Runs the state chunk on L; publishes the result only when `publish` (L is the state that has the player).
int UpdateState(Ctx* c, uint32_t L, bool publish = true) {
    std::string out;
    uint32_t status = Eval(c, L, kStateChunk, out);
    int bits = 0;
    if (status == 0) bits = 8 | ((out.size() && out[0] == '1') ? 1 : 0) | ((out.size() && out[0] == '3') ? 3 : 0);
    if (!publish) return bits;
    g_state.store(bits, std::memory_order_relaxed);
    if (out != g_last_state) {
        Log("player state: status %u: %.200s", status, out.c_str());
        g_last_state = out;
    }
    if ((bits & 1) && !g_probed) {
        g_probed = true;
        Eval(c, L, kProbeChunk, out);
        for (size_t i = 0; i < out.size(); i += 900) Log("lua probe: %s", out.substr(i, 900).c_str());
    }
    return bits;
}

void RunChunk(Ctx* c, uint32_t L, const std::string& src) {
    std::string out;
    uint32_t status = Eval(c, L, src, out);
    for (size_t i = 0; i == 0 || i < out.size(); i += 900) Log("lua: status %u: %s", status, out.substr(i, 900).c_str());
    if (FILE* f = fopen("lua_out.txt", "w")) {
        fprintf(f, "%u\n%s\n", status, out.c_str());
        fclose(f);
    }
}
}  // namespace
}  // namespace native

using namespace native;

// lua_pcall(L, ...): remembers the Lua state the game tick's thread used last. The first state the game calls into
// is not always the one that lives on (running a chunk on it crashed in luaL_loadbuffer with a dead global state),
// and other threads' states are not ours to touch.
thread_local bool t_tick_thread = false;
std::atomic<uint32_t> g_tick_L{0};
extern "C" void Hook_LuaPcall(Ctx* c) {
    if (t_tick_thread) g_tick_L.store(R32(c->esp + 4), std::memory_order_relaxed);
}

namespace {
// A Lua 5.1 lua_State (32-bit): tt (byte 4) is LUA_TTHREAD, top +8, base +12, l_G +16.
bool StateLooksAlive(uint32_t L) {
    if (!L || (L & 3) || L >= 0xfff00000u) return false;
    if (R8(L + 4) != 8) return false;  // LUA_TTHREAD
    uint32_t top = R32(L + 8), base = R32(L + 12), g = R32(L + 16);
    return g && !(g & 3) && g < 0xfff00000u && top && base && top >= base;
}
// The main thread of L's global state: global_State.mainthread sits at +100 (float lua_Number, 8-byte TValue
// registry) or +112 (double); the right one points back to the same global state. 0 when neither does.
uint32_t MainThread(uint32_t L) {
    if (!StateLooksAlive(L)) return 0;
    uint32_t g = R32(L + 16);
    for (uint32_t off : {100u, 112u}) {
        uint32_t m = R32(g + off);
        if (StateLooksAlive(m) && R32(m + 16) == g) return m;
    }
    return 0;
}
}  // namespace

// The main thread's game tick: no Lua on the stack here, so a chunk can run on the main state.
extern "C" void Hook_GameTick(Ctx* c) {
    t_tick_thread = true;
    uint64_t frame = g_frames.load(std::memory_order_relaxed);
    if (frame < g_next_check || t_inside) return;
    g_next_check = frame + 15;
    // The game runs several Lua states from this thread in turn. Each main thread seen is a candidate once it has
    // existed ~10 s; the one whose chunk finds a player is used from then on, while it stays alive.
    struct Cand {
        uint32_t L;
        uint64_t since;
    };
    static Cand s_cands[8];
    static int s_ncand = 0, s_next_slot = 0;
    static uint32_t s_good = 0;
    uint32_t L = MainThread(g_tick_L.load(std::memory_order_relaxed));
    if (L) {
        bool known = false;
        for (int i = 0; i < s_ncand; ++i) known |= s_cands[i].L == L;
        if (!known) {
            int slot = s_ncand < 8 ? s_ncand++ : (s_next_slot++ & 7);
            s_cands[slot] = {L, frame};
            Log("lua: main state %08x (candidate %d)", L, slot);
        }
    }
    if (s_good && MainThread(s_good) != s_good) {
        Log("lua: state %08x gone", s_good);
        s_good = 0;
    }
    static const bool s_touch_state = SettingInt("touch_state", 1) != 0;  // 0: the controls never ask the game
    if (s_touch_state && !s_good) {
        t_inside = true;
        for (int i = 0; i < s_ncand && !s_good; ++i) {
            const Cand& k = s_cands[i];
            if (frame - k.since < 300 || MainThread(k.L) != k.L) continue;
            if (UpdateState(c, k.L, false) & 1) {
                s_good = k.L;
                Log("lua: player found in state %08x", s_good);
            }
        }
        t_inside = false;
    }
    if (!s_good) {
        g_state.store(0, std::memory_order_relaxed);
        return;
    }
    g_L = s_good;
    if (s_touch_state) {
        t_inside = true;
        UpdateState(c, g_L);
        t_inside = false;
    }
    FILE* f = fopen("lua_cmd.lua", "r");
    if (!f) return;
    std::string src;
    char tmp[4096];
    for (size_t n; (n = fread(tmp, 1, sizeof tmp, f)) > 0;) src.append(tmp, n);
    fclose(f);
    unlink("lua_cmd.lua");
    t_inside = true;
    RunChunk(c, g_L, src);
    t_inside = false;
}

namespace native {
extern std::atomic<int> g_movies_open, g_menu_videos_open;
int GameStateBits() {  // as nativeGameState: 1 player, 2 vehicle, 4 movie, 8 known; 16 a menu background plays
    return g_state.load(std::memory_order_relaxed) | (g_movies_open.load(std::memory_order_relaxed) > 0 ? 4 : 0) |
           (g_menu_videos_open.load(std::memory_order_relaxed) > 0 ? 16 : 0);
}
}

#ifdef __ANDROID__
#include <jni.h>
// Bits for TouchControls: 1 a player exists, 2 in a vehicle, 4 a movie plays, 8 the player state is known.
extern "C" JNIEXPORT jint JNICALL Java_com_mafia2re_game_TouchControls_nativeGameState(JNIEnv*, jclass) {
    return native::g_state.load(std::memory_order_relaxed) | (native::g_movies_open.load(std::memory_order_relaxed) > 0 ? 4 : 0);
}
#endif
