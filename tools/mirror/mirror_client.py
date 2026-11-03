#!/usr/bin/env python3
"""A command-line client for the E16 mirror (protocol in src/mirror/mirror_core.h).

Subscribes over USB MIDI, keeps the stream alive, decodes it, and prints
the state and every input event; at the end prints what arrived and,
optionally, writes the OLED (and the rings) as a PNG. Needs mido and
python-rtmidi (pip install mido python-rtmidi), like tools/console.py.

  mirror_client.py                       watch for 10 s
  mirror_client.py -t 30 --png oled.png  watch 30 s, then save the picture
  mirror_client.py --text                print the OLED as text art at the end
  mirror_client.py --rings               print the LED rings' levels at the end
  mirror_client.py --do "turn 1 3" --do "press 2" --do "shift-hold 1.2"
                                         drive the E16 remotely after subscribing
                                         (turn N STEPS | press N | down N | up N |
                                         shift-tap | shift-hold S | wait S; N 1-16)
  mirror_client.py --no-keepalive        subscribe once and report when the
                                         stream stops (the 3 s timeout)
  mirror_client.py --quiet               no per-event lines
"""
import argparse
import struct
import sys
import time
import zlib

import mido

HDR = [0x7D, 0x45]
MODES = ["menu", "script", "error", "disk", "webcam", "app"]


def find_ports():
    def mine(n):
        return "E16" in n and "FW Update" not in n
    iname = next((n for n in mido.get_input_names() if mine(n)), None)
    oname = next((n for n in mido.get_output_names() if mine(n)), None)
    if not iname or not oname:
        sys.exit("no E16 MIDI port")
    return iname, oname


def unpack7(data):
    out = []
    for i in range(0, len(data), 8):
        top = data[i]
        for j, b in enumerate(data[i + 1:i + 8]):
            out.append(b | (((top >> j) & 1) << 7))
    return out


class Mirror:
    def __init__(self):
        self.oled = [[0] * 128 for _ in range(8)]
        self.rings = [[(0, 0, 0)] * 16 for _ in range(16)]
        self.state = None
        self.counts = {"oled": 0, "ring": 0, "event": 0, "state": 0, "sync": 0, "bytes": 0}
        self.events = []
        self.first = None
        self.last = None

    def take(self, d, quiet):
        """d: the SysEx payload without F0/F7."""
        if len(d) < 3 or d[0:2] != HDR:
            return False
        cmd, p = d[2], d[3:]
        now = time.time()
        if cmd == 0x60:
            return False  # a log line: not the mirror's
        if cmd not in (0x70, 0x71, 0x72, 0x73, 0x74):
            return False
        self.counts["bytes"] += len(d) + 2
        self.first = self.first or now
        self.last = now
        if cmd == 0x71:
            page, col = p[0], p[1]
            seg = unpack7(p[2:])
            self.oled[page][col:col + len(seg)] = seg
            self.counts["oled"] += 1
        elif cmd == 0x72:
            v = unpack7(p[1:])
            self.rings[p[0]] = [tuple(v[i * 3:i * 3 + 3]) for i in range(16)]
            self.counts["ring"] += 1
        elif cmd == 0x73:
            kind, index, value, source = p[0], p[1], p[2], p[3]
            name = "Shift" if index == 16 else f"enc {index + 1}"
            who = "remote" if source else "device"
            if kind == 0:
                text = f"turn {name} {value - 64:+d} ({who})"
            else:
                text = f"{'down' if value else 'up'} {name} ({who})"
            self.events.append((now, text))
            self.counts["event"] += 1
            if not quiet:
                print(f"event: {text}", flush=True)
        elif cmd == 0x70:
            proto, mode, flags = p[0], p[1], p[2]
            rest = bytes(p[3:])
            version, _, name = rest.partition(b"\0")
            self.state = {
                "proto": proto,
                "mode": MODES[mode] if mode < len(MODES) else str(mode),
                "remote": bool(flags & 1),
                "dev": bool(flags & 2),
                "version": version.decode("ascii", "replace"),
                "name": name.decode("ascii", "replace"),
            }
            self.counts["state"] += 1
            print(f"state: {self.state}", flush=True)
        elif cmd == 0x74:
            self.counts["sync"] += 1
        return True

    def text(self):
        rows = []
        for y in range(0, 64, 2):
            row = ""
            for x in range(128):
                a = (self.oled[y // 8][x] >> (y % 8)) & 1
                b = (self.oled[(y + 1) // 8][x] >> ((y + 1) % 8)) & 1
                row += " ▀▄█"[a | (b << 1)]
            rows.append(row.rstrip())
        return "\n".join(rows)

    def png(self, path, scale=4):
        """The OLED at scale, white on black, with a 4x4 sketch of the rings under it."""
        w, h_oled = 128 * scale, 64 * scale
        cell = w // 4
        h = h_oled + cell * 4
        img = [[(0, 0, 0)] * w for _ in range(h)]
        for y in range(64):
            for x in range(128):
                if (self.oled[y // 8][x] >> (y % 8)) & 1:
                    for dy in range(scale):
                        for dx in range(scale):
                            img[y * scale + dy][x * scale + dx] = (235, 242, 255)
        import math
        for r in range(16):
            cx, cy = (r % 4) * cell + cell // 2, h_oled + (r // 4) * cell + cell // 2
            for led in range(16):
                rgb = self.rings[r][led]
                c = tuple(min(255, v * 3) for v in rgb)  # levels stay under ~85
                a = math.radians(led * 22.5)
                px, py = int(cx + math.sin(a) * cell * 0.35), int(cy - math.cos(a) * cell * 0.35)
                for dy in range(-4, 5):
                    for dx in range(-4, 5):
                        if 0 <= py + dy < h and 0 <= px + dx < w:
                            img[py + dy][px + dx] = c
        raw = b"".join(b"\0" + bytes(v for px in row for v in px) for row in img)

        def chunk(t, data):
            c = struct.pack(">I", len(data)) + t + data
            return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def enc(arg):
    if arg == "shift":
        return 16
    n = int(arg)
    if not 1 <= n <= 16:
        sys.exit("encoder 1-16")
    return n - 1


def actions(spec):
    """'turn 1 3' -> [(ctl, act, n) or ('wait', s)]"""
    w = spec.split()
    if w[0] == "turn":
        return [(enc(w[1]), 2, 64 + max(-63, min(63, int(w[2]))))]
    if w[0] == "press":
        return [(enc(w[1]), 1, 64), ("wait", 0.08), (enc(w[1]), 0, 64)]
    if w[0] in ("down", "up"):
        return [(enc(w[1]), 1 if w[0] == "down" else 0, 64)]
    if w[0] == "shift-tap":
        return [(16, 1, 64), ("wait", 0.15), (16, 0, 64)]
    if w[0] == "shift-hold":
        return [(16, 1, 64), ("wait", float(w[1]) if len(w) > 1 else 1.3), (16, 0, 64)]
    if w[0] == "wait":
        return [("wait", float(w[1]))]
    sys.exit(f"unknown action {spec!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-t", "--seconds", type=float, default=10)
    ap.add_argument("--png")
    ap.add_argument("--text", action="store_true")
    ap.add_argument("--rings", action="store_true")
    ap.add_argument("--do", action="append", default=[])
    ap.add_argument("--no-keepalive", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--log", action="store_true", help="print the launcher's log lines too")
    a = ap.parse_args()

    iname, oname = find_ports()
    m = Mirror()
    todo = [x for spec in a.do for x in actions(spec)]
    with mido.open_input(iname) as inp, mido.open_output(oname) as out:
        def send(*d):
            out.send(mido.Message("sysex", data=HDR + list(d)))
        send(0x20)
        t0 = time.time()
        sub_at = t0
        keep_at = t0
        next_action = t0 + 0.5  # let the full refresh go first
        log = ""
        while time.time() - t0 < a.seconds:
            now = time.time()
            if not a.no_keepalive and now - keep_at >= 1.0:
                send(0x20)
                keep_at = now
            if todo and now >= next_action:
                x = todo.pop(0)
                if x[0] == "wait":
                    next_action = now + x[1]
                else:
                    send(0x10, *x)
                    next_action = now + 0.04
            for msg in inp.iter_pending():
                if msg.type != "sysex":
                    continue
                d = list(msg.data)
                if not m.take(d, a.quiet) and a.log and d[:3] == HDR + [0x60]:
                    log += bytes(d[3:]).decode("ascii", "replace")
                    while "\n" in log:
                        line, log = log.split("\n", 1)
                        print(f"log: {line}", flush=True)
            time.sleep(0.002)
        if not a.no_keepalive:
            send(0x21)

    span = (m.last - m.first) if m.first and m.last and m.last > m.first else 0
    c = m.counts
    print(f"received over {a.seconds:.1f} s: {c['sync']} frames, {c['oled']} oled segments, {c['ring']} ring updates, "
          f"{c['event']} events, {c['state']} states, {c['bytes']} bytes")
    if a.seconds > 0:
        print(f"rates: {c['sync'] / a.seconds:.1f} frames/s, {c['ring'] / a.seconds:.1f} rings/s, "
              f"{c['bytes'] / a.seconds:.0f} bytes/s")
    if a.no_keepalive:
        print(f"stream: first message {m.first - sub_at if m.first else -1:.2f} s, "
              f"last {m.last - sub_at if m.last else -1:.2f} s after the subscribe (span {span:.2f} s)")
    if a.text:
        print(m.text())
    if a.rings:
        for r in range(16):
            print(f"ring {r + 1:2d}: " + " ".join("%02x%02x%02x" % rgb for rgb in m.rings[r]))
    if a.png:
        m.png(a.png)
        print(f"wrote {a.png}")


if __name__ == "__main__":
    main()
