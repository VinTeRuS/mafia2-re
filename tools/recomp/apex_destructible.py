# Recompiler settings for APEX_destructible.dll (APEX 1.x from the game folder), rebased to 0x10600000 (--base).
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_APEX_DESTRUCTIBLE", "~/mafia2-re-data/physx/apex_destructible.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "apex_destructible", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "apex_destructible", "extra_functions.txt")
TABLE_SUFFIX = "_apex_destructible"
INLINE_HELPERS = {0x1004b600: "chkstk16", 0x1004b616: "chkstk8", 0x1004c540: "allshl", 0x10051b00: "chkstk", 0x10058d00: "aullshr", 0x1006a180: "allmul"}
JUMP_TABLES = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x1004aa10, 0x10058670}  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
TRACE_HOOKS = True
