# Native E16 apps (.e16app). A standalone project, cross-compiled:
#
#   cmake -S apps -B build-apps && cmake --build build-apps
#
# Outputs build-apps/<name>.e16app; copy them to /apps on the E16's disk.
cmake_minimum_required(VERSION 3.20)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/../external/E16-SDK/cmake/arm-none-eabi.cmake)
project(e16_apps C)
set(CMAKE_C_STANDARD 11)
# The toolchain file links firmware against newlib-nano (--specs=...);
# apps link against libgcc only (sdk/cmake/e16app.cmake).
set(CMAKE_EXE_LINKER_FLAGS "")
include(sdk/cmake/e16app.cmake)

e16_add_app(hello demos/hello/hello.c)
e16_add_app(plasma demos/plasma/plasma.c)
# .e16v animation player (tools/gif2e16.py makes the files); borrows the
# launcher's own 5x7 font (src/ui/font.c is freestanding).
e16_add_app(player demos/player/player.c ${CMAKE_CURRENT_LIST_DIR}/../src/ui/font.c)
target_include_directories(player.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src/ui)
# Drone: an evolving drone synth on the app audio stream (docs/drone.md);
# the DSP in drone/core is portable C, host-tested by test/drone/drone_test.c,
# and runs in the audio interrupt: built for speed.
e16_add_app(drone drone/app.c drone/core/drone.c drone/core/drone_save.c ${CMAKE_CURRENT_LIST_DIR}/../src/ui/font.c)
target_include_directories(drone.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src/ui ${CMAKE_CURRENT_LIST_DIR}/drone/core)
set_source_files_properties(drone/core/drone.c PROPERTIES COMPILE_OPTIONS "-O2")
# Test apps (docs/hw-acceptance.md): faults on purpose; pokes the
# internal-flash controller (app mode's MPU guard must stop it); writes
# through a NULL pointer (the MPU's NULL guard must stop it); writes a
# file twice and reads it back; overloads and hangs its audio callback.
e16_add_app(fault tests/fault/fault.c)
e16_add_app(flashpoke tests/flashpoke/flashpoke.c)
e16_add_app(nullpoke tests/nullpoke/nullpoke.c)
e16_add_app(filewrite tests/filewrite/filewrite.c)
# The app audio stream's guards: overload stop, restart, stuck callback.
e16_add_app(audiostress tests/audiostress/audiostress.c)
# x0x-lite: an 808 kit and a 303 with TB-3PO on a 16-step sequencer, ported
# from X0X (docs/x0x.md). GPL-3.0-only, unlike the rest of this repository
# (apps/x0x/LICENSING.md). The engine in x0x/core is portable C, host-tested
# by test/x0x/x0x_test.c, and runs in the audio interrupt: built for speed.
set(X0X_CORE x0x/core/x0x.c x0x/core/seq.c x0x/core/pattern.c x0x/core/tb3po.c
    x0x/core/bass303.c x0x/core/drum808.c x0x/core/x0x_save.c)
e16_add_app(x0x x0x/app.c x0x/gfx.c ${X0X_CORE})
target_include_directories(x0x.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/x0x ${CMAKE_CURRENT_LIST_DIR}/x0x/core)
set_source_files_properties(x0x/core/x0x.c x0x/core/seq.c x0x/core/bass303.c x0x/core/drum808.c
    PROPERTIES COMPILE_OPTIONS "-O2")
# x0xbench: x0x-lite's engines timed on the device (cycles per sample), a dev aid.
e16_add_app(x0xbench x0x/bench.c ${X0X_CORE})
target_include_directories(x0xbench.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/x0x/core)
