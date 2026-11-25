#!/usr/bin/env python3
"""Draws the launcher icon: a gold fedora and a city skyline on a dark sepia night. Original art, no game assets."""
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024
GOLD, DARK, CREAM = (200, 164, 92), (18, 14, 11), (240, 230, 207)

def background():
    im = Image.new("RGB", (S, S), DARK)
    d = ImageDraw.Draw(im)
    for y in range(S):  # night sky fading to warm haze near the horizon
        t = y / S
        d.line([(0, y), (S, y)], fill=(int(18 + 70 * t * t), int(14 + 42 * t * t), int(11 + 22 * t * t)))
    d.ellipse([610, 120, 830, 340], fill=(226, 208, 164))  # moon
    x = 0
    heights = [300, 420, 360, 520, 300, 610, 380, 470, 320, 540, 350, 280]
    for i, h in enumerate(heights):  # skyline
        w = 60 + (i * 37) % 70
        d.rectangle([x, S - h, x + w, S], fill=(10, 8, 7))
        for wy in range(S - h + 24, S - 30, 46):
            for wx in range(x + 12, x + w - 12, 26):
                if (wx * 7 + wy * 3 + i) % 5 == 0:
                    d.rectangle([wx, wy, wx + 9, wy + 16], fill=(170, 130, 70))
        x += w
    return im.filter(ImageFilter.GaussianBlur(1.2))

def foreground():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # fedora: brim, crown with a pinched top, band
    d.ellipse([190, 540, 834, 680], fill=GOLD + (255,))
    crown = [(330, 610), (352, 400), (420, 330), (512, 372), (604, 330), (672, 400), (694, 610)]
    d.polygon(crown, fill=GOLD + (255,))
    d.polygon([(338, 548), (686, 548), (694, 610), (330, 610)], fill=DARK + (255,))
    d.ellipse([300, 590, 724, 652], fill=(150, 118, 62, 255))
    return im

def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "android", "res")
    bg, fg = background(), foreground()
    full = bg.convert("RGBA")
    full.alpha_composite(fg)
    for name, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(root, "mipmap-" + name)
        os.makedirs(d, exist_ok=True)
        full.convert("RGB").resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
        layer = int(px * 2.25)  # adaptive layers are 108 dp, the icon 48 dp
        bg.resize((layer, layer), Image.LANCZOS).save(os.path.join(d, "ic_launcher_bg.png"))
        # keep the hat inside the adaptive safe zone
        small = fg.resize((int(S * 0.72), int(S * 0.72)), Image.LANCZOS)
        canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        canvas.alpha_composite(small, ((S - small.width) // 2, (S - small.height) // 2))
        canvas.resize((layer, layer), Image.LANCZOS).save(os.path.join(d, "ic_launcher_fg.png"))
    full.convert("RGB").resize((512, 512), Image.LANCZOS).save(os.path.join(root, "..", "icon-512.png"))

main()
