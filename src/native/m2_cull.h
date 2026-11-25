// Native version of mafia2.exe f_01652180 (~8% of the game's CPU time): an axis-aligned box against up to 32 planes.
//   uint32_t f(const float min[4], const float max[4], const float planes[][4], uint32_t mask, uint32_t* out)  (cdecl)
// The box becomes centre c = (min + max) / 2 (w = 1) and half extent e = max - c (w = 0). For each plane P, while
// mask has bits left (the loop runs to its highest set bit, every plane counts):
//   r = |P|.e, d = P.c;  out |= bit if 0 < r - d;  result &= (0 <= d + r);  bit doubles.
// Returns 1 when the box is on the inner side of all planes (else 0); *out gets the bits of the planes it crosses.
// Bit-exact with the SSE original, including the order of the horizontal sums ((x2 + x3) + (x0 + x1)), NaN compares
// (false) and the sign of zero; contraction into fused multiply-adds is off. Checked by tools/difftest/cull_test.c
// against the original machine code.
#pragma once
#include <stdint.h>
#include <string.h>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

static inline float M2Abs(float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    b &= 0x7fffffffu;
    memcpy(&v, &b, 4);
    return v;
}

static inline uint32_t M2CullBox(const float* mn, const float* mx, const float* planes, uint32_t mask, uint32_t* out) {
    float c[4], e[4];
    for (int i = 0; i < 3; ++i) {
        float h = (mn[i] + mx[i]) * 0.5f;  // addps, mulps, then + 0.0 (andps keeps x, addps adds the w-only 1.0)
        c[i] = h + 0.0f;
        e[i] = mx[i] - c[i];
    }
    c[3] = 1.0f;
    e[3] = 0.0f;
    uint32_t any = 0, all = 1, bit = 1;
    for (; mask; mask >>= 1, planes += 4) {
        float t0 = M2Abs(planes[0]) * e[0], t1 = M2Abs(planes[1]) * e[1], t2 = M2Abs(planes[2]) * e[2],
              t3 = M2Abs(planes[3]) * e[3];
        float u0 = planes[0] * c[0], u1 = planes[1] * c[1], u2 = planes[2] * c[2], u3 = planes[3] * c[3];
        float r = (t2 + t3) + (t0 + t1);
        float d = (u2 + u3) + (u0 + u1);
        if (0.0f < r - d) any |= bit;
        if (!(0.0f <= d + r)) all = 0;
        bit += bit;
    }
    *out = any;
    return all;
}

// Native version of f_016522a0: the same box against `count` planes (cdecl (min, max, planes, count)), no mask and no
// crossing bits: 1 when 0 <= d + r for every plane, else 0 (count <= 0: 1). Sums in the order of the SSE original
// ((u3 + u2) + (u1 + u0)) and ((t3 + t2) + (t1 + t0)), then d + r. Checked by tools/difftest/cull_test.c.
static inline uint32_t M2CullBoxAll(const float* mn, const float* mx, const float* planes, int32_t count) {
    float c[4], e[4];
    for (int i = 0; i < 3; ++i) {
        float h = (mn[i] + mx[i]) * 0.5f;
        c[i] = h + 0.0f;
        e[i] = mx[i] - c[i];
    }
    c[3] = 1.0f;
    e[3] = 0.0f;
    uint32_t all = 1;
    for (; count > 0; --count, planes += 4) {
        float t0 = M2Abs(planes[0]) * e[0], t1 = M2Abs(planes[1]) * e[1], t2 = M2Abs(planes[2]) * e[2],
              t3 = M2Abs(planes[3]) * e[3];
        float u0 = planes[0] * c[0], u1 = planes[1] * c[1], u2 = planes[2] * c[2], u3 = planes[3] * c[3];
        float r = (t3 + t2) + (t1 + t0);
        float d = (u3 + u2) + (u1 + u0);
        if (!(0.0f <= d + r)) all = 0;
    }
    return all;
}
