#!/usr/bin/env python3
"""Website / README images from E2E snapshots (docs/notes/29-discoverability.md). Needs Pillow.

    make_images.py --race BMP --second BMP --menu BMP [--setup BMP] --out DIR

Writes DIR/race.jpg, race2.jpg, menu.jpg (1600 px wide), setup.png and social-preview.png
(1280x640, GitHub's social preview size: race picture with the title on a dark band).
The snapshots come from scripts/dev/e2e_race.sh with KEEP_FRAMES=1 (gpu_*.bmp).
"""
import argparse
import os

from PIL import Image, ImageDraw, ImageFont

FONT = "/System/Library/Fonts/Avenir Next Condensed.ttc"   # macOS; any bold sans works


def font(size, index):
    try:
        return ImageFont.truetype(FONT, size, index=index)
    except OSError:
        return ImageFont.load_default(size)


def save_jpg(src, dst, width=1600):
    im = Image.open(src).convert("RGB")
    im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    im.save(dst, quality=86, optimize=True, progressive=True)
    return im


def social(race, dst):
    w, h = 1280, 640
    im = Image.open(race).convert("RGB")
    scale = max(w / im.width, h / im.height)
    im = im.resize((round(im.width * scale), round(im.height * scale)), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    im = im.crop((x, y, x + w, y + h))
    band = Image.new("RGBA", (w, 230), (8, 10, 16, 215))
    im.paste(band, (0, h - 230), band)
    d = ImageDraw.Draw(im)
    d.text((48, h - 214), "Moto Racer Recomp", font=font(96, 9), fill=(255, 255, 255))   # Heavy Italic
    d.text((52, h - 98), "Native on Mac · Windows · Linux · Steam Deck", font=font(44, 2), fill=(255, 176, 0))
    d.text((52, h - 48), "Widescreen up to 4K · gamepads · from your own GOG copy", font=font(30, 5),
           fill=(200, 205, 215))
    im.save(dst, optimize=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--race", required=True)
    ap.add_argument("--second", required=True)
    ap.add_argument("--menu", required=True)
    ap.add_argument("--setup")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    save_jpg(a.race, os.path.join(a.out, "race.jpg"))
    save_jpg(a.second, os.path.join(a.out, "race2.jpg"))
    save_jpg(a.menu, os.path.join(a.out, "menu.jpg"))
    if a.setup:
        Image.open(a.setup).convert("RGB").save(os.path.join(a.out, "setup.png"), optimize=True)
    social(a.race, os.path.join(a.out, "social-preview.png"))


if __name__ == "__main__":
    main()
