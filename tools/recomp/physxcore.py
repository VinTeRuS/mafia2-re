# Recompiler settings for PhysXCore.dll 2.8.3.37 (NVIDIA PhysX System Software 9.10.0513, from the game's Redist msi),
# loaded at its preferred base 0x10000000. Static MSVC CRT; imports kernel32/wsock32/cudart32/user32 only.
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_PHYSXCORE", "~/mafia2-re-data/physx/PhysXCore_283.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "physxcore", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "physxcore", "extra_functions.txt")
TABLE_SUFFIX = "_physxcore"
INLINE_HELPERS = {0x101e2360: "aullshr", 0x101e2380: "allshl", 0x101e23a0: "chkstk16", 0x101e23b6: "chkstk8",
                  0x101e2860: "chkstk", 0x101e5050: "allmul"}
JUMP_TABLES = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x101e19c0, 0x101e1ea0}  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
TRACE_HOOKS = True

# NxScene::createCloth(const NxClothDesc&): the game hands it a desc whose globalPose is NaN (see src/native/m2_imports.cpp)
PRE_HOOKS = {0x100e9740: "Hook_PxCreateCloth"}
