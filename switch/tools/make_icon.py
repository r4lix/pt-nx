#!/usr/bin/env python3
"""Draws the public .nro icon (switch/assets/icon.jpg): an empty corridor in one-point perspective with a lit end, and "P.T." in Noto Sans
(assets/fonts, SIL OFL). Original artwork: nothing of the game. Personal builds may use their own icon from switch/assets/private/.

usage: make_icon.py [output]   (needs Pillow)
"""
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCALE = 4
SIZE = 256 * SCALE


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "switch" / "assets" / "icon.jpg"
    s = SCALE
    img = Image.new("RGB", (SIZE, SIZE))
    px = img.load()
    for y in range(SIZE):  # dark, slightly warm towards the floor
        t = y / (SIZE - 1)
        c = (int(10 + 10 * t), int(10 + 8 * t), int(12 + 4 * t))
        for x in range(SIZE):
            px[x, y] = c

    # the far end of the corridor: a lit opening with a soft glow around it
    end = (104 * s, 58 * s, 152 * s, 136 * s)
    glow = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(glow).rectangle(end, fill=255)
    glow = glow.filter(ImageFilter.GaussianBlur(26 * s))
    warm = Image.new("RGB", (SIZE, SIZE), (196, 178, 140))
    img = Image.composite(warm, img, glow.point(lambda v: int(v * 0.55)))

    d = ImageDraw.Draw(img)
    # walls, floor and ceiling: the corners of the frame run to the corners of the opening
    corners = [(0, 0), (SIZE, 0), (0, SIZE), (SIZE, SIZE)]
    targets = [(end[0], end[1]), (end[2], end[1]), (end[0], end[3]), (end[2], end[3])]
    floor = [(0, SIZE), (SIZE, SIZE), (end[2], end[3]), (end[0], end[3])]
    d.polygon(floor, fill=(30, 26, 22))
    for a, b in zip(corners, targets):
        d.line([a, b], fill=(70, 66, 60), width=2 * s)
    # door frames on both walls, smaller with depth
    for depth in (0.28, 0.55, 0.75):
        for side in (0, 1):
            x_near = 0 if side == 0 else SIZE
            x_far = end[0] if side == 0 else end[2]
            x = x_near + (x_far - x_near) * depth
            top = end[1] * depth + 0 * (1 - depth)
            bottom = SIZE + (end[3] - SIZE) * depth
            h = bottom - top
            d.line([(x, top + h * 0.22), (x, bottom)], fill=(58, 54, 49), width=max(1, int(2 * s * (1 - depth) + s)))
    d.rectangle(end, fill=(214, 200, 166))
    d.rectangle(end, outline=(120, 110, 92), width=2 * s)

    font = ImageFont.truetype(str(ROOT / "assets" / "fonts" / "NotoSans.ttf"), 50 * s)
    text = "P.T."
    box = d.textbbox((0, 0), text, font=font)
    x = (SIZE - (box[2] - box[0])) / 2 - box[0]
    y = 168 * s - box[1]
    shadow = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(shadow).text((x, y), text, font=font, fill=255)
    shadow = shadow.filter(ImageFilter.GaussianBlur(6 * s))
    img = Image.composite(Image.new("RGB", (SIZE, SIZE), (0, 0, 0)), img, shadow.point(lambda v: int(v * 0.8)))
    d = ImageDraw.Draw(img)
    d.text((x, y), text, font=font, fill=(226, 224, 218))
    small = ImageFont.truetype(str(ROOT / "assets" / "fonts" / "NotoSans.ttf"), 13 * s)
    label = "S W I T C H   P O R T"
    box = d.textbbox((0, 0), label, font=small)
    d.text(((SIZE - (box[2] - box[0])) / 2 - box[0], 232 * s - box[1]), label, font=small, fill=(150, 146, 138))

    img = img.resize((256, 256), Image.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out, quality=92)
    print(f"icon written to {out}")


if __name__ == "__main__":
    main()
