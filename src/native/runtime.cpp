#include <algorithm>
// Host services for the recompiled code (runtime.h) in the native host: dispatch of computed calls, imports,
// setjmp/longjmp, cpuid, traps.
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <mutex>

#include <unistd.h>
#include <sys/syscall.h>
#include "guest.h"

extern "C" {
extern const struct RecompTableEntry {
    uint32_t va;
    RecompFn fn;
} g_recomp_table[];
extern const uint32_t g_recomp_count;
extern const struct RecompTableEntry g_recomp_table_physxcore[] __attribute__((weak));
extern const uint32_t g_recomp_count_physxcore __attribute__((weak));
extern const struct RecompTableEntry g_recomp_table_nxcooking[] __attribute__((weak));
extern const uint32_t g_recomp_count_nxcooking __attribute__((weak));
extern const struct RecompTableEntry g_recomp_table_apex_release[] __attribute__((weak));
extern const uint32_t g_recomp_count_apex_release __attribute__((weak));
extern const struct RecompTableEntry g_recomp_table_apex_clothing[] __attribute__((weak));
extern const uint32_t g_recomp_count_apex_clothing __attribute__((weak));
extern const struct RecompTableEntry g_recomp_table_apex_destructible[] __attribute__((weak));
extern const uint32_t g_recomp_count_apex_destructible __attribute__((weak));
extern const struct RecompTableEntry g_recomp_table_apex_emitter[] __attribute__((weak));
extern const uint32_t g_recomp_count_apex_emitter __attribute__((weak));
extern const struct RecompTableEntry g_recomp_table_apex_iofx[] __attribute__((weak));
extern const uint32_t g_recomp_count_apex_iofx __attribute__((weak));
extern const struct RecompTableEntry g_recomp_table_apex_particles[] __attribute__((weak));
extern const uint32_t g_recomp_count_apex_particles __attribute__((weak));

volatile uint32_t RecompRestrictLo = 0;
volatile uint32_t RecompRestrictHi = 0xffffffffu;
volatile uint32_t RecompTestDetours = 0;
uint8_t* RecompMemoryBase = nullptr;
}

namespace native {

namespace {

constexpr int kHashBits = 18;
struct HashSlot {
    uint32_t va;
    RecompFn fn;
};
HashSlot* g_hash = nullptr;

uint32_t HashOf(uint32_t va) { return (va * 2654435761u) >> (32 - kHashBits); }

void BuildHash() {
    g_hash = static_cast<HashSlot*>(calloc(size_t(1) << kHashBits, sizeof(HashSlot)));
    for (uint32_t i = 0; i < g_recomp_count; ++i) {
        uint32_t h = HashOf(g_recomp_table[i].va);
        while (g_hash[h].va) h = (h + 1) & ((1u << kHashBits) - 1);
        g_hash[h] = {g_recomp_table[i].va, g_recomp_table[i].fn};
    }
    auto add = [](const RecompTableEntry* table, uint32_t count) {  // extra images (PhysXCore, NxCooking)
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t h = HashOf(table[i].va);
            while (g_hash[h].va) h = (h + 1) & ((1u << kHashBits) - 1);
            g_hash[h] = {table[i].va, table[i].fn};
        }
    };
    if (&g_recomp_count_physxcore && g_recomp_table_physxcore) add(g_recomp_table_physxcore, g_recomp_count_physxcore);
    if (&g_recomp_count_nxcooking && g_recomp_table_nxcooking) add(g_recomp_table_nxcooking, g_recomp_count_nxcooking);
    if (&g_recomp_count_apex_release && g_recomp_table_apex_release) add(g_recomp_table_apex_release, g_recomp_count_apex_release);
    if (&g_recomp_count_apex_clothing && g_recomp_table_apex_clothing) add(g_recomp_table_apex_clothing, g_recomp_count_apex_clothing);
    if (&g_recomp_count_apex_destructible && g_recomp_table_apex_destructible) add(g_recomp_table_apex_destructible, g_recomp_count_apex_destructible);
    if (&g_recomp_count_apex_emitter && g_recomp_table_apex_emitter) add(g_recomp_table_apex_emitter, g_recomp_count_apex_emitter);
    if (&g_recomp_count_apex_iofx && g_recomp_table_apex_iofx) add(g_recomp_table_apex_iofx, g_recomp_count_apex_iofx);
    if (&g_recomp_count_apex_particles && g_recomp_table_apex_particles) add(g_recomp_table_apex_particles, g_recomp_count_apex_particles);
}

}  // namespace

// Ctx of the guest code that most recently called out to the host on this thread/fiber; host callbacks into guest
// code continue below its stack pointer.
thread_local Ctx* tl_ctx = nullptr;
uint32_t g_engine_lo = 0, g_engine_hi = 0;

}  // namespace native

using namespace native;

// ---- runtime call tracing (debug console `fn`): which guest functions are called, how often, with which arguments ----
extern "C" {
volatile int g_recomp_trace = 0;
}
namespace {
std::mutex g_trace_mutex;
struct TraceInfo {
    uint64_t calls = 0;
    uint32_t last_args[6] = {};
    uint32_t last_ecx = 0, last_ret = 0, last_eax = 0, last_edx = 0, last_esi = 0, last_ebx = 0;
    std::map<uint32_t, uint64_t> rets;  // return address -> calls
    bool log = false;                   // every call goes to fnlog.txt
    bool lua = false;                   // a Lua C function: record its arguments
    std::vector<uint32_t> watch;        // guest addresses whose dwords are recorded at each call
    std::string watched;
    std::string lua_args;
};
std::map<uint32_t, TraceInfo> g_traced;
}  // namespace
void native::TraceAdd(uint32_t va, bool lua) {
    std::lock_guard<std::mutex> l(g_trace_mutex);
    g_traced[va].lua = lua;
    g_recomp_trace = 1;
}
void native::TraceWatch(uint32_t va, uint32_t addr) {
    std::lock_guard<std::mutex> l(g_trace_mutex);
    g_traced[va].watch.push_back(addr);
    g_recomp_trace = 1;
}
void native::TraceLog(uint32_t va) {
    std::lock_guard<std::mutex> l(g_trace_mutex);
    g_traced[va].log = true;
    g_recomp_trace = 1;
}
void native::TraceClear() {
    std::lock_guard<std::mutex> l(g_trace_mutex);
    g_traced.clear();
    g_recomp_trace = 0;
}
std::string native::TraceDump(bool reset) {
    std::lock_guard<std::mutex> l(g_trace_mutex);
    std::string out;
    char b[200];
    for (auto& [va, t] : g_traced) {
        snprintf(b, sizeof b, "f_%08x calls %llu eax=%x ecx=%x edx=%x esi=%x ebx=%x ret=%x args %x %x %x %x\n", va, (unsigned long long)t.calls, t.last_eax,
                 t.last_ecx, t.last_edx, t.last_esi, t.last_ebx, t.last_ret, t.last_args[0], t.last_args[1], t.last_args[2], t.last_args[3]);
        out += b;
        if (!t.lua_args.empty()) out += "    lua args: " + t.lua_args + "\n";
        if (!t.watched.empty()) out += "    watched: " + t.watched + "\n";
        for (auto& [ra, n] : t.rets) {
            snprintf(b, sizeof b, "    from %08x: %llu\n", ra, (unsigned long long)n);
            out += b;
        }
        if (reset) {
            t.calls = 0;
            t.rets.clear();
        }
    }
    return out;
}
// Heap integrity check (debug): walks region 5's used and free lists and reports overlapping blocks.
extern "C" void RecompTrace(Ctx* c, uint32_t va) {
    std::lock_guard<std::mutex> l(g_trace_mutex);
    auto it = g_traced.find(va);
    if (it == g_traced.end()) return;
    TraceInfo& t = it->second;
    ++t.calls;
    if (t.log) {
        static FILE* f = fopen("fnlog.txt", "w");
        static uint64_t seq = 0;
        fprintf(f, "%llu t%d f_%x esp=%x ebp=%x eax=%x ecx=%x edx=%x esi=%x edi=%x ret=%x a=%x,%x,%x,%x\n", (unsigned long long)seq++, int(gettid()), va, c->esp, c->ebp, c->eax,
                c->ecx, c->edx, c->esi, c->edi, R32(c->esp), R32(c->esp + 4), R32(c->esp + 8), R32(c->esp + 12), R32(c->esp + 16));
        fflush(f);
    }
    t.last_ecx = c->ecx;
    t.last_eax = c->eax;
    t.last_edx = c->edx;
    t.last_esi = c->esi;
    t.last_ebx = c->ebx;
    t.last_ret = R32(c->esp);
    if (t.rets.size() < 16 || t.rets.count(t.last_ret)) ++t.rets[t.last_ret];
    if (t.lua) t.lua_args = native::DebugLuaArgs();
    if (!t.watch.empty()) {
        t.watched.clear();
        for (uint32_t a : t.watch) {
            char b[32];
            snprintf(b, sizeof b, "[%x]=%x ", a, R32(a));
            t.watched += b;
        }
    }
    for (int i = 0; i < 6; ++i) t.last_args[i] = R32(c->esp + 4 + 4 * uint32_t(i));
}

extern "C" RecompFn RecompLookup(uint32_t target) {
    // main builds the table before any guest thread exists (RecompLookup(0)), so the hot path is a plain pointer test:
    // call_once is an acquire load on every indirect call (~2% of the render thread).
    if (__builtin_expect(!g_hash, 0)) {
        static std::once_flag once;
        std::call_once(once, BuildHash);
    }
    // The table is 4 MiB (a cache miss per probe on a phone): recent targets first, from a 32 KiB table of
    // (va << 32 | code offset of the function from RecompLookup) words, one relaxed 64-bit load/store between threads.
    static uint64_t hot[4096];
    const uintptr_t base = reinterpret_cast<uintptr_t>(&RecompLookup);
    const uint32_t hi = (target * 2654435761u) >> 20;
    const uint64_t v = __atomic_load_n(&hot[hi], __ATOMIC_RELAXED);
    if (__builtin_expect(uint32_t(v >> 32) == target && target, 1))
        return reinterpret_cast<RecompFn>(base + intptr_t(int32_t(uint32_t(v))));
    for (uint32_t h = HashOf(target);; h = (h + 1) & ((1u << kHashBits) - 1)) {
        if (g_hash[h].va == target) {
            const intptr_t off = reinterpret_cast<intptr_t>(g_hash[h].fn) - intptr_t(base);
            if (off == intptr_t(int32_t(off)))
                __atomic_store_n(&hot[hi], (uint64_t(target) << 32) | uint32_t(int32_t(off)), __ATOMIC_RELAXED);
            return g_hash[h].fn;
        }
        if (!g_hash[h].va) return nullptr;
    }
}

// Thunks are not recompiled (Ghidra marks them as such): `jmp dword ptr [iat]` and `jmp rel32` are followed here.
static uint32_t FollowThunks(uint32_t target) {
    for (int i = 0; i < 8; ++i) {
        if ((target < g_engine_lo || target >= g_engine_hi) && (target < kPhysxBase || target >= kPhysxBase + kExtraReserve)) break;
        if (R8(target) == 0xff && R8(target + 1) == 0x25) {
            target = R32(R32(target + 2));
        } else if (R8(target) == 0xe9) {
            target = target + 5 + R32(target + 1);
        } else {
            break;
        }
    }
    return target;
}


extern "C" void RecompDispatchSlowIC(Ctx* ctx, uint32_t target, uint64_t* ic) {
    if (!native::InImportArea(target)) {
        if (RecompFn fn = RecompLookup(target)) {
            const intptr_t off = reinterpret_cast<intptr_t>(fn) - intptr_t(reinterpret_cast<uintptr_t>(&RecompLookup));
            if (target && off == intptr_t(int32_t(off)))
                __atomic_store_n(ic, (uint64_t(target) << 32) | uint32_t(int32_t(off)), __ATOMIC_RELAXED);
            fn(ctx);
            return;
        }
    }
    RecompDispatch(ctx, target);
}

extern "C" void RecompDispatch(Ctx* ctx, uint32_t target) {
    // Import calls first (D3D alone makes 20 000-30 000 a frame): straight to the handler, no hash probes.
    if (native::InImportArea(target)) {
        Ctx* saved = tl_ctx;
        tl_ctx = ctx;
        native::DispatchImport(ctx, target);
        tl_ctx = saved;
        return;
    }
    if (RecompFn fn = RecompLookup(target)) {
        fn(ctx);
        return;
    }
    uint32_t real = FollowThunks(target);
    if (real != target) {
        if (RecompFn fn = RecompLookup(real)) {
            fn(ctx);
            return;
        }
        target = real;
    }
    Ctx* saved = tl_ctx;
    tl_ctx = ctx;
    if (!DispatchImport(ctx, target)) {
        // Usually a call through a vtable: show the registers and what the object and its vtable look like.
        Log("bad call to %08x: eax %08x ecx %08x edx %08x ebx %08x esi %08x edi %08x esp %08x", target, ctx->eax, ctx->ecx,
            ctx->edx, ctx->ebx, ctx->esi, ctx->edi, ctx->esp);
        for (uint32_t r : {ctx->eax, ctx->ecx, ctx->edx, ctx->esi}) {
            if (r < 0x400000 || r >= 0xfff00000u) continue;  // low values are not pointers and may be unmapped
            Log("  [%08x]: %08x %08x %08x %08x %08x %08x", r, R32(r), R32(r + 4), R32(r + 8), R32(r + 12), R32(r + 16),
                R32(r + 20));
        }
        {  // return addresses on the guest stack that point into the images
            for (uint32_t i = 0; i < 48; ++i) {
                uint32_t v = R32(ctx->esp + 4 * i);
                if ((v >= g_engine_lo && v < g_engine_hi) || (v >= kPhysxBase && v < kPhysxBase + kExtraReserve))
                    Log("  stack[%u] = %08x", i, v);
            }
        }
        for (uint32_t r : {ctx->edi, ctx->ebx}) {
            if (r < 0x400000 || r >= 0xfff00000u) continue;  // low values are not pointers and may be unmapped
            for (uint32_t o = 0; o < 0x100; o += 16)
                Log("  edi/ebx %08x: %08x %08x %08x %08x", r + o, R32(r + o), R32(r + o + 4), R32(r + o + 8), R32(r + o + 12));
            break;
        }
        Fatal("call to %08x: neither recompiled nor an import (return address %08x)", target, R32(ctx->esp));
    }
    tl_ctx = saved;
}

// Spinlocks for locked operations on misaligned addresses (runtime.h RecompLockAddr), shared by all generated code.
extern "C" uint8_t RecompMisalignedLocks[256] = {};

extern "C" void RecompCpuid(Ctx* ctx) {
    // A plain SSE2 Pentium 4-class CPU: the engine only probes for SSE/SSE2 and the core count.
    switch (ctx->eax) {
        case 0:
            ctx->eax = 1;
            ctx->ebx = 0x756e6547;  // "GenuineIntel"
            ctx->edx = 0x49656e69;
            ctx->ecx = 0x6c65746e;
            break;
        case 1:
            ctx->eax = 0x00000f29;
            ctx->ebx = 0x00010800;
            ctx->ecx = 0x00000001;   // SSE3
            ctx->edx = 0x078bfbffu;  // FPU..SSE2, HTT clear
            break;
        default:
            ctx->eax = ctx->ebx = ctx->ecx = ctx->edx = 0;
    }
}

extern "C" void RecompUnimplemented(Ctx* ctx, uint32_t va, const char* what) {
    Fatal("trap at %08x (%s), esp=%08x", va, what, ctx->esp);
}


// ---- setjmp/longjmp (same scheme as the in-process bridge: slots keyed by the guest jmp_buf) ----
namespace {

constexpr int kJmpSlots = 64;
struct JmpSlots {
    RecompJmpSlot slots[kJmpSlots];
    int next;
};
thread_local JmpSlots* tl_jmp = nullptr;

RecompJmpSlot* FindSlot(uint32_t guest_buf, bool create) {
    if (!tl_jmp) tl_jmp = static_cast<JmpSlots*>(calloc(1, sizeof(JmpSlots)));
    for (auto& s : tl_jmp->slots) {
        if (s.guest_buf == guest_buf) return &s;
    }
    if (!create) return nullptr;
    RecompJmpSlot* s = &tl_jmp->slots[tl_jmp->next];
    tl_jmp->next = (tl_jmp->next + 1) % kJmpSlots;
    s->guest_buf = guest_buf;
    return s;
}

}  // namespace

extern "C" jmp_buf* RecompSetjmpPrepare(Ctx* ctx, uint32_t guest_buf) {
    RecompJmpSlot* s = FindSlot(guest_buf, true);
    s->esp = ctx->esp;
    s->ebp = ctx->ebp;
    s->ebx = ctx->ebx;
    s->esi = ctx->esi;
    s->edi = ctx->edi;
    s->st_top = ctx->st_top;
    s->fpu_cw = ctx->fpu_cw;
    s->value = 0;
    return &s->host;
}

extern "C" void RecompSetjmpResume(Ctx* ctx, uint32_t guest_buf) {
    RecompJmpSlot* s = FindSlot(guest_buf, false);
    ctx->esp = s->esp;
    ctx->ebp = s->ebp;
    ctx->ebx = s->ebx;
    ctx->esi = s->esi;
    ctx->edi = s->edi;
    ctx->st_top = s->st_top;
    ctx->fpu_cw = s->fpu_cw;
    ctx->eax = s->value;
}

extern "C" void RecompLongjmp(Ctx* ctx, uint32_t guest_buf, uint32_t value) {
    RecompJmpSlot* s = FindSlot(guest_buf, false);
    if (!s) RecompUnimplemented(ctx, guest_buf, "longjmp to an unknown jmp_buf");
    s->value = value ? value : 1;
    longjmp(s->host, 1);
}

namespace native {

uint32_t GuestCall(uint32_t va, std::initializer_list<uint32_t> args, uint32_t ecx, uint32_t* edx_out) {
    RecompFn fn = RecompLookup(va);
    if (!fn) Fatal("GuestCall: %08x is not a recompiled function", va);
    GuestThread* t = CurrentThread();
    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.esp = (tl_ctx ? tl_ctx->esp - 256 : t->stack_hi - 64) & ~15u;
    ctx.fs_base = t->teb;
    ctx.stack_lo = t->stack_lo;  // B* accessors: [ebp + k] on this stack skips TSO ordering
    ctx.stack_hi = t->stack_hi;
    ctx.fpu_cw = 0x027f;
    ctx.ecx = ecx;
    const uint32_t* a = args.end();
    while (a != args.begin()) Push32(&ctx, *--a);
    Push32(&ctx, 0xfffffff0u);  // return address; never jumped to
    Ctx* saved = tl_ctx;
    tl_ctx = nullptr;
    fn(&ctx);
    tl_ctx = saved;
    if (edx_out) *edx_out = ctx.edx;
    return ctx.eax;
}

}  // namespace native

// Debug: MAFIA2RE_SELFTEST=1 runs a few of the exe's own CRT float routines with known inputs (called once the CRT is up).
namespace native {
// GuestCall from a recompiled function's hook: the guest stack below c->esp is free, whatever tl_ctx says.
uint32_t GuestCallFrom(Ctx* c, uint32_t va, std::initializer_list<uint32_t> args, uint32_t ecx) {
    Ctx* saved = tl_ctx;
    tl_ctx = c;
    uint32_t r = GuestCall(va, args, ecx);
    tl_ctx = saved;
    return r;
}
static double GuestCallF(uint32_t va, std::initializer_list<uint32_t> args) {
    RecompFn fn = RecompLookup(va);
    if (!fn) return -12345.0;
    GuestThread* t = CurrentThread();
    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.esp = (tl_ctx ? tl_ctx->esp - 256 : t->stack_hi - 64) & ~15u;
    ctx.fs_base = t->teb;
    ctx.stack_lo = t->stack_lo;
    ctx.stack_hi = t->stack_hi;
    ctx.fpu_cw = 0x027f;
    const uint32_t* a = args.end();
    while (a != args.begin()) Push32(&ctx, *--a);
    Push32(&ctx, 0xfffffff0u);
    Ctx* saved = tl_ctx;
    tl_ctx = nullptr;
    fn(&ctx);
    tl_ctx = saved;
    return ctx.st[(ctx.st_top) & 7];
}
void RunSelfTest() {
    static bool done = false;
    if (done || !getenv("MAFIA2RE_SELFTEST")) return;
    done = true;
    const char* tests[] = {"0.707107", "1.4", "-0.5", "123.456", "1e-5", "100", "0.0001", "3.14159265358979", "-42.5e3", "0"};
    for (const char* t : tests) {
        uint32_t p = GuestStrdup(t);
        double r = GuestCallF(0x00e756ee, {p});  // _atof
        Log("selftest _atof(\"%s\") = %.9g", t, r);
    }
}
}  // namespace native

#ifdef RECOMP_NAN_TRIP
namespace native { extern std::atomic<uint64_t> g_frames; }
extern "C" __attribute__((noinline)) void RecompNanHit(uint32_t addr) { asm volatile("" ::: "memory"); }  // gdb: break RecompNanHit
extern "C" void RecompNanTrip(uint32_t addr, uint64_t bits) {
    static const uint64_t after = getenv("MAFIA2RE_NAN_AFTER") ? strtoull(getenv("MAFIA2RE_NAN_AFTER"), nullptr, 10) : 0;
    if (native::g_frames.load() < after) return;
    static std::atomic<int> n{0};
    int k = n++;
    if (k < 20) native::Log("NANSTORE #%d addr=%08x bits=%llx", k, addr, (unsigned long long)bits);
    RecompNanHit(addr);
}
#endif
