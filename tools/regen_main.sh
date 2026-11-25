#!/bin/sh
# Regenerates only the main image; unchanged files keep their mtime.
cd "$(dirname "$0")/.." || exit 1
T=${TMPDIR:-/tmp}/m2_all_old; rm -rf "$T"; cp -a build/recomp/all "$T"
X87_EXITS_FILE=build/recomp/x87_exits.json python3 -I third_party/x86recomp/tools/recomp.py --functions all --split 400 --out build/recomp/all > build/recomp.log 2>&1
n=0; for f in build/recomp/all/*.c; do b=$(basename $f); if cmp -s $f $T/$b; then touch -r $T/$b $f; else n=$((n+1)); fi; done
echo "changed files: $n"; rm -rf "$T"
