#!/bin/sh
# Copies the game data the Android port needs to the phone (Android/data/com.mafia2re.game/files/game).
# Skips pc/jre (the launcher's Java) and Redist; adds physx/ (PhysXCore_283.dll, NxCooking_283.dll).
# Re-running continues: items already complete on the phone (same size) are skipped.
G=~/mafia2-re-data/game
D=/sdcard/Android/data/com.mafia2re.game/files/game
set -e
adb shell mkdir -p $D/pc $D/physx
push() {  # local, remote
    l=$(du -sbL "$1" | cut -f1)
    r=$(adb shell "du -sb '$2' 2>/dev/null" | cut -f1)
    [ "$l" = "$r" ] && { echo "skip $1"; return; }
    echo "push $1 ($l bytes)"; adb push "$1" "$(dirname "$2")/" >/dev/null
}
push $G/edit $D/edit
for f in $G/pc/*; do
    case "$(basename "$f")" in jre) continue ;; esac
    push "$f" "$D/pc/$(basename "$f")"
done
for f in PhysXCore_283.dll NxCooking_283.dll; do adb push ~/mafia2-re-data/physx/$f $D/physx/ >/dev/null; done
adb shell chmod -R a+rwX $D 2>/dev/null || true  # files the app created itself refuse; they are its own anyway
echo done
