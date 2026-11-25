// Difftest for src/native/m2_cull.h: runs the original x86 machine code of mafia2.exe f_01652180 (copied from the
// user's exe at test time by cull_test.sh, never stored in the repo) and M2CullBox on the same random inputs.
// Build: tools/difftest/cull_test.sh (gcc -m32, SSE2).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../../src/native/m2_cull.h"
#include "cull_orig.h"  // generated: kCode/kCodeVa, kConst*

typedef uint32_t (*OrigFn)(const float*, const float*, const float*, uint32_t, uint32_t*);
typedef uint32_t (*OrigAllFn)(const float*, const float*, const float*, int32_t);

static uint32_t rng = 12345;
static uint32_t Rand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float RandFloat(void) {
    uint32_t k = Rand() % 100, b;
    float v;
    if (k < 2) { b = 0x7fc00000u | (Rand() & 0x803fffffu); memcpy(&v, &b, 4); return v; }  // NaN
    if (k < 4) return (Rand() & 1) ? 1.0f / 0.0f : -1.0f / 0.0f;
    if (k < 6) return (Rand() & 1) ? 0.0f : -0.0f;
    if (k < 8) { b = Rand() & 0x807fffffu; memcpy(&v, &b, 4); return v; }  // denormal
    if (k < 60) return ((float)(int32_t)Rand() / 2147483648.0f) * 100.0f;
    b = Rand(); memcpy(&v, &b, 4); return v;  // any bits
}
static void* MapAt(uint32_t va, uint32_t size) {
    uint32_t page = va & ~0xfffu;
    void* p = mmap((void*)(uintptr_t)page, (va + size - page + 0xfff) & ~0xfffu, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}
int main(int argc, char** argv) {
    long n = argc > 1 ? atol(argv[1]) : 2000000;
    MapAt(kCodeVa, sizeof kCode); memcpy((void*)(uintptr_t)kCodeVa, kCode, sizeof kCode);
    if ((kCode2Va & ~0xfffu) != (kCodeVa & ~0xfffu)) MapAt(kCode2Va, sizeof kCode2);
    memcpy((void*)(uintptr_t)kCode2Va, kCode2, sizeof kCode2);
    for (unsigned i = 0; i < sizeof kConsts / sizeof kConsts[0]; ++i) {
        static uint32_t mapped[8]; int done = 0;
        for (int j = 0; j < 8; ++j) if (mapped[j] == (kConsts[i].va & ~0xfffu)) done = 1;
        if (!done) { MapAt(kConsts[i].va, 16); for (int j = 0; j < 8; ++j) if (!mapped[j]) { mapped[j] = kConsts[i].va & ~0xfffu; break; } }
        memcpy((void*)(uintptr_t)kConsts[i].va, kConsts[i].bytes, 16);
    }
    OrigFn orig = (OrigFn)(uintptr_t)kCodeVa;
    static float mn[4] __attribute__((aligned(16))), mx[4] __attribute__((aligned(16))), pl[32 * 4] __attribute__((aligned(16)));
    long bad = 0;
    for (long it = 0; it < n; ++it) {
        for (int i = 0; i < 4; ++i) { mn[i] = RandFloat(); mx[i] = (Rand() & 3) ? mn[i] + M2Abs(RandFloat()) : RandFloat(); }
        for (int i = 0; i < 32 * 4; ++i) pl[i] = RandFloat();
        if (Rand() & 1) {  // planes tuned so the box touches them (r - d and d + r within a few ulps of 0): sum order matters
            float c[3], e[3];
            for (int i = 0; i < 3; ++i) { c[i] = (mn[i] + mx[i]) * 0.5f; e[i] = mx[i] - c[i]; }
            for (int p = 0; p < 32; ++p) {
                float* P = pl + 4 * p;
                for (int i = 0; i < 3; ++i) P[i] = ((float)(int32_t)Rand() / 2147483648.0f) * 10.0f;
                double r = 0, d = 0;
                for (int i = 0; i < 3; ++i) { r += (double)M2Abs(P[i]) * e[i]; d += (double)P[i] * c[i]; }
                float w = (float)(((Rand() & 1) ? r : -r) - d);
                uint32_t b; memcpy(&b, &w, 4); b += (int32_t)(Rand() % 9) - 4; memcpy(&w, &b, 4);
                P[3] = w;
            }
        }
        {  // f_016522a0: all planes, by count
            int32_t count = (Rand() & 7) == 0 ? (int32_t)(Rand() % 5) - 2 : (int32_t)(Rand() % 33);
            uint32_t a1 = ((OrigAllFn)(uintptr_t)kCode2Va)(mn, mx, pl, count), a2 = M2CullBoxAll(mn, mx, pl, count);
            if (a1 != a2 && ++bad <= 5) printf("MISMATCH (all) it %ld count %d: orig %u native %u\n", it, count, a1, a2);
        }
        uint32_t mask = (Rand() & 3) == 0 ? Rand() : (Rand() & 0x3f), o1 = 0xdead, o2 = 0xbeef;
        uint32_t r1 = orig(mn, mx, pl, mask, &o1), r2 = M2CullBox(mn, mx, pl, mask, &o2);
        if (r1 != r2 || o1 != o2) {
            if (++bad <= 5) printf("MISMATCH it %ld mask %08x: orig %u/%08x native %u/%08x\n", it, mask, r1, o1, r2, o2);
        }
    }
    printf("%ld cases, %ld mismatches\n", n, bad);
    return bad != 0;
}
