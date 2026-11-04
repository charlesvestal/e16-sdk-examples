#!/bin/bash
# Run every host test: the runtime specs (test/rt/*_spec.lua) and the
# oxi-e16-lua suites (test/run_suites.sh), on each host build that exists.
# Builds nothing; build first:
#
#   cmake -S . -B build-host -DE16_HOST=ON && cmake --build build-host
#   cmake -S . -B build-host-double -DE16_HOST=ON -DE16_HOST_DOUBLE_TIME=ON \
#       && cmake --build build-host-double      # optional: device-style double times
#
# build-host (float times, like the mock) is required; build-host-double is
# run too if present. Override with HOSTS="path/to/e16rt-host ...".
# Exits nonzero if any spec or suite fails, or a required binary is missing.
#
# One exception to "builds nothing": if arm-none-eabi-gcc is on the PATH,
# the native apps (apps/, into build-apps/) are built first so loader_test
# can check the real cross-built images (hello, plasma, player and the test apps); without the
# toolchain that part of loader_test is skipped with a note.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

APP_IMAGES=("$ROOT/build-apps/hello.e16app" "$ROOT/build-apps/plasma.e16app" "$ROOT/build-apps/player.e16app" "$ROOT/build-apps/fault.e16app"
    "$ROOT/build-apps/flashpoke.e16app" "$ROOT/build-apps/nullpoke.e16app" "$ROOT/build-apps/filewrite.e16app" "$ROOT/build-apps/drone.e16app" "$ROOT/build-apps/audiostress.e16app"
    "$ROOT/build-apps/x0x.e16app")
if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    if ! { cmake -S apps -B build-apps >/dev/null && cmake --build build-apps >/dev/null; }; then
        echo "apps build FAILED (cmake -S apps -B build-apps && cmake --build build-apps)"
        exit 1
    fi
else
    echo "note: arm-none-eabi-gcc not found; loader_test skips the real-app check"
fi
SPECS="out ctl clock var_store harness draw ring audio midiin demos reader"

if [ -z "$HOSTS" ]; then
    HOSTS="$ROOT/build-host/e16rt-host"
    if [ -x "$ROOT/build-host-double/e16rt-host" ]; then
        HOSTS="$HOSTS $ROOT/build-host-double/e16rt-host"
    else
        echo "note: build-host-double not built; skipping the double-time run"
    fi
fi

status=0
for host in $HOSTS; do
    echo "######## $host"
    if [ ! -x "$host" ]; then
        echo "missing: $host (build it first)"
        status=1
        continue
    fi
    for s in $SPECS; do
        out="$("$host" "test/rt/${s}_spec.lua" 2>&1)" && rc=0 || rc=$?
        last="$(printf '%s\n' "$out" | tail -1)"
        echo "== ${s}_spec: $last"
        if [ $rc -ne 0 ] || [ "$last" != "ALL PASSED" ]; then
            printf '%s\n' "$out" | grep -E "^FAIL|rror" | head -20
            status=1
        fi
    done
    HOST="$host" bash "$ROOT/test/run_suites.sh" || status=1

    # Plain-C host tests (not Lua specs run through e16rt-host): the block
    # device, the persistence slot table, the erase-rate budget, the FAT
    # volume, the bounded disk retries and bursts, the app api's file
    # writes, the disk-script header parser (CMake dumps the JSON it
    # compares against from embed_scripts.py before this test is built),
    # the scene provider, the synth voice, the mirror stream's core, webcam mode's video synth and its render-ahead bookkeeping, the drone and x0x engines, the app loader (given the
    # cross-built app, if any) and the app-mode boot decision. They don't depend on RT_TIME_FLOAT/double; each runs once per
    # build dir that has it.
    for ctest in blockdev_test persist_test flash_budget_test fatfs_test disk_ops_test app_files_test scene_parse_test scenes_test voice_test app_stream_test drone_test x0x_test wc_render_test wc_ring_test exit_prompt_test mirror_test loader_test app_boot_test; do
        bin="$(dirname "$host")/$ctest"
        args=()
        [ "$ctest" = loader_test ] && args=("${APP_IMAGES[@]}")
        if [ -x "$bin" ]; then
            out="$("$bin" "${args[@]}" 2>&1)" && rc=0 || rc=$?
            printf '%s\n' "$out" | grep -E "^(note|real app)" | sed "s/^/   $ctest: /"
            last="$(printf '%s\n' "$out" | tail -1)"
            echo "== $ctest: $last"
            if [ $rc -ne 0 ] || [ "$last" != "ALL PASSED" ]; then
                printf '%s\n' "$out" | grep -E "^FAIL|rror" | head -20
                status=1
            fi
        else
            echo "missing: $bin (build it first)"
            status=1
        fi
    done
done

if [ $status -eq 0 ]; then
    echo "ALL HOST TESTS PASSED"
else
    echo "SOME HOST TESTS FAILED"
fi
exit $status
