// Sampling CPU profiler for finding what is slow on a phone (setting cpu_profile = 1). ITIMER_PROF fires SIGPROF
// every millisecond of process CPU time on whichever thread is running; the handler records the interrupted program
// counter in a lock-free ring. Every 30 s a reporter thread logs the 200 most frequent addresses as offsets into
// libmain.so (64-byte buckets), with their share of the samples. The APK's library is stripped: the offsets are
// turned into function names on the desktop with the unstripped build (tools/prof_report.py), and summed per function.
// A sample outside libmain (libc's memcpy, the driver, DXVK) is charged to its caller inside libmain when the link
// register points there (marked "c" in the log), and every sample counts for its thread.
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

#include "guest.h"

namespace native {

namespace {

#if defined(__aarch64__)
constexpr uintptr_t kVia = 1;  // bit 0 of a recorded pc: charged through the caller (arm64 code is 4-byte aligned)
#else
constexpr uintptr_t kVia = 0;  // x86 pcs can be odd
#endif
constexpr uint32_t kRing = 1u << 16;
std::atomic<uint32_t> g_head{0};
uintptr_t g_ring[kRing];
uint32_t g_tid_ring[kRing];
uintptr_t g_end = 0;  // end of libmain's mapping
uintptr_t g_base = 0;

void OnProf(int, siginfo_t*, void* context) {
    auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    uintptr_t pc = uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = 0;
#endif
#if defined(__aarch64__)
    if (pc < g_base || pc >= g_end) {
        uintptr_t lr = uc->uc_mcontext.regs[30];
        if (lr >= g_base && lr < g_end) pc = lr | 1;  // bit 0: charged through the caller
    }
#endif
    uint32_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    g_ring[i & (kRing - 1)] = pc;
    g_tid_ring[i & (kRing - 1)] = uint32_t(gettid());
}

void Report() {
    uint32_t seen = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        uint32_t head = g_head.load(std::memory_order_relaxed);
        uint32_t n = std::min(head - seen, kRing);
        std::unordered_map<uintptr_t, uint32_t> hist;
        std::unordered_map<uint32_t, uint32_t> threads, threads_out;
        std::unordered_map<std::string, uint32_t> modules;  // samples outside libmain per shared object
        std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> thread_modules;
        std::unordered_map<uintptr_t, const char*> page_module;
        uint32_t outside = 0;
        // cpu_profile_busiest = 1: buckets from the busiest thread only (the game's frame thread)
        static const bool busiest = SettingInt("cpu_profile_busiest", 0) != 0;
        uint32_t only_tid = 0;
        if (busiest) {
            std::unordered_map<uint32_t, uint32_t> cnt;
            uint32_t best = 0;
            for (uint32_t k = head - n; k != head; ++k) {
                uint32_t t = g_tid_ring[k & (kRing - 1)];
                if (++cnt[t] > best) { best = cnt[t]; only_tid = t; }
            }
        }
        for (uint32_t k = head - n; k != head; ++k) {
            uintptr_t pc = g_ring[k & (kRing - 1)];
            uint32_t tid = g_tid_ring[k & (kRing - 1)];
            ++threads[tid];
            if (pc < g_base || pc >= g_end) {
                ++outside;
                ++threads_out[tid];
                auto [it, fresh] = page_module.try_emplace(pc >> 12, "?");
                Dl_info di;
                if (fresh && dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_fname) {
                    const char* slash = strrchr(di.dli_fname, '/');
                    it->second = slash ? slash + 1 : di.dli_fname;
                }
                ++modules[it->second];
                std::string key = it->second;
                if (strncmp(it->second, "libc.", 5) == 0 && dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_sname)
                    key = std::string("libc:") + di.dli_sname;  // memcpy, pthread_mutex_lock, ...
                ++thread_modules[tid][key];
                continue;
            }
            if (pc & kVia) {  // charged through the caller: the sample itself ran outside libmain
                ++outside;
                ++threads_out[tid];
                ++modules["(via caller)"];
                ++thread_modules[tid]["(via caller)"];
            }
            if (only_tid && tid != only_tid) continue;
            uintptr_t off = (pc & ~uintptr_t(1)) - g_base;
            ++hist[(pc & kVia) ? (off | 1) : (off & ~uintptr_t(63))];
        }
        {
            std::vector<std::pair<uint32_t, uint32_t>> t;
            for (auto& [tid, c] : threads) t.push_back({c, tid});
            std::sort(t.rbegin(), t.rend());
            std::string tl;
            for (size_t k = 0; k < t.size() && k < 10; ++k) tl += " " + std::to_string(t[k].second) + ":" + std::to_string(t[k].first);
            Log("profile threads:%s", tl.c_str());
            for (size_t k = 0; k < t.size() && k < 6; ++k) {  // name, samples, share outside libmain
                char path[64], name[32] = "?";
                snprintf(path, sizeof path, "/proc/self/task/%u/comm", t[k].second);
                if (FILE* f = fopen(path, "r")) {
                    if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
                    fclose(f);
                }
                Log("profile thread %u %s: %u samples, %u%% outside libmain", t[k].second, name, t[k].first,
                    unsigned(100.0 * threads_out[t[k].second] / t[k].first));
            }
            std::vector<std::pair<uint32_t, std::string>> m;
            for (auto& [mod, c] : modules) m.push_back({c, mod});
            std::sort(m.rbegin(), m.rend());
            std::string ml;
            for (size_t k = 0; k < m.size() && k < 8; ++k) ml += " " + m[k].second + ":" + std::to_string(m[k].first);
            Log("profile modules:%s", ml.c_str());
            if (!t.empty()) {  // the busiest thread (the game's render thread)
                std::vector<std::pair<uint32_t, std::string>> r;
                for (auto& [mod, c] : thread_modules[t[0].second]) r.push_back({c, mod});
                std::sort(r.rbegin(), r.rend());
                std::string rl;
                for (size_t k = 0; k < r.size() && k < 12; ++k) rl += " " + r[k].second + ":" + std::to_string(r[k].first);
                Log("profile modules of thread %u:%s", t[0].second, rl.c_str());
            }
        }
        seen = head;
        std::vector<std::pair<uint32_t, uintptr_t>> top;
        for (auto& [off, count] : hist) top.push_back({count, off});
        std::sort(top.rbegin(), top.rend());
        static const size_t keep = size_t(SettingInt("cpu_profile_top", 200));  // more for a full desktop profile
        if (top.size() > keep) top.resize(keep);
        Log("profile: %u samples (%u outside libmain: drivers, libc, DXVK), top buckets:", n, outside);
        std::string line;
        for (auto& [count, off] : top) {
            char item[48];
            snprintf(item, sizeof item, (off & 1) ? " c%lx:%u" : " %lx:%u", (unsigned long)(off & ~uintptr_t(1)), count);
            line += item;
            if (line.size() > 900) {
                Log("profile:%s", line.c_str());
                line.clear();
            }
        }
        if (!line.empty()) Log("profile:%s", line.c_str());
    }
}

}  // namespace

void StartProfiler() {
    if (!SettingInt("cpu_profile", 0)) return;
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&StartProfiler), &info) && info.dli_fbase)
        g_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    g_end = 0;  // the last mapping of the game module (libmain.so on Android, mafia2-native on the PC)
    if (FILE* f = fopen("/proc/self/maps", "r")) {  // the last mapping of libmain.so
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned long lo, hi;
            if ((strstr(line, "libmain") || strstr(line, "mafia2-native")) && sscanf(line, "%lx-%lx", &lo, &hi) == 2 && lo >= g_base) g_end = hi;
        }
        fclose(f);
    }
    struct sigaction sa {};
    sa.sa_sigaction = OnProf;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    itimerval t {};
    t.it_interval.tv_usec = 1000;
    t.it_value.tv_usec = 1000;
    setitimer(ITIMER_PROF, &t, nullptr);
    std::thread(Report).detach();
    Log("profiler: sampling every 1 ms of CPU time, module base %lx", (unsigned long)g_base);
}

}  // namespace native
