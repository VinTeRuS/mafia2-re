#!/usr/bin/env python3
"""Writes tools/recomp/apex_<name>.py for the six APEX DLLs (copied to ~/mafia2-re-data/physx/apex_<name>.dll):
INLINE_HELPERS come from tools/find_helpers.py run against each image."""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASES = {"release": 0x10400000, "clothing": 0x10500000, "destructible": 0x10600000, "emitter": 0x10700000,
         "iofx": 0x10800000, "particles": 0x10900000}
for name, base in BASES.items():
    dll = os.path.expanduser(f"~/mafia2-re-data/physx/apex_{name}.dll")
    out = subprocess.run([sys.executable, "-I", f"{ROOT}/third_party/x86recomp/tools/find_helpers.py",
                          f"{ROOT}/tools/recomp/game.py", dll], capture_output=True, text=True).stdout
    m = re.search(r"^INLINE_HELPERS = (\{.*\})", out, re.M)
    helpers = m.group(1) if m else "{}"
    open(f"{ROOT}/tools/recomp/apex_{name}.py", "w").write(f'''# Recompiler settings for APEX_{name}.dll (APEX 1.x from the game folder), rebased to {base:#x} (--base).
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.environ.get("MAFIA2RE_APEX_{name.upper()}", "~/mafia2-re-data/physx/apex_{name}.dll")
FUNCTIONS_CSV = os.path.join(ROOT, "re", "decomp", "apex_{name}", "functions.csv")
EXTRA_FUNCTIONS = os.path.join(ROOT, "re", "decomp", "apex_{name}", "extra_functions.txt")
TABLE_SUFFIX = "_apex_{name}"
INLINE_HELPERS = {helpers}
JUMP_TABLES = {{}}
NATIVE_ALWAYS = True
NATIVE_OVERRIDES = set()
TRACE_HOOKS = True
''')
    print(name, helpers)
