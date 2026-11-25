#!/usr/bin/env python3
# syms.py 0xOFF ... : host offsets from "crash: #NN ... mafia2-native+0xOFF" -> f_<guest va> (+ Ghidra name)
import subprocess, sys, bisect, csv, os
exe = os.path.expanduser("~/mafia2-re/build/native/mafia2-native")
syms = []
for l in subprocess.run(["nm", "-n", exe], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in "tT": syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
names = {}
for f in ("functions.csv", "physxcore/functions.csv", "nxcooking/functions.csv"):
    for r in csv.reader(open(os.path.expanduser("~/mafia2-re/re/decomp/" + f))):
        names[r[0]] = r[4]
for a in sys.argv[1:]:
    v = int(a, 16); i = bisect.bisect_right(addrs, v) - 1
    n = syms[i][1]
    g = n[2:] if n.startswith("f_") else ""
    print(a, n, names.get(g, ""))
