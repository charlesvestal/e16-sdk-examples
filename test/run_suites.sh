#!/bin/bash
# Run oxi-e16-lua's suites against the native runtime: a scratch dir whose
# test/e16mock.lua is our shim (test/host/e16mock.lua) and whose scripts and
# tests are the originals, unmodified. chords_test.lua also reads the voiced
# chord sets from build/chordgen/sets, which oxi-e16-lua's own build.py
# writes (offline: --no-reference skips the downloaded reference sets).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OXI="$ROOT/external/oxi-e16-lua"
HOST="${HOST:-$ROOT/build-host/e16rt-host}"
SUITES="euclid lfo chords modseq noteseq tb3po"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/test"
cp "$OXI"/*.lua "$W/"
cp "$OXI"/test/*_test.lua "$W/test/"
cp "$ROOT/test/host/e16mock.lua" "$W/test/e16mock.lua"
python3 "$OXI/tools/chordgen/build.py" --no-reference --out "$W/build/chordgen" >/dev/null
status=0
for s in $SUITES; do
    echo "== $s"
    out="$(cd "$W" && "$HOST" "test/${s}_test.lua" 2>&1)" && rc=0 || rc=$?
    if [ $rc -ne 0 ] || [ "$(printf '%s\n' "$out" | tail -1)" != "ALL PASSED" ]; then
        printf '%s\n' "$out" | grep -E "^(FAIL|  )|rror" | head -20
        status=1
    fi
    printf '%s\n' "$out" | tail -1
done
exit $status
