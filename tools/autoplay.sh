#!/bin/bash
# Unattended run on the phone: tools/autoplay.sh [scenario] [--install APK] [--timeout SECS] [--keep]
# Puts the scenario on the phone as autoplay.txt, starts the game, waits for "autoplay: done" / a crash / the
# timeout, then pulls the log, screenshots and the crash buffer into build/tmp/autoplay/<time>/ and prints a summary.
# The scenario file is removed from the phone once the game has read it, so a normal start never autoplays.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCEN=$ROOT/tools/autoplay/basic.txt
APK= ; TIMEOUT=900 ; KEEP=0 ; SETS=()
while [ $# -gt 0 ]; do
    case $1 in
        --install) APK=$2; shift ;;
        --timeout) TIMEOUT=$2; shift ;;
        --keep) KEEP=1 ;;
        --set) SETS+=("$2"); shift ;;  # key=value written into the phone's mafia2re.ini before the start
        *) SCEN=$1 ;;
    esac
    shift
done
PKG=com.mafia2re.game
D=/sdcard/Android/data/$PKG/files
OUT=$ROOT/build/tmp/autoplay/$(date +%m%d-%H%M%S)
mkdir -p "$OUT"
adb get-state >/dev/null || { echo "no phone"; exit 1; }

[ -n "$APK" ] && { adb install -r "$APK" | tail -1 || exit 1; }
adb shell am force-stop $PKG
adb shell rm -f "$D/autoplay_*.ppm" "$D/shot_now" "$D/shot_now.ppm" "$D/mafia2-native.log"
adb push "$SCEN" $D/autoplay.txt >/dev/null || exit 1
if [ ${#SETS[@]} -gt 0 ]; then
    INI=$(mktemp)
    adb shell cat $D/mafia2re.ini > "$INI"
    for kv in "${SETS[@]}"; do
        k=${kv%%=*}; v=${kv#*=}
        grep -v "^$k *=" "$INI" > "$INI.new"; echo "$k = $v" >> "$INI.new"; mv "$INI.new" "$INI"
    done
    adb push "$INI" $D/mafia2re.ini >/dev/null
    cp "$INI" "$OUT/mafia2re.ini"
    rm -f "$INI"
fi
adb logcat -c -b crash 2>/dev/null
adb shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
echo "started $(date +%T), scenario $(basename "$SCEN"), output $OUT"

t0=$(date +%s); removed=0; result=timeout
while [ $(( $(date +%s) - t0 )) -lt "$TIMEOUT" ]; do
    sleep 5
    st=$(adb shell "grep -E -c 'autoplay: [0-9]+ steps' $D/mafia2-native.log 2>/dev/null")
    if [ "$removed" = 0 ] && [ "${st:-0}" -gt 0 ]; then adb shell rm -f $D/autoplay.txt; removed=1; fi
    last=$(adb shell "grep -E 'autoplay: (done|step|timeout)|^crash: signal|^watchdog: no frame' $D/mafia2-native.log 2>/dev/null | tail -1")
    case $last in
        *"autoplay: done"*) result=done; break ;;
        *"crash: signal"*) result=crash; break ;;
        *"watchdog: no frame"*) result=hang; sleep 3; break ;;
    esac
    if ! adb shell pidof $PKG >/dev/null; then result=exited; break; fi
    # someone opened another app: the game pauses in the background, so stop instead of waiting it out
    # only the topResumedActivity line counts: dumpsys also prints "Broken pipe" notes when grep closes early
    top=$(adb shell "dumpsys activity activities 2>/dev/null" 2>/dev/null | grep -m1 topResumedActivity)
    if [ -n "$top" ] && ! echo "$top" | grep -q $PKG; then
        away=$(( ${away:-0} + 1 ))
        [ "$away" -ge 3 ] && { result=background; break; }
    else
        away=0
    fi
done
adb shell rm -f $D/autoplay.txt
echo "result: $result after $(( $(date +%s) - t0 )) s"

adb pull $D/mafia2-native.log "$OUT/" >/dev/null
for f in $(adb shell "ls $D/autoplay_*.ppm 2>/dev/null"); do adb pull "$f" "$OUT/" >/dev/null; done
adb logcat -d -b crash > "$OUT/logcat-crash.txt" 2>/dev/null
adb pull $D/app_process64_d3d9.log "$OUT/dxvk.log" >/dev/null 2>&1
[ "$KEEP" = 1 ] || adb shell am force-stop $PKG
cp "$SCEN" "$OUT/scenario.txt"
# the unstripped libraries this run used (offsets in the cpu_profile lines refer to them): hard links, no copies
ln -f "$ROOT/build/android/arm64-v8a/libmain.so" "$OUT/libmain.so" 2>/dev/null
ln -f "$ROOT/build/android/arm64-v8a-fast/libmain.so" "$OUT/libmain_fast.so" 2>/dev/null
python3 - "$OUT" <<'P'
import glob, sys
try:
    from PIL import Image
except ImportError:
    sys.exit()
for f in glob.glob(sys.argv[1] + '/*.ppm'):
    Image.open(f).save(f[:-4] + '.png')
P
rm -f "$OUT"/*.ppm
L=$OUT/mafia2-native.log
echo "--- steps / state"
grep -E "autoplay: (state|timeout|shot|done|unknown)|player state|^crash: (signal|pc)" "$L" | cut -c1-160
[ -x "$ROOT/tools/autoplay_extra.sh" ] && "$ROOT/tools/autoplay_extra.sh" "$OUT"
[ -s "$OUT/logcat-crash.txt" ] && echo "--- logcat crash buffer: $OUT/logcat-crash.txt"
ls "$OUT"
