"""Draws the Redline Recompiled emblem into launcher/redline.ico (16-256 px) and launcher/emblem.png (a preview).

Usage: python tools/make_icon.py

The emblem: a gauge's dial (three quarters of a circle, 135 to 405 degrees, as the launcher's tachometer), its last
fifth the redline in red, a red needle swept into it, and from 48 px up a bold italic R (Segoe UI Black Italic) in the
middle. Drawn at 8x and scaled down, on a dark rounded square. Ours alone: none of the game's or its makers' marks.
The launcher draws the same emblem with Direct2D (launcher.cpp emblem()).
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RED, RIM, DARK, WHITE = (227, 38, 46, 255), (243, 238, 238, 255), (20, 10, 12, 255), (255, 255, 255, 255)
A0, SWEEP, RED_FROM, NEEDLE = 135.0, 270.0, 0.8, 0.86  # degrees, clockwise from +x (screen y down)


def emblem(size, letter=True, background=True):
    s = size * 8
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if background:
        d.rounded_rectangle((0, 0, s - 1, s - 1), radius=s * 0.22, fill=DARK)
    cx, cy, r = s / 2, s * 0.54, s * 0.36
    w_rim, w_red = int(max(s * 0.05, 8)), int(max(s * 0.1, 12))
    box = lambda rr: (cx - rr, cy - rr, cx + rr, cy + rr)
    split = A0 + SWEEP * RED_FROM
    d.arc(box(r), A0, split, fill=RIM, width=w_rim)  # the dial; PIL's angles run clockwise from +x, as these do
    d.arc(box(r + (w_red - w_rim) / 2), split, A0 + SWEEP, fill=RED, width=w_red)  # the redline, wider, outward
    if size >= 32:
        for i in range(1, 7):  # major ticks inside the rim, up to the redline
            a = math.radians(A0 + SWEEP * i / 8)
            r0, r1 = r * 0.78, r * 0.88
            d.line([(cx + r0 * math.cos(a), cy + r0 * math.sin(a)), (cx + r1 * math.cos(a), cy + r1 * math.sin(a))],
                   fill=RIM, width=int(max(s * 0.024, 6)))
    a = math.radians(A0 + SWEEP * NEEDLE)  # the needle, from behind the R into the redline
    tip = (cx + r * 1.02 * math.cos(a), cy + r * 1.02 * math.sin(a))
    nx, ny = -math.sin(a), math.cos(a)
    base, bx, by = s * 0.024, cx + r * 0.3 * math.cos(a), cy + r * 0.3 * math.sin(a)  # from behind the R's leg, not its bowl
    if not letter:
        bx, by = cx, cy
    d.polygon([(bx + nx * base, by + ny * base), tip, (bx - nx * base, by - ny * base)], fill=RED)
    if letter:
        font = ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "seguibli.ttf"),
                                  int(s * 0.5))
        b = d.textbbox((0, 0), "R", font=font)
        d.text((cx - (b[0] + b[2]) / 2, cy - (b[1] + b[3]) / 2 + s * 0.02), "R", font=font, fill=WHITE,
               stroke_width=int(s * 0.018), stroke_fill=DARK)
    else:
        hub = s * 0.07
        d.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=RED)
    return im.resize((size, size), Image.LANCZOS)


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [emblem(n, letter=n >= 48) for n in sizes]
    out = os.path.join(ROOT, "launcher", "redline.ico")
    images[-1].save(out, format="ICO", sizes=[(n, n) for n in sizes], append_images=images[:-1])
    emblem(512).save(os.path.join(ROOT, "launcher", "emblem.png"))
    print("wrote", out)


if __name__ == "__main__":
    sys.exit(main())
