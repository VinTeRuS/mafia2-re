// Import thunks: each imported dll!name gets a 16-byte slot at g_thunk_base + 16*index (an arena block: a fixed
// address may be taken on Android, where ART maps its heap below 4 GiB). Generated code calls through
// the IAT with RecompDispatch(target), and target falls in this range. Data imports (MSVCR80 _HUGE, _adjust_fdiv)
// keep their value in the slot itself.
#include <sys/mman.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "guest.h"

namespace native {

std::atomic<uint64_t> g_trace_until_ns{0};  // debug console `trace`: log every import call until this time

namespace {

uint32_t g_thunk_base = 0;
constexpr uint32_t kThunkSize = 0x00100000u;
constexpr uint32_t kSlot = 16;

struct Slot {
    std::string dll, name;
    ImportFn fn;
};

std::map<std::string, ImportFn>& Handlers() {
    static std::map<std::string, ImportFn> handlers;
    return handlers;
}

std::vector<Slot>& Slots() {
    static std::vector<Slot> slots;
    return slots;
}
std::vector<Slot>* g_slots_cache = nullptr;  // Slots(), cached for DispatchImport
int g_import_slow = -1;  // DispatchImport: 1 when import tracing or profiling is on, -1 not known yet

std::string Key(const char* dll, const char* name) { return std::string(dll) + "!" + name; }

// Setting import_profile: every thread logs, every 10 s, where its wall time went - the imports (WinAPI, D3D9
// methods) it spent the most time in, inclusive of nested calls, and the rest (guest code). Finds the waits that
// keep a frame long while no thread is busy.
bool ImportProfiling() {
    static const bool on = SettingInt("import_profile", 0) != 0;
    return on;
}

struct ThreadProfile {
    using Clock = std::chrono::steady_clock;
    Clock::time_point window = Clock::now();
    std::vector<uint64_t> ns, calls;
    int depth = 0;
};

void ProfiledCall(const Slot& s, uint32_t index, Ctx* ctx) {
    thread_local ThreadProfile tp;
    if (tp.ns.size() <= index) {
        tp.ns.resize(index + 64);
        tp.calls.resize(index + 64);
    }
    auto t0 = ThreadProfile::Clock::now();
    ++tp.depth;
    s.fn(ctx);
    --tp.depth;
    auto t1 = ThreadProfile::Clock::now();
    tp.ns[index] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    ++tp.calls[index];
    if (tp.depth || t1 - tp.window < std::chrono::seconds(10)) return;
    const double wall = std::chrono::duration<double, std::milli>(t1 - tp.window).count();
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < tp.ns.size(); ++i) {
        if (tp.calls[i]) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return tp.ns[a] > tp.ns[b]; });
    std::string line;
    double in_imports = 0;
    for (uint32_t i : order) in_imports += double(tp.ns[i]) / 1e6;
    auto& slots = Slots();
    for (size_t k = 0; k < order.size() && k < 10; ++k) {
        uint32_t i = order[k];
        char buf[160];
        snprintf(buf, sizeof(buf), " %s %.0f%% (%llu)", slots[i].name.c_str(), 100.0 * double(tp.ns[i]) / 1e6 / wall,
                 (unsigned long long)tp.calls[i]);
        line += buf;
    }
    Log("imports tid %d: guest %.0f%% |%s", int(gettid()), 100.0 * (wall - in_imports) / wall, line.c_str());
    std::fill(tp.ns.begin(), tp.ns.end(), 0);
    std::fill(tp.calls.begin(), tp.calls.end(), 0);
    tp.window = t1;
}

}  // namespace

void RegisterImports(const ImportDef* defs, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        Handlers()[Key(defs[i].dll, defs[i].name)] = defs[i].fn;
    }
}

uint32_t ImportThunk(const char* dll, const char* name) {
    if (!g_thunk_base) {
        g_thunk_base = PageAlloc(kThunkSize);
        if (!g_thunk_base) Fatal("cannot allocate the import thunk area");
    }
    std::string key = Key(dll, name);
    auto& slots = Slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].dll == dll && slots[i].name == name) return g_thunk_base + kSlot * uint32_t(i);
    }
    auto h = Handlers().find(key);
    slots.push_back({dll, name, h == Handlers().end() ? nullptr : h->second});
    uint32_t addr = g_thunk_base + kSlot * uint32_t(slots.size() - 1);
    if (strcmp(name, "_HUGE") == 0) {
        WF64(addr, HUGE_VAL);
    }
    if (strcmp(name, "_acmdln") == 0) {  // char* _acmdln: the command line the CRT parses
        W32(addr, GuestStrdup("\"C:\\Game\\mafia2.exe\""));
    }
    return addr;
}

bool HasImport(const char* dll, const char* name) { return Handlers().count(Key(dll, name)) != 0; }

bool InImportArea(uint32_t target) { return g_thunk_base && target >= g_thunk_base && target < g_thunk_base + kThunkSize; }

bool DispatchImport(Ctx* ctx, uint32_t target) {
    if (!g_thunk_base || target < g_thunk_base || target >= g_thunk_base + kThunkSize) return false;
    uint32_t index = (target - g_thunk_base) / kSlot;
    // 30 000 calls a frame: plain loads of values computed once, no function-local static guards on this path
    // (each is an acquire load on arm64).
    std::vector<Slot>* sp = g_slots_cache;
    if (__builtin_expect(!sp, 0)) sp = g_slots_cache = &Slots();
    auto& slots = *sp;
    if (index >= slots.size() || (target & (kSlot - 1))) {
        Fatal("call into the import area at %08x", target);
    }
    const Slot& s = slots[index];
    if (!s.fn) {
        Fatal("unimplemented import %s!%s (called from %08x, args %08x %08x %08x %08x)", s.dll.c_str(),
              s.name.c_str(), R32(ctx->esp), Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3));
    }
    // MAFIA2RE_TRACE_IMPORTS=<seconds>: every import call with its caller from that many seconds after the start
    // (0: from the beginning; tracing slows the game down a lot), for following what the game is doing.
    // (the clock is read only while a trace is armed: this runs for every import call, 30 000 a frame)
    if (uint64_t until = g_trace_until_ns.load(std::memory_order_relaxed);
        until && until > uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()))
    {
        Log("import %s!%s from %08x arg0 %08x %08x %08x %08x", s.dll.c_str(), s.name.c_str(), R32(ctx->esp), Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3));
        if (s.name.find("ShaderConstantF") != std::string::npos && Arg(ctx, 2)) {  // the registers' values
            std::string vals;
            for (uint32_t r = 0; r < Arg(ctx, 3) && r < 12; ++r) {
                char b[96];
                float v[4];
                for (int i = 0; i < 4; ++i) { uint32_t u = R32(Arg(ctx, 2) + 16 * r + 4 * uint32_t(i)); memcpy(&v[i], &u, 4); }
                snprintf(b, sizeof b, " [%g %g %g %g]", v[0], v[1], v[2], v[3]);
                vals += b;
            }
            Log("    c%u x%u:%s", Arg(ctx, 1), Arg(ctx, 3), vals.c_str());
        }
    }
    int slow = g_import_slow;
    if (__builtin_expect(slow < 0, 0)) slow = g_import_slow = (getenv("MAFIA2RE_TRACE_IMPORTS") || ImportProfiling()) ? 1 : 0;
    if (__builtin_expect(!slow, 1)) {
        s.fn(ctx);
        return true;
    }
    static const char* trace_env = getenv("MAFIA2RE_TRACE_IMPORTS");
    static const auto trace_from = std::chrono::steady_clock::now() + std::chrono::seconds(trace_env ? atoi(trace_env) : 0);
    if (trace_env && std::chrono::steady_clock::now() >= trace_from)
        Log("import %s!%s from %08x arg0 %08x %08x %08x %08x", s.dll.c_str(), s.name.c_str(), R32(ctx->esp), Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3));
    if (ImportProfiling() && s.name != "SwitchToFiber") {  // a fiber switch does not return until switched back
        ProfiledCall(s, index, ctx);
    } else {
        s.fn(ctx);
    }
    return true;
}

}  // namespace native
