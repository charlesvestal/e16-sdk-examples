#!/bin/sh
# Serve the E16 mirror page (index.html, next to this script) on localhost.
# Web MIDI needs a secure page, and http://localhost counts as one.
# Usage: tools/mirror/serve.sh [port]   (default 8016)
PORT="${1:-8016}"
cd "$(dirname "$0")" || exit 1
echo "E16 mirror: open http://localhost:$PORT/ in Chrome or Edge (Ctrl-C to stop)"
exec python3 -m http.server "$PORT" --bind 127.0.0.1
