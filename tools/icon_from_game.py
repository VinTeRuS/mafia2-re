#!/usr/bin/env python3
"""icon_from_game.py GAME_DIR OUT_RES: launcher icon from the game's own icon.ico (the red II with the gangster), red on
black like the title screen. Written into a resource overlay at build time, so no game asset is kept in the repo."""
import os, sys
from PIL import Image

game, out = sys.argv[1], sys.argv[2]
ico = Image.open(os.path.join(game, "icon.ico"))
ico.size = max(ico.info.get("sizes", {(256, 256)}))
ico.load()
logo = ico.convert("RGBA")
logo = logo.crop(logo.getbbox())
BLACK = (0, 0, 0, 255)

def placed(size, frac, bg):
    im = Image.new("RGBA", (size, size), bg)
    s = int(size * frac)
    w, h = logo.size
    k = s / max(w, h)
    l = logo.resize((max(1, int(w * k)), max(1, int(h * k))), Image.LANCZOS)
    im.alpha_composite(l, ((size - l.width) // 2, (size - l.height) // 2))
    return im

# legacy icon 48dp, adaptive layers 108dp (the launcher's mask keeps the middle 72dp: the logo stays inside it)
for dpi, k in {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}.items():
    d = os.path.join(out, f"mipmap-{dpi}")
    os.makedirs(d, exist_ok=True)
    placed(int(48 * k), 0.78, BLACK).save(os.path.join(d, "ic_launcher.png"))
    placed(int(108 * k), 0.56, (0, 0, 0, 0)).save(os.path.join(d, "ic_launcher_fg.png"))
    Image.new("RGBA", (int(108 * k), int(108 * k)), BLACK).save(os.path.join(d, "ic_launcher_bg.png"))
