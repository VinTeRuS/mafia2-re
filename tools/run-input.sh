#!/bin/sh
# Headless run with scripted input and screenshots: run-input.sh SECS "INPUT" "SHOT_FRAMES"
# INPUT = "<frame>:<SDL key name>,<frame>:click:<x>,<y>,..." (user32.cpp), shots land in native_run/shot_<frame>.png
T=${1:-120}
cd ~/mafia2-re-data/native_run || exit 1
rm -f shot_*.ppm shot_*.png
MAFIA2RE_INPUT="$2" MAFIA2RE_SHOT="$3" DXVK_CONFIG="d3d9.presentInterval = 0" MAFIA2RE_CREDIT="Mafia II Android port by VinTeRuS" MAFIA2RE_AUDIO=${AUDIO:-0} MAFIA2RE_WINDOWED=1 SDL_VIDEODRIVER=x11 \
MESA_VK_WSI_DEBUG=sw SDL_AUDIODRIVER=dummy timeout -s KILL "$T" nice -n 19 xvfb-run -a -s "-screen 0 1280x720x24" \
~/mafia2-re/build/native/mafia2-native ~/mafia2-re-data/game > run.out 2>&1
python3 - <<'P'
from PIL import Image
import glob, os
for f in sorted(glob.glob(os.path.expanduser('~/mafia2-re-data/native_run/shot_*.ppm'))):
    Image.open(f).save(f.replace('.ppm','.png')); print(f.replace('.ppm','.png'))
P
