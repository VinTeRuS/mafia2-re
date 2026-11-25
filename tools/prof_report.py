#!/usr/bin/env python3
"""Profile report for the Android build: reads the game's log from the phone (or a saved logcat file), maps the CPU
profiler's libmain offsets to function names with the unstripped library, and prints where the time goes.

  tools/prof_report.py [--serial S] [--log FILE] [--last N]

CPU: the last N "profile:" reports (default 3), summed per function; "via" rows are samples in libc, the driver or
DXVK charged to the libmain function that called them. Threads: samples per thread with their names.
GPU: the last "gpu:" frames. Perf: the last fps lines."""
import argparse, bisect, collections, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "build/android/arm64-v8a/libmain.so")
NDK_NM = os.path.expanduser("~/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm")

def adb(serial, *args):
    cmd = ["adb"] + (["-s", serial] if serial else []) + list(args)
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=60).stdout

def symbols():
    out = subprocess.run([NDK_NM, "-n", "--defined-only", LIB], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16), p[2]))
    return [a for a, _ in syms], [n for _, n in syms]

def demangle(name):
    if not name.startswith("_Z"):
        return name
    try:
        return subprocess.run(["c++filt", name], capture_output=True, text=True).stdout.strip()[:70]
    except OSError:
        return name

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial")
    ap.add_argument("--log")
    ap.add_argument("--last", type=int, default=3)
    ap.add_argument("--lib", help="unstripped module the offsets refer to (PC: build/native/mafia2-native)")
    ap.add_argument("--nm", help="nm to read it with (PC: nm)")
    a = ap.parse_args()
    global LIB, NDK_NM
    LIB = a.lib or LIB
    NDK_NM = a.nm or NDK_NM
    text = open(a.log, errors="replace").read() if a.log else adb(a.serial, "logcat", "-d", "-s", "mafia2re")
    lines = [re.sub(r"^.*?mafia2re: ", "", l) for l in text.splitlines() if "mafia2re:" in l or not a.log or True]
    # CPU reports
    reports, cur = [], None
    for l in lines:
        if l.startswith("profile: ") and "samples" in l:
            cur = {"head": l, "items": [], "threads": None}
            reports.append(cur)
        elif l.startswith("profile threads:") and reports:
            reports[-1]["threads"] = l
        elif l.startswith("profile:") and cur is not None:
            cur["items"] += re.findall(r" (c?)([0-9a-f]+):(\d+)", l)
    addrs, names = symbols()
    if reports:
        use = reports[-a.last:]
        per = collections.Counter()
        total = 0
        for r in use:
            m = re.search(r"profile: (\d+) samples", r["head"])
            total += int(m.group(1)) if m else 0
            for via, off, cnt in r["items"]:
                o = int(off, 16)
                i = bisect.bisect_right(addrs, o) - 1
                fn = demangle(names[i]) if i >= 0 else "?"
                per[("via " if via else "") + fn] += int(cnt)
        print(f"CPU: {total} samples (1 ms of CPU each) in the last {len(use)} reports")
        for fn, cnt in per.most_common(40):
            print(f"  {100.0 * cnt / max(total, 1):5.1f}%  {fn}")
        names_by_tid = {}
        pid = adb(a.serial, "shell", "pidof", "com.mafia2re.game").strip()
        if pid:
            for row in adb(a.serial, "shell", "ps", "-T", "-p", pid).splitlines()[1:]:
                p = row.split()
                if len(p) >= 10:
                    names_by_tid[p[2]] = p[-1]
        th = collections.Counter()
        for r in use:
            if r["threads"]:
                for tid, cnt in re.findall(r" (\d+):(\d+)", r["threads"]):
                    th[tid] += int(cnt)
        print("Threads (share of samples):")
        for tid, cnt in th.most_common(10):
            print(f"  {100.0 * cnt / max(total, 1):5.1f}%  {tid} {names_by_tid.get(tid, '')}")
    else:
        print("CPU: no profile reports (cpu_profile = 1 in mafia2re.ini?)")
    gpu = [i for i, l in enumerate(lines) if l.startswith("gpu: frame")]
    for i in gpu[-2:]:
        print(lines[i])
        for l in lines[i + 1:i + 13]:
            if l.startswith("gpu:   "):
                print(" ", l[4:])
    perf = [l for l in lines if l.startswith("perf:")]
    print("Perf:", *perf[-4:], sep="\n  ")

main()
