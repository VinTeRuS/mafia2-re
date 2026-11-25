#!/bin/bash
# Profile-guided build from bot sessions on the phone, no one at the controls:
#   tools/pgo_phone.sh [--skip-collect]
# 1. builds the instrumented APK (make PGO=gen, LIBMAIN_DIR=...-pgogen) and installs it
# 2. plays tools/autoplay scenarios with it; each process writes files/mafia2.profraw every 60 s, pulled after each run
# 3. merges the raw profiles (the NDK's llvm-profdata, same LLVM as the compiler) into build/pgo/mafia2.profdata
# 4. builds libmain with PGO=use and packs build/mafia2-pgo.apk (installed only with --install)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
PROFDATA=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-profdata
D=/sdcard/Android/data/com.mafia2re.game/files
RAW=build/pgo/raw
COLLECT=1; INSTALL=0
for a in "$@"; do
    case $a in
        --skip-collect) COLLECT=0 ;;
        --install) INSTALL=1 ;;
    esac
done
mkdir -p "$RAW"

if [ "$COLLECT" = 1 ]; then
    make -C src/native -j4 PLATFORM=android PGO=gen
    cp build/apk/mafia2.apk build/mafia2-release.apk 2>/dev/null || true
    LIBMAIN_DIR=build/android/arm64-v8a-pgogen bash android/build-apk.sh
    mv build/apk/mafia2.apk build/mafia2-pgogen.apk
    adb install -r build/mafia2-pgogen.apk
    n=0
    # a mission on foot, the DLC city intro, the mission again with shooting: ~15 min of play
    for s in tools/autoplay/basic.txt tools/autoplay/jv_intro.txt tools/autoplay/bj_intro.txt tools/autoplay/basic.txt; do
        adb shell rm -f $D/mafia2.profraw
        tools/autoplay.sh "$s" --timeout 900 | grep -E "^result" || true
        n=$((n + 1))
        adb pull $D/mafia2.profraw "$RAW/run$n.profraw" || echo "run $n: no profile written"
    done
fi

ls "$RAW"/*.profraw >/dev/null
"$PROFDATA" merge -o build/pgo/mafia2.profdata "$RAW"/*.profraw
"$PROFDATA" show build/pgo/mafia2.profdata | tail -3
make -C src/native -j4 PLATFORM=android PGO=use
LIBMAIN_DIR=build/android/arm64-v8a-pgo bash android/build-apk.sh
cp build/apk/mafia2.apk build/mafia2-pgo.apk
[ -e build/mafia2-release.apk ] && cp build/mafia2-release.apk build/apk/mafia2.apk
ls -la build/mafia2-pgo.apk
if [ "$INSTALL" = 1 ]; then
    tools/autoplay.sh tools/autoplay/basic.txt --install build/mafia2-pgo.apk
fi
