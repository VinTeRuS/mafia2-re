#!/bin/bash
# Packages the APK without Gradle: SDL's Java activity + ours -> classes.dex, the manifest -> aapt2, native
# libraries for the requested ABIs, then zipalign and apksigner (debug key).
#
# Usage: android/build-apk.sh [abi ...]          (default: arm64-v8a)
# Needs: build/android/<abi>/libmain.so (make -C src/native PLATFORM=android ABI=<abi>) and the libraries from
# android/build-deps.sh in $DEPS_ROOT/<abi>/lib. Output: build/apk/mafia2.apk
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
BUILD_TOOLS=${BUILD_TOOLS:-$SDK/build-tools/36.0.0}
PLATFORM_JAR=${PLATFORM_JAR:-$SDK/platforms/android-35/android.jar}
DEPS_ROOT=${DEPS_ROOT:-$HOME/mafia2-re-data/android-deps}
TURNIP_ROOT=${TURNIP_ROOT:-$HOME/mafia2-re-data/android-turnip}
SDL_SRC=${SDL_SRC:-$HOME/proto-re-data/android-deps-src/SDL2}
KEYSTORE=${KEYSTORE:-$HOME/.android/debug.keystore}
ABIS=("${@:-arm64-v8a}")

OUT=$ROOT/build/apk
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/pkg"

# Java: SDL's activity classes plus ours.
javac --release 11 -encoding UTF-8 -nowarn -cp "$PLATFORM_JAR" -d "$OUT/classes" \
    "$SDL_SRC"/android-project/app/src/main/java/org/libsdl/app/*.java "$ROOT"/android/java/com/mafia2re/game/*.java
"$BUILD_TOOLS/d8" --release --min-api 26 --lib "$PLATFORM_JAR" --output "$OUT/dex" \
    $(find "$OUT/classes" -name '*.class')

# Resources (launcher icon; tools/make_icon.py draws it).
"$BUILD_TOOLS/aapt2" compile --dir "$ROOT/android/res" -o "$OUT/res.zip"
# The original Mafia II icon from the user's own game folder replaces the drawn one (not stored in the repo).
GAME_ICON_DIR=${MAFIA2RE_GAME:-$HOME/mafia2-re-data/game}
GAME_RES=()
if [ -e "$GAME_ICON_DIR/icon.ico" ]; then
    rm -rf "$OUT/res_game"
    python3 "$ROOT/tools/icon_from_game.py" "$GAME_ICON_DIR" "$OUT/res_game"
    "$BUILD_TOOLS/aapt2" compile --dir "$OUT/res_game" -o "$OUT/res_game.zip"
    GAME_RES=(-R "$OUT/res_game.zip")
fi
"$BUILD_TOOLS/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM_JAR" --manifest "$ROOT/android/AndroidManifest.xml" \
    -R "$OUT/res.zip" "${GAME_RES[@]}" --auto-add-overlay --min-sdk-version 26 --target-sdk-version 35

cp "$OUT/dex/classes.dex" "$OUT/pkg/"
for abi in "${ABIS[@]}"; do
    mkdir -p "$OUT/pkg/lib/$abi"
    cp "${LIBMAIN_DIR:-$ROOT/build/android/$abi}/libmain.so" "$DEPS_ROOT/$abi"/lib/lib{SDL2,dxvk_d3d9}.so "$OUT/pkg/lib/$abi/"
    # arm64: libmain.so targets ARMv8.2 + RCpc; the ARMv8.0 build for older cores (make ... ARM_ARCH=armv8-a
    # OUT=../../build/android/arm64-v8a-v80) ships next to it, GameActivity picks one at run time.
    # The Turnip probe program (an executable, named like a library so the installer extracts it with the rest).
    if [ -e "$ROOT/build/android/$abi/libturnipprobe.so" ]; then
        cp "$ROOT/build/android/$abi/libturnipprobe.so" "$OUT/pkg/lib/$abi/"
    fi
    if [ -e "$ROOT/build/android/$abi-v80/libmain.so" ]; then
        cp "$ROOT/build/android/$abi-v80/libmain.so" "$OUT/pkg/lib/$abi/libmain_v80.so"
    fi
    # Adreno: Turnip and the adrenotools hooks (android/build-turnip.sh), when built
    if [ "$abi" = arm64-v8a ] && [ -e "$TURNIP_ROOT/lib/libvulkan_freedreno.so" ]; then
        cp "$TURNIP_ROOT"/lib/*.so "$OUT/pkg/lib/$abi/"
    fi
    "$SDK"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-unneeded "$OUT/pkg/lib/$abi"/*.so
done
(cd "$OUT/pkg" && zip -q -r "$OUT/base.apk" classes.dex lib)

"$BUILD_TOOLS/zipalign" -P 16 -f 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/mafia2.apk" "$OUT/aligned.apk"
ls -la "$OUT/mafia2.apk"
