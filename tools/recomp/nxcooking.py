# Recompiler settings for NxCooking.dll 2.8.3.37; preferred base 0x10000000 clashes with PhysXCore, so it is generated
# with --base 0x10300000 (relocated) and loaded there.
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_NXCOOKING", "~/mafia2-re-data/physx/NxCooking_283.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "nxcooking", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "nxcooking", "extra_functions.txt")
TABLE_SUFFIX = "_nxcooking"
INLINE_HELPERS = {0x10030a50: "chkstk16", 0x10030a66: "chkstk8", 0x10031720: "chkstk", 0x1003c500: "aullshr",
                  0x1003ef50: "allmul"}
JUMP_TABLES = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x10030350, 0x100306c0}  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
TRACE_HOOKS = True
