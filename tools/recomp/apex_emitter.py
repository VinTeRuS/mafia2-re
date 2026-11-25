# Recompiler settings for APEX_emitter.dll (APEX 1.x from the game folder), rebased to 0x10700000 (--base).
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_APEX_EMITTER", "~/mafia2-re-data/physx/apex_emitter.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "apex_emitter", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "apex_emitter", "extra_functions.txt")
TABLE_SUFFIX = "_apex_emitter"
INLINE_HELPERS = {0x1003ad70: "aullshr", 0x10040fa0: "allmul", 0x1004ce40: "chkstk16", 0x1004ce56: "chkstk8", 0x1004ed30: "chkstk"}
JUMP_TABLES = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x1002e090, 0x1003a6e0}  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
TRACE_HOOKS = True
