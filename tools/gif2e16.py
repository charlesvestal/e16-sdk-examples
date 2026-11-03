#!/usr/bin/env python3
"""Convert an animated GIF (or any image / image sequence Pillow reads) into
an .e16v file for the player app (apps/demos/player).

  gif2e16.py in.gif out.e16v                 fit (letterbox), Floyd-Steinberg
  gif2e16.py in.gif out.e16v --fill          crop to fill the screen instead
  gif2e16.py in.gif out.e16v --threshold 100 plain threshold, no dithering
  gif2e16.py in.gif out.e16v --invert        swap lit and dark pixels
  gif2e16.py in.gif out.e16v --preview p.png what the OLED shows (x4) + rings
  gif2e16.py --playlist /Volumes/E16/apps/player
                                             (re)write playlist.txt there,
                                             listing every .e16v file in it

Several inputs (in.gif a.png b.png ... out.e16v) become one sequence, one
frame per still image (or every frame of each animation).

.e16v format (little-endian), version 1:

  header, 32 bytes:
    0  char[4] "E16V"
    4  u8      version (1)
    5  u8      flags: bit 0 = LED colours present, bit 1 = --invert was used
    6  u16     header size (32; frames start here)
    8  u16     frame count
    10 u16     default frame delay, ms (the most common one; informational)
    12 u16     width (128)          14 u16 height (64)
    16 u32     frame record size (4 + 1024 + 768 = 1796)
    20 u16     LED rings (16)       22 u16 LEDs per ring (16)
    24 u8[8]   reserved, 0
  then frame count records of:
    0  u16     this frame's delay, ms (the GIF's own, at least 20)
    2  u16     reserved, 0
    4  u8[1024] the 128x64 1-bit framebuffer in the OLED's native layout
               (e16_api.h / the SDK's display.h): byte page*128 + x holds
               column x of 8-pixel strip `page`, bit 0 = top pixel, 1 = lit
    1028 u8[768] a 16x16 RGB "thumbnail" for the LED rings, already in ring
               order: row r = ring r (encoder r+1), column l = LED l, 3 bytes
               r,g,b each 0-255 (the player scales them to its brightness)

LED mapping: the 16 rings sit in a 4x4 grid (ring r at row r // 4, column
r % 4), laid over the colour frame (the whole source picture, or the --fill
crop, stretched to a square), each ring owning one cell. LED l of a ring
samples (averaging a small patch) the point on a circle around the cell's
centre, radius 0.35 of a cell, at angle --led0-deg + l * 22.5 degrees
clockwise from 12 o'clock (counter-clockwise with --led-ccw). So each ring
shows the colours around its own part of the picture, and together the 16
rings look like a coarse 16-ring "pixel" version of it. The default (LED 0
at 12 o'clock, clockwise) is an assumption about the hardware; adjust with
--led0-deg / --led-ccw if the picture looks rotated or mirrored per ring.
"""
import argparse
import math
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageSequence
except ImportError:
    sys.exit("needs Pillow: pip3 install --user pillow")

W, H = 128, 64
RINGS, LEDS = 16, 16
FB_SIZE = W * H // 8
LED_SIZE = RINGS * LEDS * 3
REC_SIZE = 4 + FB_SIZE + LED_SIZE
HDR_SIZE = 32
MAGIC = b"E16V"
VERSION = 1
MIN_DELAY = 20
FLAG_LEDS, FLAG_INVERT = 1, 2


def load_frames(paths, default_delay):
    """(RGB image, delay ms) for every frame of every input, transparency
    composited onto black."""
    out = []
    for p in paths:
        im = Image.open(p)
        for fr in ImageSequence.Iterator(im):
            d = fr.info.get("duration", 0) or 0
            d = default_delay if d <= 0 else max(MIN_DELAY, int(d))
            rgba = fr.convert("RGBA")
            bg = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
            bg.alpha_composite(rgba)
            out.append((bg.convert("RGB"), min(d, 65535)))
    if not out:
        sys.exit("no frames")
    return out


def crop_fill(im, aspect):
    """Centre crop of im to the given width/height aspect."""
    w, h = im.size
    if w / h > aspect:
        nw = max(1, round(h * aspect))
        x = (w - nw) // 2
        return im.crop((x, 0, x + nw, h))
    nh = max(1, round(w / aspect))
    y = (h - nh) // 2
    return im.crop((0, y, w, y + nh))


def fit_gray(im, fill):
    """The frame as 128x64 grey: cropped (fill) or letterboxed (fit) on
    black. Returns (grey image, mask of the picture area)."""
    g = im.convert("L")
    if fill:
        g = crop_fill(g, W / H)
        return (g.resize((W, H), Image.LANCZOS) if g.size != (W, H) else g), None
    w, h = g.size
    s = min(W / w, H / h)
    nw, nh = max(1, round(w * s)), max(1, round(h * s))
    g = g.resize((nw, nh), Image.LANCZOS) if (nw, nh) != (w, h) else g
    canvas = Image.new("L", (W, H), 0)
    box = ((W - nw) // 2, (H - nh) // 2)
    canvas.paste(g, box)
    mask = Image.new("1", (W, H), 0)
    mask.paste(1, (box[0], box[1], box[0] + nw, box[1] + nh))
    return canvas, mask


def to_1bit(gray, mask, threshold, invert):
    if invert:
        gray = gray.point(lambda v: 255 - v)
    if threshold is None:
        bw = gray.convert("1")  # Floyd-Steinberg
    else:
        bw = gray.point(lambda v: 255 if v >= threshold else 0).convert("1", dither=Image.Dither.NONE)
    if mask is not None:  # letterbox bars stay dark, inverted or not
        black = Image.new("1", (W, H), 0)
        bw = Image.composite(bw, black, mask)
    return bw


def pack_fb(bw):
    px = bw.load()
    fb = bytearray(FB_SIZE)
    for page in range(H // 8):
        for x in range(W):
            b = 0
            for bit in range(8):
                if px[x, page * 8 + bit]:
                    b |= 1 << bit
            fb[page * W + x] = b
    return fb


def unpack_fb(fb):
    """Framebuffer bytes back to a 128x64 list of rows of 0/1."""
    return [[(fb[(y >> 3) * W + x] >> (y & 7)) & 1 for x in range(W)] for y in range(H)]


def led_points(led0_deg, ccw, size=64):
    """For each (ring, led): the sample point in a size x size square."""
    cell = size / 4
    r = 0.35 * cell
    pts = []
    for ring in range(RINGS):
        cx, cy = (ring % 4 + 0.5) * cell, (ring // 4 + 0.5) * cell
        row = []
        for led in range(LEDS):
            a = math.radians(led0_deg + (-1 if ccw else 1) * led * 360 / LEDS)
            row.append((cx + r * math.sin(a), cy - r * math.cos(a)))
        pts.append(row)
    return pts


def led_colours(im, fill, pts, size=64):
    src = crop_fill(im, W / H) if fill else im
    sq = src.resize((size, size), Image.BOX).load()
    out = bytearray()
    for ring in range(RINGS):
        for (x, y) in pts[ring]:
            acc = [0, 0, 0]
            n = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    xi = min(size - 1, max(0, int(x) + dx))
                    yi = min(size - 1, max(0, int(y) + dy))
                    c = sq[xi, yi]
                    for k in range(3):
                        acc[k] += c[k]
                    n += 1
            out += bytes(a // n for a in acc)
    return out


def render_preview(fb, leds, led_pts, scale=4):
    """The OLED (x scale, lit = white) above a 4x4 sketch of the rings."""
    rows = unpack_fb(fb)
    oled = Image.new("RGB", (W, H))
    op = oled.load()
    for y in range(H):
        for x in range(W):
            if rows[y][x]:
                op[x, y] = (255, 255, 255)
    oled = oled.resize((W * scale, H * scale), Image.NEAREST)
    ring_h = W * scale  # square ring panel as wide as the screen
    img = Image.new("RGB", (W * scale, H * scale + 8 + ring_h), (40, 40, 40))
    img.paste(oled, (0, 0))
    d = ImageDraw.Draw(img)
    k = ring_h / 64
    oy = H * scale + 8
    dot = max(2, int(k * 1.6))
    for ring in range(RINGS):
        for led in range(LEDS):
            x, y = led_pts[ring][led]
            i = (ring * LEDS + led) * 3
            c = tuple(leds[i:i + 3])
            cx, cy = x * k, oy + y * k
            d.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=c)
    return img


def write_playlist(folder):
    names = sorted(n for n in os.listdir(folder)
                   if n.lower().endswith(".e16v") and not n.startswith("."))
    path = os.path.join(folder, "playlist.txt")
    with open(path, "w", newline="\n") as f:
        f.write("# one .e16v file name per line, played in this order\n")
        for n in names:
            f.write(n + "\n")
    print(f"{path}: {len(names)} file(s)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="*", help="input image(s) ... output.e16v")
    ap.add_argument("--fill", action="store_true", help="crop to fill 128x64 (default: letterbox)")
    ap.add_argument("--threshold", type=int, metavar="N", help="threshold at N (0-255) instead of dithering")
    ap.add_argument("--invert", action="store_true", help="invert the picture")
    ap.add_argument("--default-delay", type=int, default=100, metavar="MS",
                    help="delay for frames without one (stills), ms (default 100)")
    ap.add_argument("--led0-deg", type=float, default=0.0,
                    help="angle of LED 0, degrees clockwise from 12 o'clock (default 0)")
    ap.add_argument("--led-ccw", action="store_true", help="LEDs count counter-clockwise")
    ap.add_argument("--no-leds", action="store_true", help="leave the ring colours dark")
    ap.add_argument("--preview", metavar="OUT", help="render the OLED (x4) and rings: "
                    ".png = one frame (--preview-frame), .gif = animated")
    ap.add_argument("--preview-frame", type=int, default=0, metavar="N")
    ap.add_argument("--playlist", metavar="DIR", help="write DIR/playlist.txt listing DIR's .e16v files, then exit")
    a = ap.parse_args()

    if a.playlist:
        write_playlist(a.playlist)
        return
    if len(a.paths) < 2:
        ap.error("need input image(s) and an output .e16v")
    if a.threshold is not None and not 0 <= a.threshold <= 255:
        ap.error("--threshold 0-255")
    *inputs, out = a.paths

    frames = load_frames(inputs, max(MIN_DELAY, a.default_delay))
    if len(frames) > 65535:
        sys.exit("too many frames (65535 max)")
    pts = led_points(a.led0_deg, a.led_ccw)
    delays = [d for _, d in frames]
    default_delay = max(set(delays), key=delays.count)
    flags = (0 if a.no_leds else FLAG_LEDS) | (FLAG_INVERT if a.invert else 0)

    hdr = MAGIC + struct.pack("<BBHHHHHIHH8x", VERSION, flags, HDR_SIZE, len(frames),
                              default_delay, W, H, REC_SIZE, RINGS, LEDS)
    assert len(hdr) == HDR_SIZE
    recs = []
    for im, d in frames:
        gray, mask = fit_gray(im, a.fill)
        fb = pack_fb(to_1bit(gray, mask, a.threshold, a.invert))
        leds = bytes(LED_SIZE) if a.no_leds else led_colours(im, a.fill, pts)
        recs.append((d, fb, leds))
    with open(out, "wb") as f:
        f.write(hdr)
        for d, fb, leds in recs:
            f.write(struct.pack("<HH", d, 0) + fb + leds)
    total = sum(delays)
    print(f"{out}: {len(frames)} frames, {HDR_SIZE + len(recs) * REC_SIZE} bytes, "
          f"{total / 1000:.2f} s loop ({1000 * len(frames) / total:.1f} fps avg)")

    if a.preview:
        imgs = [render_preview(fb, leds, pts) for _, fb, leds in recs]
        if a.preview.lower().endswith(".gif"):
            imgs[0].save(a.preview, save_all=True, append_images=imgs[1:],
                         duration=[d for d, _, _ in recs], loop=0)
        else:
            n = min(max(0, a.preview_frame), len(imgs) - 1)
            imgs[n].save(a.preview)
        print(f"preview: {a.preview}")


if __name__ == "__main__":
    main()
