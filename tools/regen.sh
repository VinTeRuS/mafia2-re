#!/bin/sh
# Regenerates build/recomp/all; files whose content did not change keep their mtime, so make rebuilds only the rest.
cd "$(dirname "$0")/.." || exit 1
T=${TMPDIR:-/tmp}/m2_all_old; rm -rf "$T"; cp -a build/recomp/all "$T"
python3 -I third_party/x86recomp/tools/recomp.py --functions all --split 400 --out build/recomp/all > build/recomp.log 2>&1
n=0; for f in build/recomp/all/*.c; do b=$(basename $f); if cmp -s $f $T/$b; then touch -r $T/$b $f; else n=$((n+1)); fi; done
echo "changed files: $n"; rm -rf "$T"
# Extra images (PhysX, cooking, APEX) at the bases that avoid collisions; see regen_one in tools/regen_extra.sh.
sh tools/regen_extra.sh
