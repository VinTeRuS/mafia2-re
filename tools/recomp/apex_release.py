# Recompiler settings for APEX_release.dll (APEX 1.x from the game folder), rebased to 0x10400000 (--base).
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_APEX_RELEASE", "~/mafia2-re-data/physx/apex_release.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "apex_release", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "apex_release", "extra_functions.txt")
TABLE_SUFFIX = "_apex_release"
INLINE_HELPERS = {0x100440e0: "chkstk16", 0x100440f6: "chkstk8", 0x1004af20: "chkstk", 0x1004af50: "allmul", 0x10067710: "aullshr"}
JUMP_TABLES = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x1002ee00, 0x10033b30}  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
TRACE_HOOKS = True
