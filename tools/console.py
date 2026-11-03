#!/usr/bin/env python3
"""The launcher's USB SysEx console (protocol in src/console.h).

  console.py                 follow the log until Ctrl-C
  console.py --once          exit after the first line (or after 5 s)

Development aids (only in firmware built with E16_DEV_CONSOLE=ON, the
default; a release build turns it off and ignores these):

  console.py screen          print the OLED framebuffer as text art
  console.py turn N STEPS    turn encoder N (1-16) by STEPS (-63..63)
  console.py press N         press and release encoder N (1-16)
  console.py down N | up N   press / release encoder N (N = shift for Shift)
  console.py shift-tap       tap Shift (opens the page picker)
  console.py shift-hold [S]  hold Shift S seconds (default 1.3: back to menu)
  console.py pwm             sample TRS 2's PWM duty 50 times over 100 ms
                             (the synth's activity check); prints the
                             "console: pwm ..." log line it answers with
  console.py reset           reboot back into the launcher (emulates a
                             power cycle, without the bootloader's reflash
                             wait; persisted vars/store reload from flash)
  console.py corrupt INDEX   corrupt scenes[INDEX]'s saved vars record on
                             flash (persist.h's dev aid; the next start
                             of that script logs "bad record")

  The synth without a script (a script load or unload releases it):
  console.py synth note N [VEL]      play note N (VEL default 100; 0 = off)
  console.py synth off               release it (the synth stops)
  console.py synth set NAME V        cutoff resonance envmod decay waveform
                                     volume glide slide, 0-127
  console.py synth output WHERE      auto, usb, trs2 or both

  The noise lab (src/noise_lab.h; settings last until a power cycle or
  "noise reset"):
  console.py noise status            log the current settings
  console.py noise oled-contrast N   0-255 (default 143)
  console.py noise oled-off | oled-on
  console.py noise oled-clock N      SSD1306 0xD5 byte (default 0xF0)
  console.py noise oled-precharge N  0xD9 byte (default 0x22)
  console.py noise oled-vcomh N      0xDB byte (default 0x35)
  console.py noise leds-off | leds-on
  console.py noise led-brightness N  0-255 (255 = as drawn)
  console.py noise usb-quiet | usb-loud   hold the log back / let it go
  console.py noise reset             everything back to the defaults
  (N may be decimal or 0x hex.) Each prints the "noise: ..." line it
  answers with, except under usb-quiet.

Each log line may arrive as several SysEx messages (the firmware chunks a
line to <=120 bytes of payload per message, '\\n' included), so this
concatenates the payloads and only prints on '\\n'.
"""
import sys, time
import mido

HDR = [0x7D, 0x45]


def ports():
    # The launcher's port ("E16 Launcher"), not the bootloader's "OXI E16 FW Update".
    def mine(n):
        return "E16" in n and "FW Update" not in n
    iname = next((n for n in mido.get_input_names() if mine(n)), None)
    oname = next((n for n in mido.get_output_names() if mine(n)), None)
    if not iname or not oname:
        sys.exit("no E16 MIDI port")
    return iname, oname


def follow_log(once):
    iname, oname = ports()
    start = time.time()
    try:
        port = mido.open_input(iname)
    except (IOError, OSError) as e:
        sys.exit(f"could not open E16 MIDI input {iname!r}: {e}")
    with port:
        try:
            with mido.open_output(oname) as out:
                out.send(mido.Message("sysex", data=HDR + [0x61]))
        except (IOError, OSError) as e:
            sys.exit(f"could not open E16 MIDI output {oname!r}: {e}")
        pending = ""
        while True:
            for m in port.iter_pending():
                if m.type == "sysex" and list(m.data[:3]) == HDR + [0x60]:
                    pending += bytes(m.data[3:]).decode("ascii", "replace")
                    while "\n" in pending:
                        line, pending = pending.split("\n", 1)
                        print(line, flush=True)
                        if once:
                            return
            if once and time.time() - start > 5:
                sys.exit("no log line within 5 s")
            time.sleep(0.005)


def control(out, ctl, act, n=64):
    out.send(mido.Message("sysex", data=HDR + [0x10, ctl, act, n]))
    time.sleep(0.04)  # longer than a main-loop pass, so edges aren't merged


def enc_index(arg):
    if arg == "shift":
        return 16
    n = int(arg)
    if not 1 <= n <= 16:
        sys.exit("encoder 1-16")
    return n - 1


def screen():
    iname, oname = ports()
    fb = [[0] * 128 for _ in range(8)]
    got = set()
    with mido.open_input(iname) as inp, mido.open_output(oname) as out:
        out.send(mido.Message("sysex", data=HDR + [0x11]))
        start = time.time()
        while len(got) < 16 and time.time() - start < 3:
            for m in inp.iter_pending():
                d = list(m.data) if m.type == "sysex" else []
                if d[:3] == HDR + [0x62] and len(d) == 5 + 128:
                    page, half = d[3], d[4]
                    for i in range(64):
                        fb[page][half * 64 + i] = (d[5 + 2 * i] << 4) | d[6 + 2 * i]
                    got.add((page, half))
            time.sleep(0.002)
    if len(got) < 16:
        sys.exit(f"incomplete framebuffer dump ({len(got)}/16)")
    for y in range(64):
        print("".join("#" if fb[y >> 3][x] >> (y & 7) & 1 else "." for x in range(128)))


def pwm():
    iname, oname = ports()
    with mido.open_input(iname) as inp:
        with mido.open_output(oname) as out:
            out.send(mido.Message("sysex", data=HDR + [0x12]))
        pending, start = "", time.time()
        while time.time() - start < 3:
            for m in inp.iter_pending():
                if m.type == "sysex" and list(m.data[:3]) == HDR + [0x60]:
                    pending += bytes(m.data[3:]).decode("ascii", "replace")
                    while "\n" in pending:
                        line, pending = pending.split("\n", 1)
                        if line.startswith("console: pwm"):
                            print(line)
                            return
            time.sleep(0.005)
    sys.exit("no pwm answer within 3 s")


SYNTH_PARAMS = ["cutoff", "resonance", "envmod", "decay", "waveform", "volume", "glide", "slide"]
SYNTH_OUTPUTS = ["auto", "usb", "trs2", "both"]
NOISE_CMDS = {
    "status": 0, "oled-contrast": 1, "oled-off": 2, "oled-on": 3, "oled-clock": 4,
    "oled-precharge": 5, "oled-vcomh": 6, "leds-off": 7, "leds-on": 8,
    "led-brightness": 9, "usb-quiet": 10, "usb-loud": 11, "reset": 12,
}


def send_and_wait(data, prefix, timeout=2.0):
    """Send one SysEx and print the first log line starting with prefix."""
    iname, oname = ports()
    with mido.open_input(iname) as inp:
        with mido.open_output(oname) as out:
            out.send(mido.Message("sysex", data=HDR + data))
        pending, start = "", time.time()
        while time.time() - start < timeout:
            for m in inp.iter_pending():
                if m.type == "sysex" and list(m.data[:3]) == HDR + [0x60]:
                    pending += bytes(m.data[3:]).decode("ascii", "replace")
                    while "\n" in pending:
                        line, pending = pending.split("\n", 1)
                        if line.startswith(prefix):
                            print(line)
                            return True
            time.sleep(0.005)
    return False


def synth(a):
    if not a:
        sys.exit(__doc__)
    if a[0] == "note":
        op, x, y = 0, int(a[1], 0), int(a[2], 0) if len(a) > 2 else 100
    elif a[0] == "off":
        op, x, y = 3, 0, 0
    elif a[0] == "set":
        if a[1] not in SYNTH_PARAMS:
            sys.exit("synth set: one of " + " ".join(SYNTH_PARAMS))
        op, x, y = 1, SYNTH_PARAMS.index(a[1]), int(a[2], 0)
    elif a[0] == "output":
        if a[1] not in SYNTH_OUTPUTS:
            sys.exit("synth output: one of " + " ".join(SYNTH_OUTPUTS))
        op, x, y = 2, SYNTH_OUTPUTS.index(a[1]), 0
    else:
        sys.exit(__doc__)
    if not (0 <= x <= 127 and 0 <= y <= 127):
        sys.exit("synth: values 0-127")
    if not send_and_wait([0x13, op, x, y], "console: synth"):
        sys.exit("no answer within 2 s")


def noise(a):
    if not a or a[0] not in NOISE_CMDS:
        sys.exit("noise: one of " + " ".join(NOISE_CMDS))
    v = int(a[1], 0) if len(a) > 1 else 0
    if not 0 <= v <= 255:
        sys.exit("noise: value 0-255")
    quiet = a[0] == "usb-quiet"
    if not send_and_wait([0x14, NOISE_CMDS[a[0]], v >> 7, v & 0x7F], "noise:", 0.5 if quiet else 2.0) and not quiet:
        sys.exit("no answer within 2 s")


def main():
    a = sys.argv[1:]
    if not a or a[0] == "--once":
        follow_log("--once" in a)
        return
    if a[0] == "screen":
        screen()
        return
    if a[0] == "pwm":
        pwm()
        return
    if a[0] == "synth":
        synth(a[1:])
        return
    if a[0] == "noise":
        noise(a[1:])
        return
    _, oname = ports()
    with mido.open_output(oname) as out:
        cmd = a[0]
        if cmd == "turn":
            steps = max(-63, min(63, int(a[2])))
            control(out, enc_index(a[1]), 2, 64 + steps)
        elif cmd == "press":
            i = enc_index(a[1])
            control(out, i, 1)
            control(out, i, 0)
        elif cmd == "down":
            control(out, enc_index(a[1]), 1)
        elif cmd == "up":
            control(out, enc_index(a[1]), 0)
        elif cmd == "shift-tap":
            control(out, 16, 1)
            control(out, 16, 0)
        elif cmd == "shift-hold":
            control(out, 16, 1)
            time.sleep(float(a[1]) if len(a) > 1 else 1.3)
            control(out, 16, 0)
        elif cmd == "reset":
            out.send(mido.Message("sysex", data=HDR + [0x06]))
        elif cmd == "corrupt":
            out.send(mido.Message("sysex", data=HDR + [0x07, int(a[1])]))
        else:
            sys.exit(__doc__)


if __name__ == "__main__":
    main()
