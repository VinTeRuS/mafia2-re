#!/bin/sh
# Headless run (no window on the user's screen, no sound): run-headless.sh [seconds]
T=${1:-120}
cd ~/mafia2-re-data/native_run || exit 1
DXVK_CONFIG="d3d9.presentInterval = 0" MAFIA2RE_CREDIT="Mafia II Android port by VinTeRuS" MAFIA2RE_AUDIO=${AUDIO:-0} MAFIA2RE_WINDOWED=1 SDL_VIDEODRIVER=x11 \
MESA_VK_WSI_DEBUG=sw SDL_AUDIODRIVER=dummy timeout -s KILL "$T" nice -n 19 xvfb-run -a -s "-screen 0 1280x720x24" \
~/mafia2-re/build/native/mafia2-native ~/mafia2-re-data/game > run.out 2>&1
echo rc=$?; tail -${LINES_TAIL:-25} mafia2-native.log
