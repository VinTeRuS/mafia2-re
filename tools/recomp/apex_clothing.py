# Recompiler settings for APEX_clothing.dll (APEX 1.x from the game folder), rebased to 0x10500000 (--base).
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_APEX_CLOTHING", "~/mafia2-re-data/physx/apex_clothing.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "apex_clothing", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "apex_clothing", "extra_functions.txt")
TABLE_SUFFIX = "_apex_clothing"
INLINE_HELPERS = {0x10041ae0: "allmul", 0x10042330: "aullshr", 0x100423c0: "chkstk16", 0x100423d6: "chkstk8", 0x1004ae40: "chkstk"}
JUMP_TABLES = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x10031e20, 0x10037180}  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
TRACE_HOOKS = True
