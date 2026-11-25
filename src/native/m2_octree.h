// Native version of mafia2.exe f_0066feb0: the objects of a hashed sparse octree whose boxes overlap a query box.
//   thiscall bool tree->Query(uint8_t level, Node* root, const Box* q, Sink* sink, Out* out)   (ret 0x14)
// Tree: +0x44 = per-level hash tables (index level - 1): +0x18 buckets, +0x24 bucket mask.
// Node: +0/+4/+8 int cell x, y, z; +0x0c min xyz, +0x18 max xyz (floats); +0x24 first object; +0x28 next in its
//       bucket; +0x30 (low byte) mask of the 8 children. Child k sits at cell (2x + (k&1), 2y + (k>>1&1), 2z + (k>>2)),
//       found by hash z*0x4f9ffb7 + x*0x466f45d + y*0x127409f (f_00669e40; missing children are pushed as 0).
// Object: +0 min xyz, +0x0c max xyz, +0x18 next. Query box: min at +0, max at +0x10.
// Out: +0x200 array, +0x204 capacity, +0x208 count. A full array goes to sink->vtable[1](array, count) (thiscall,
//      returns bool; false ends the query with false), then the count restarts at 0.
// Walk: an explicit stack of 0x80 entries (a child that does not fit is dropped), last pushed first; a node whose
// box misses is skipped with its children; children are pushed in bit order with level - 1, level 0 has none.
// Overlap: no axis with max < qmin or qmax < min (ordered compares: NaN counts as overlapping), as the SSE original.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>

#include "guest.h"

namespace m2oct {

inline float F(uint32_t a) { uint32_t v = R32(a); float f; memcpy(&f, &v, 4); return f; }

struct Query {
    float qmin[3], qmax[3];
    bool Overlaps(uint32_t box) const {  // box: min xyz, max xyz
        for (int i = 0; i < 3; ++i) {
            if (F(box + 12 + 4 * i) < qmin[i]) return false;
            if (qmax[i] < F(box + 4 * i)) return false;
        }
        return true;
    }
};

// emit(object) returns false to end the walk (sink refused). Returns the function's bool result.
template <class Emit> inline bool Walk(uint32_t tree, uint8_t level0, uint32_t root, uint32_t qbox, Emit&& emit) {
    Query q;
    for (int i = 0; i < 3; ++i) { q.qmin[i] = F(qbox + 4 * i); q.qmax[i] = F(qbox + 16 + 4 * i); }
    struct Entry { uint32_t node; uint8_t level; };
    Entry st[0x80];
    uint32_t n = 1;
    st[0] = {root, level0};
    const uint32_t levels = R32(tree + 0x44);
    while (n) {
        const Entry e = st[--n];
        const uint32_t node = e.node;
        if (!q.Overlaps(node + 0x0c)) continue;
        for (uint32_t o = R32(node + 0x24); o; o = R32(o + 0x18))
            if (q.Overlaps(o) && !emit(o)) return false;
        if (!e.level) continue;
        const uint32_t table = R32(levels + 4u * e.level - 4);
        const uint32_t mask = R32(table + 0x24), buckets = R32(table + 0x18);
        const uint32_t children = R16(node + 0x30) & 0xff;
        const uint32_t x = R32(node), y = R32(node + 4), z = R32(node + 8);
        uint32_t kids[8], k = 0;
        for (uint32_t b = 0; b < 8; ++b) {
            if (!(children >> b & 1)) continue;
            const uint32_t cx = 2 * x + (b & 1), cy = 2 * y + (b >> 1 & 1), cz = 2 * z + (b >> 2);
            uint32_t p = R32(buckets + 4 * ((cz * 0x4f9ffb7u + cx * 0x466f45du + cy * 0x127409fu) & mask));
            while (p && !(R32(p) == cx && R32(p + 4) == cy && R32(p + 8) == cz)) p = R32(p + 0x28);
            kids[k++] = p;
        }
        for (uint32_t i = 0; i < k; ++i)
            if (n < 0x80) st[n++] = {kids[i], uint8_t(e.level - 1)};
    }
    return true;
}

}  // namespace m2oct
