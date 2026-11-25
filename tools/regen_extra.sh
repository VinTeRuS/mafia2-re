#!/bin/sh
# Regenerates the extra images, three at a time (the recompiler is single-threaded). Unchanged files keep their mtime.
cd "$(dirname "$0")/.." || exit 1
one() {
  i=${1%:*}; b=${1#*:}
  T=${TMPDIR:-/tmp}/m2_${i}_old; rm -rf "$T"; cp -a build/recomp/$i "$T"; rm -f build/recomp/$i/*.c
  python3 -I third_party/x86recomp/tools/recomp.py --config tools/recomp/$i.py --functions all --split 400 --base $b --out build/recomp/$i > build/recomp_$i.log 2>&1 || echo "FAILED $i"
  n=0; for f in build/recomp/$i/*.c; do x=$(basename $f); if cmp -s $f $T/$x; then touch -r $T/$x $f; else n=$((n+1)); fi; done
  echo "$i: changed $n of $(ls build/recomp/$i/*.c | wc -l)"; rm -rf "$T"
}
{ printf '%s\n' physxcore:0x10000000 apex_clothing:0x10500000 apex_destructible:0x10600000 apex_release:0x10400000 \
  nxcooking:0x10300000 apex_emitter:0x10700000 apex_iofx:0x10800000 apex_particles:0x10900000 |
{ while read s; do
    while [ "$(pgrep -c -u "$(id -u)" -x python3)" -ge 4 ]; do sleep 1; done
    one "$s" &
  done
  wait  # in the same subshell as the jobs: a wait after the pipeline would return at once
}; }
echo extras-done
