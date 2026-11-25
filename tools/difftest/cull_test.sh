#!/bin/sh
# Builds and runs cull_test.c against the original code from the user's mafia2.exe. Output in build/difftest.
set -e
cd "$(dirname "$0")/../.."
O=build/difftest; mkdir -p $O
python3 - > $O/cull_orig.h <<'P'
import pefile, os
pe = pefile.PE(os.path.expanduser('~/mafia2-re-data/game/pc/mafia2.exe'), fast_load=True)
get = lambda va, n: pe.get_data(va - 0x400000, n)
code = get(0x1652180, 0x165229c - 0x1652180)
print("static const uint32_t kCodeVa = 0x1652180u;")
print("static const unsigned char kCode[] = {" + ",".join(str(b) for b in code) + "};")
code2 = get(0x16522a0, 0x165235d - 0x16522a0)
print("static const uint32_t kCode2Va = 0x16522a0u;")
print("static const unsigned char kCode2[] = {" + ",".join(str(b) for b in code2) + "};")
print("static const struct { uint32_t va; unsigned char bytes[16]; } kConsts[] = {")
for va in (0x1a95e80, 0x1a95e90, 0x1a95eb0, 0x1a95ec0, 0x18f0e78):
    print("  {0x%xu, {%s}}," % (va, ",".join(str(b) for b in get(va, 16))))
print("};")
P
cp tools/difftest/cull_test.c $O/
gcc -m32 -O2 -msse2 -mfpmath=sse -ffp-contract=off -o $O/cull_test $O/cull_test.c
$O/cull_test "$@"
