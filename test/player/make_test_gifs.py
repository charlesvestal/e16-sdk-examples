#!/usr/bin/env python3
"""Generate the player's test content: two synthetic animated GIFs, each
converted to .e16v (with --preview PNG/GIF next to it) by tools/gif2e16.py.

  test/player/make_test_gifs.py [OUTDIR]     (default build-player/)

  spiral.gif   30 frames, 60 ms: a rotating rainbow spiral with a ball
               bouncing across it
  banner.gif   64 frames, 40 ms: "E16" scrolling across on a moving
               colour gradient, with a frame counter in the corner

Then, in OUTDIR: spiral.e16v, banner.e16v, playlist.txt and the previews.
Nothing here is committed; rerun it to get the files back.
"""
import colorsys
import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GIF2E16 = os.path.join(ROOT, "tools", "gif2e16.py")


def hsv(h, s=1.0, v=1.0):
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, s, v)
    return int(r * 255), int(g * 255), int(b * 255)


def spiral(path, n=30, w=256, h=128):
    frames = []
    cx, cy = w / 2, h / 2
    for i in range(n):
        im = Image.new("RGB", (w, h), (0, 0, 0))
        d = ImageDraw.Draw(im)
        rot = 2 * math.pi * i / n
        for arm in range(3):
            pts = []
            for k in range(220):
                t = k / 220 * 4 * math.pi
                r = 4 + t * 10
                a = t + rot + arm * 2 * math.pi / 3
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a) * 0.9))
            for k in range(len(pts) - 1):
                d.line([pts[k], pts[k + 1]], fill=hsv(arm / 3 + k / 600 + i / n), width=5)
        # a ball bouncing left-right, up-down
        bx = 20 + (w - 40) * abs((i / n * 2) % 2 - 1)
        by = 16 + (h - 32) * abs(math.sin(math.pi * i / n * 2))
        d.ellipse((bx - 12, by - 12, bx + 12, by + 12), fill=(255, 255, 255))
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=60, loop=0)


def font(size):
    for f in ("/System/Library/Fonts/Supplemental/Arial Black.ttf",
              "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
              "/Library/Fonts/Arial Bold.ttf",
              "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def banner(path, n=64, w=128, h=64):
    big, small = font(44), font(10)
    text = "E16"
    tw = ImageDraw.Draw(Image.new("RGB", (1, 1))).textlength(text, font=big)
    frames = []
    for i in range(n):
        im = Image.new("RGB", (w, h))
        px = im.load()
        for y in range(h):  # a dim moving gradient behind (dithers to sparse dots)
            for x in range(w):
                c = hsv(x / w + i / n, 1.0, 0.12 + 0.08 * math.sin((x + y + i * 4) / 9))
                px[x, y] = c
        d = ImageDraw.Draw(im)
        x = w - (w + tw) * i / n
        d.text((x, 6), text, font=big, fill=(255, 255, 255))
        d.rectangle((0, 0, 22, 11), fill=(0, 0, 0))
        d.text((1, 0), f"{i:02d}", font=small, fill=(255, 255, 255))
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=40, loop=0)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-player")
    os.makedirs(out, exist_ok=True)
    jobs = [("spiral", spiral, []), ("banner", banner, ["--fill"])]
    for name, make, opts in jobs:
        gif = os.path.join(out, name + ".gif")
        make(gif)
        e16v = os.path.join(out, name + ".e16v")
        for prev in (name + "_preview.png", name + "_preview.gif"):
            subprocess.check_call([sys.executable, GIF2E16, gif, e16v, *opts,
                                   "--preview", os.path.join(out, prev)])
    subprocess.check_call([sys.executable, GIF2E16, "--playlist", out])


if __name__ == "__main__":
    main()
