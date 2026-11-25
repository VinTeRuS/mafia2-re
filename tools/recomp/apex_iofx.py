# Recompiler settings for APEX_iofx.dll (APEX 1.x from the game folder), rebased to 0x10800000 (--base).
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_APEX_IOFX", "~/mafia2-re-data/physx/apex_iofx.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "apex_iofx", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "apex_iofx", "extra_functions.txt")
TABLE_SUFFIX = "_apex_iofx"
INLINE_HELPERS = {0x10036650: "aullshr", 0x1003cd60: "allmul", 0x10048b30: "chkstk16", 0x10048b46: "chkstk8", 0x1004a8e0: "chkstk"}
JUMP_TABLES = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x10029710, 0x10035fc0}  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
TRACE_HOOKS = True
