# mafia2-re

[Русский](README.ru.md) | English

Native Android port of Mafia II (2010). Not an emulator and not Wine: `pc/mafia2.exe` is statically recompiled from
x86 to C, built for arm64 and run on top of my own Win32/D3D9 layer (SDL2, DXVK). The game's physics is the real
PhysX 2.8.3 and APEX, recompiled the same way.

> **The project is at an early stage.** Expect frequent commits, quick fixes, things breaking and getting rewritten.

If you like the port, you can support me on Patreon: **[patreon.com/VinTeRuS](https://www.patreon.com/cw/VinTeRuS/membership)**.

## Status

- The whole game runs: intro videos, menus, new game and saves, the story missions, the city, driving, shooting,
  cutscenes with subtitles, sound and music. The DLC episodes (Jimmy's Vendetta, Betrayal of Jimmy) start too.
- Touch controls change with the situation (on foot, in a car, in menus) and hide during videos; gamepads work.
- Settings (gear button): resolution, texture quality, FPS limit, game graphics preset, battery saver, button size
  and opacity, vibration, camera speed, invert Y.
- Tested on a Pixel 6 (Mali-G78) only.
- Known problems: the GPU is the limit on phones, so heavy scenes (the city at night, big fights) drop frames; no
  multiplayer yet.

## Installing

Android 10+, arm64.

1. Copy your installed game folder (`pc`, `edit`; `pc/jre` and `Redist` aren't needed) to
   `Internal storage/Android/data/com.mafia2re.game/files/game` or to `Internal storage/Mafia II`.
2. Next to it, a `physx` folder with `PhysXCore_283.dll` and `NxCooking_283.dll` (PhysX 2.8.3), taken from the
   PhysX installer the game ships in `Redist` (for example with 7-Zip).
3. Install the APK and start it.

The APK has no game files in it: you need your own copy of the game.

## Game version

GOG Mafia II (the 2010 original, not the Definitive Edition), `pc/mafia2.exe` (sha256 in `re/target.sha256`). Other
builds won't work.

## What's in the repo

My own code: the Win32/D3D9/DirectSound host (`src/native`), the Android app (`android/`, Java: touch controls and
settings), build and test scripts (`tools/`) and the recompiler (`third_party/x86recomp`). Hand-written replacements
for a few hot game functions are checked against the original machine code (`tools/difftest`). No game files and no
recompiled game code: that code is generated from your own copy of the game at build time.

## Building (rough)

Linux, Python 3 with `capstone` and `pefile`, the Android NDK and SDK build-tools, the game installed at
`~/mafia2-re-data/game`, a Ghidra function list of the exe in `re/decomp/functions.csv`.

1. `sh tools/regen_main.sh` generates C from the game (settings in `tools/recomp/game.py`); run it twice the first
   time. `tools/regen_extra.sh` does the same for PhysX and APEX.
2. `make -C src/native -j4` for a desktop (Linux) build; `tools/run-input.sh` runs it headless with scripted input.
3. Android dependencies (SDL2, DXVK, a Bink-only FFmpeg) in `~/mafia2-re-data/android-*`, then
   `make -C src/native -j4 PLATFORM=android` and `android/build-apk.sh` give `build/apk/mafia2.apk`.

`tools/autoplay.sh` plays a scripted scenario on a connected phone (for example the game's built-in benchmark,
`tools/autoplay/builtin_bench.txt`) and collects the log and screenshots.

## Credits and forks

Port author: **VinTeRuS** (https://github.com/VinTeRuS). If you fork this project, share the port, publish a build
of it or use its code (the host, the recompiler, the Android app, any part of it), you **must**:

- credit the author of the original port (VinTeRuS) and link this repository: https://github.com/VinTeRuS/mafia2-re,
  in your README, release notes and in the app's description;
- say clearly which changes are yours, and not present the port or its parts as your own work;
- not sell builds or put them behind a paywall;
- not add game files or code generated from the game to your fork or builds.

## Legal

The repository has no game files and no code generated from them, and the port does not bypass any DRM: you need
your own bought copy of Mafia II. Mafia II is a trademark of Take-Two Interactive; this project isn't affiliated with
Take-Two, 2K or Hangar 13.
