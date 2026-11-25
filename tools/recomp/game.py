# Recompiler settings for Mafia II (2010, GOG original mafia2.exe; sha256 in re/target.sha256), read by third_party/x86recomp.
# Static MSVC 2010 CRT, base 0x400000, no .reloc. Addresses from Ghidra's names.
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_EXE", "~/mafia2-re-data/game/pc/mafia2.exe")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "extra_functions.txt")
SETJMP_TARGETS = {0x00e797a0}        # __setjmp3
LONGJMP_TARGETS = {0x00e796ec}       # ___longjmp_internal (to be verified)
SETJMP_IAT = set()
LONGJMP_IAT = set()
INLINE_HELPERS = {0x00e731c0: "allmul", 0x00e760e0: "allshl", 0x00e79a10: "aullshr",
                  0x00e749d0: "chkstk", 0x00e75b20: "chkstk16", 0x00e75b36: "chkstk8", 0x00e74a00: "ftol2"}
INLINE_IMPORTS = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x00e73200, 0x00e75b50, 0x00790817,  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
                    0x01652180, 0x016522a0}  # box vs frustum planes (m2_cull.h, difftest tools/difftest/cull_test.sh)
TRACE_HOOKS = True
PRE_HOOKS = {0x5d3f80: "Hook_LuaPcall", 0x5a0e70: "Hook_GameTick", 0x58c070: "Hook_GameTick",  # host-side Lua (m2_script.cpp)
             # the video options' setters (profile load and the options menu): forced values from mafia2re.ini (m2_imports.cpp)
             0xb500a0: "Hook_SetAmbientOcclusion", 0xb500d0: "Hook_SetShadowQuality", 0xb50100: "Hook_SetGeometryDetail",
             0x173db90: "Hook_RendererSetup"}  # one GPU: the game's shadow map cache stays on (m2_imports.cpp)
# Host versions that can be switched off at run time (the recompiled original stays): m2_octree.h
GATE_HOOKS = {0x66feb0: "Gate_OctreeQuery"}
