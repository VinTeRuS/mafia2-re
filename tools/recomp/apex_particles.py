# Recompiler settings for APEX_particles.dll (APEX 1.x from the game folder), rebased to 0x10900000 (--base).
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_APEX_PARTICLES", "~/mafia2-re-data/physx/apex_particles.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "apex_particles", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "apex_particles", "extra_functions.txt")
TABLE_SUFFIX = "_apex_particles"
INLINE_HELPERS = {0x10021bb0: "aullshr", 0x1002a8f0: "allmul", 0x100339d0: "chkstk16", 0x100339e6: "chkstk8", 0x10035760: "chkstk"}
JUMP_TABLES = {}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = {0x10015d40, 0x10016340}  # memcpy/memmove (Native_<va>, src/native/m2_imports.cpp)
TRACE_HOOKS = True
