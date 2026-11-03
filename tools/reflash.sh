#!/bin/bash
# Reboot a running firmware into the bootloader over SysEx (F0 7D 45 05 F7,
# answered by the launcher), then upload a firmware with the SDK's
# e16_upload.py. Needs Python 3 with mido (and python-rtmidi).
# Usage: tools/reflash.sh [firmware.bin]   (default: build/launcher.bin)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="${1:-$ROOT/build/launcher.bin}"
has_port() { python3 -c "import mido,sys; sys.exit(0 if any('$1' in p for p in mido.get_output_names()) else 1)"; }
# The running firmware's port: E16_PORT if set, else any port with "E16" in
# its name that isn't the bootloader's ("E16 Launcher", "E16 Custom", ...).
app_port() { E16_PORT="${E16_PORT:-}" python3 -c "
import mido, os
want = os.environ['E16_PORT']
ports = [p for p in mido.get_output_names() if 'FW Update' not in p and (want in p if want else 'E16' in p)]
print(ports[0] if ports else '')"; }

if ! has_port "FW Update"; then
    APP_PORT="$(app_port)" python3 -c "
import mido
import os
name = os.environ.get('APP_PORT') or None
if name:
    mido.open_output(name).send(mido.Message('sysex', data=[0x7D, 0x45, 0x05]))
"
    for _ in $(seq 1 40); do has_port "FW Update" && break; sleep 0.5; done
fi
has_port "FW Update" || { echo "No bootloader: hold encoders 15+16 and press Shift to power on."; exit 1; }
sleep 0.5
python3 "$ROOT/external/E16-SDK/tools/e16_upload.py" upload "$FW" | tail -1
for _ in $(seq 1 20); do p="$(app_port)"; [ -n "$p" ] && { echo "$p is up"; exit 0; }; sleep 0.5; done
echo "Uploaded; the firmware doesn't show up on USB MIDI (fine if it doesn't use USB)."
