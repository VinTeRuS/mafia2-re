#!/usr/bin/env python3
"""apply_patches.py: the exe with hook.dll's load() patches baked in (what DumpHookPatches wrote to patches.txt:
SWBF2RE_DUMP_PATCHES=<dir> in a run), written to ~/swbf2-re-data/game_patched/BattlefrontII.exe. The recompiler reads that
image: hook.dll rewrites immediates in the exe's code at run time (heap base/size, buffer bounds), which recompiled code cannot see.
Only code/rdata bytes; the IAT range is the loader's own work. Local use only (derived from the game binary)."""
import os, sys, pefile
src = os.path.expanduser("~/swbf2-re-data/game/BattlefrontII.exe")
patches = os.path.expanduser("~/swbf2-re-data/patches/patches.txt")
dst = os.path.expanduser("~/swbf2-re-data/game_patched/BattlefrontII.exe")
pe = pefile.PE(src)
iat = pe.OPTIONAL_HEADER.DATA_DIRECTORY[12]
iat_lo, iat_hi = pe.OPTIONAL_HEADER.ImageBase + iat.VirtualAddress, pe.OPTIONAL_HEADER.ImageBase + iat.VirtualAddress + iat.Size
data = bytearray(open(src, "rb").read())
n = 0
extra = os.path.join(os.path.dirname(os.path.abspath(__file__)), "exe_patches.txt")
for line in list(open(patches)) + list(open(extra)):
    p = line.split()
    if not p or p[0] != "patch":
        continue
    a, cnt = int(p[1], 16), int(p[2])
    if iat_lo <= a < iat_hi:
        continue
    new = bytes(int(x, 16) for x in p[3:3 + cnt])
    off = pe.get_offset_from_rva(a - pe.OPTIONAL_HEADER.ImageBase)
    data[off:off + cnt] = new
    n += 1
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, "wb").write(data)
print(f"{n} patches applied -> {dst}")
