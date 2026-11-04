cmake_minimum_required(VERSION 3.20)
option(E16_HOST "Build the host test binary instead of the firmware" OFF)
# Host only: build the runtime with double rt_time_t, as on the device,
# instead of float (RT_TIME_FLOAT, rt_port.h). test/run_all.sh runs the
# specs and suites on both (docs/compat-notes.md).
option(E16_HOST_DOUBLE_TIME "Host build: double runtime times like the device (default float, like the mock)" OFF)
# Dev SysEx commands in console.c (virtual controls, screen dump, reset,
# corrupt-record -- see console.h): useful for remote testing over USB
# MIDI without a debugger attached, but they let anyone with a MIDI
# connection poke the UI and dump the screen. Default ON while this is
# still being bench-tested; a release build should set this OFF. The
# bootloader-reboot (0x05) and log-replay (0x61) SysEx stay built in
# either way.
option(E16_DEV_CONSOLE "Enable developer SysEx console commands (console.c); release builds should turn this OFF" ON)
# The mirror (src/mirror/mirror.h): a live view of the OLED, LED rings and
# inputs for a computer that subscribes over USB MIDI (tools/mirror/),
# plus remote control through the console's virtual controls. Independent
# of E16_DEV_CONSOLE, so release builds keep it; anything that can send
# MIDI to the E16 can then watch and drive it.
option(E16_MIRROR "Build in the mirror stream and its remote control (tools/mirror)" ON)
option(E16_TRS_AUDIO "Allow synth/app audio on TRS 2 (PWM). Off: the jack carries supply noise on this hardware, so TRS 2 stays MIDI" OFF)

# ChaN FatFs R0.15, vendored (external/fatfs/README.md); configured by
# src/disk/ffconf.h, so src/disk must be on the include path wherever ff.h is.
set(FATFS_SOURCES external/fatfs/source/ff.c external/fatfs/source/ffunicode.c)

if(E16_HOST)
    project(e16_launcher_host C)
    set(CMAKE_C_STANDARD 11)
    include(cmake/lua.cmake)
    add_executable(e16rt-host
        test/host/main.c
        test/host/port_host.c
        test/host/rt_module.c
        src/rt/rt.c
        src/rt/rt_api_out.c
        src/rt/rt_api_clock.c
        src/rt/rt_api_ctl.c
        src/rt/rt_api_var.c
        src/rt/rt_api_store.c
        src/rt/rt_api_draw.c
        src/rt/rt_api_ring.c
        src/rt/rt_api_launcher.c
        src/rt/rt_api_audio.c
        src/rt/rt_api_midiin.c
        src/rt/canvas.c
        src/ui/font.c)
    target_include_directories(e16rt-host PRIVATE src/rt src src/ui test/host)
    # RT_TIME_FLOAT: float runtime times, like the float32 mock (rt_port.h),
    # unless E16_HOST_DOUBLE_TIME asks for the device's double.
    target_compile_definitions(e16rt-host PRIVATE LUA_USE_MACOSX)
    if(NOT E16_HOST_DOUBLE_TIME)
        target_compile_definitions(e16rt-host PRIVATE RT_TIME_FLOAT)
    endif()
    target_link_libraries(e16rt-host PRIVATE lua m)

    # The block device over SPI flash, with a fake flash standing in
    # for the device's storage.h. Plain C, no Lua -- a separate executable
    # run directly by test/run_all.sh, not through e16rt-host.
    add_executable(blockdev_test test/disk/blockdev_test.c src/disk/blockdev.c src/disk/disk_fatal_host.c
        src/log.c)
    target_include_directories(blockdev_test PRIVATE src src/disk)
    target_compile_options(blockdev_test PRIVATE -Wall -Wextra)

    # The persistence reserve's name-keyed slot table, with the same
    # fake-flash pattern as blockdev_test above. Tests persist_slots.c's
    # record layer directly with raw byte payloads -- it doesn't need the
    # runtime (rt.h) persist.c binds it to on the device.
    add_executable(persist_test test/disk/persist_test.c src/disk/persist_slots.c
        src/disk/disk_fatal_host.c src/log.c)
    target_include_directories(persist_test PRIVATE src src/disk)
    target_compile_options(persist_test PRIVATE -Wall -Wextra)

    # The erase-rate budget both blockdev.c and persist_slots.c go
    # through on the device (flash_budget.h): a fake underlying
    # flash_ops_t and an injected fake clock, same pattern as the above.
    add_executable(flash_budget_test test/disk/flash_budget_test.c src/disk/flash_budget.c src/log.c)
    target_include_directories(flash_budget_test PRIVATE src src/disk)
    target_compile_options(flash_budget_test PRIVATE -Wall -Wextra)

    # The FAT volume (volume.c, diskio.c, vendored FatFs) over the block
    # device and an 11.5 MB fake flash, the same pattern as blockdev_test.
    add_executable(fatfs_test test/disk/fatfs_test.c src/disk/volume.c src/disk/diskio.c
        src/disk/blockdev.c src/disk/disk_fatal_host.c src/log.c ${FATFS_SOURCES})
    target_include_directories(fatfs_test PRIVATE src src/disk external/fatfs/source)
    target_compile_options(fatfs_test PRIVATE -Wall -Wextra)

    # The bounded retries, the Disk-mode bursts (the first boot's format)
    # and Disk mode's idle write-back (src/disk/disk_ops.c), over the
    # real block device, budget and volume on a fake flash and clock.
    add_executable(disk_ops_test test/disk/disk_ops_test.c src/disk/disk_ops.c src/disk/flash_budget.c
        src/disk/volume.c src/disk/diskio.c src/disk/blockdev.c src/disk/disk_fatal_host.c src/log.c
        ${FATFS_SOURCES})
    target_include_directories(disk_ops_test PRIVATE src src/disk external/fatfs/source)
    target_compile_options(disk_ops_test PRIVATE -Wall -Wextra)

    # The app api's file writes (src/apps/app_files.c): a rewrite either
    # goes through whole or is refused before anything changes, never a
    # 0-byte file after a reset; same fake flash and clock.
    add_executable(app_files_test test/apps/app_files_test.c src/apps/app_files.c src/disk/disk_ops.c
        src/disk/flash_budget.c src/disk/volume.c src/disk/diskio.c src/disk/blockdev.c
        src/disk/disk_fatal_host.c src/log.c ${FATFS_SOURCES})
    target_include_directories(app_files_test PRIVATE src src/apps src/disk external/fatfs/source)
    target_compile_options(app_files_test PRIVATE -Wall -Wextra)

    # The disk-script header parser (src/disk/scene_parse.c): a host test
    # that both exercises it directly with small in-memory scripts and
    # compares it against tools/embed_scripts.py's --dump-json for the
    # five external/oxi-e16-lua scripts. The JSON is generated by this
    # build (not checked in) so it can't drift from the Python code it's
    # compared against.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(SCENE_PARSE_DUMP_JSON ${CMAKE_BINARY_DIR}/scene_parse_dump.json)
    add_custom_command(OUTPUT ${SCENE_PARSE_DUMP_JSON}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_scripts.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/manifest.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/external/oxi-e16-lua
            ${CMAKE_BINARY_DIR}/scene_parse_unused_scripts_gen.c
            --dump-json ${SCENE_PARSE_DUMP_JSON}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_scripts.py ${CMAKE_CURRENT_SOURCE_DIR}/scripts/manifest.txt
        COMMENT "Dumping header-only scene JSON for scene_parse_test")
    add_custom_target(scene_parse_dump DEPENDS ${SCENE_PARSE_DUMP_JSON})
    add_executable(scene_parse_test test/disk/scene_parse_test.c src/disk/scene_parse.c)
    add_dependencies(scene_parse_test scene_parse_dump)
    target_include_directories(scene_parse_test PRIVATE src/disk src/rt)
    target_compile_options(scene_parse_test PRIVATE -Wall -Wextra)
    target_compile_definitions(scene_parse_test PRIVATE
        OXI_LUA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/external/oxi-e16-lua"
        SCENE_PARSE_DUMP_JSON="${SCENE_PARSE_DUMP_JSON}")

    # The scene provider (src/scenes.c): the menu's built-ins plus disk
    # scripts, over the real FAT volume on a fake flash, with the runtime's
    # loads stubbed by the test (Lua is linked only for lauxlib).
    add_executable(scenes_test test/disk/scenes_test.c src/scenes.c src/disk/scene_parse.c src/disk/volume.c
        src/disk/diskio.c src/disk/blockdev.c src/disk/disk_fatal_host.c src/log.c ${FATFS_SOURCES})
    target_include_directories(scenes_test PRIVATE src src/disk src/rt external/fatfs/source)
    target_compile_options(scenes_test PRIVATE -Wall -Wextra)
    target_link_libraries(scenes_test PRIVATE lua m)

    # The synth voice (pure DSP), tested on its own like blockdev_test.
    # -O2 so the timing report means something; the strict float flags
    # match the firmware build.
    add_executable(voice_test test/synth/voice_test.c src/synth/voice.c)
    target_include_directories(voice_test PRIVATE src/synth)
    target_compile_options(voice_test PRIVATE -O2 -Wall -Wextra)
    set_source_files_properties(src/synth/voice.c PROPERTIES
        COMPILE_OPTIONS "-Wfloat-conversion;-Wdouble-promotion")
    target_link_libraries(voice_test PRIVATE m)

    # A native app's audio stream (src/synth/app_stream.c): the double
    # buffering between its callback and the audio interrupt, the load
    # watch, the mix.
    add_executable(app_stream_test test/synth/app_stream_test.c src/synth/app_stream.c)
    target_include_directories(app_stream_test PRIVATE src/synth)
    target_compile_options(app_stream_test PRIVATE -O2 -Wall -Wextra -Wfloat-conversion -Wdouble-promotion)
    target_link_libraries(app_stream_test PRIVATE m)

    # Webcam mode's video synth (src/webcam/wc_render.c, portable C), with
    # the launcher's font for its text overlay.
    add_executable(wc_render_test test/webcam/wc_render_test.c src/webcam/wc_render.c src/ui/font.c)
    target_include_directories(wc_render_test PRIVATE src/webcam src/ui)
    target_compile_options(wc_render_test PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(wc_render_test PRIVATE m)
    # Its render-ahead bookkeeping (src/webcam/wc_ring.c): random
    # interleavings of the USB interrupt and the renderer.
    add_executable(wc_ring_test test/webcam/wc_ring_test.c src/webcam/wc_ring.c)
    target_include_directories(wc_ring_test PRIVATE src/webcam)
    target_compile_options(wc_ring_test PRIVATE -O2 -Wall -Wextra)

    # The mirror stream's core (src/mirror/mirror_core.c: packing, the
    # diffing scans, pacing, events, keepalive) against a fake USB queue.
    add_executable(mirror_test test/mirror/mirror_test.c src/mirror/mirror_core.c)
    target_include_directories(mirror_test PRIVATE src/mirror)
    target_compile_options(mirror_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

    # The native-app loader (.e16app images), tested on its own. Run with
    # the path of a cross-built app (build-apps/hello.e16app, see
    # apps/CMakeLists.txt) to check a real image too; test/run_all.sh does.
    add_executable(loader_test test/apps/loader_test.c src/apps/loader.c)
    target_include_directories(loader_test PRIVATE src/apps)
    target_compile_options(loader_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

    # The app-mode boot decision (src/apps/app_boot.c): when a "run this
    # app" record left in RAM is honoured.
    add_executable(app_boot_test test/apps/app_boot_test.c src/apps/app_boot.c)
    target_include_directories(app_boot_test PRIVATE src/apps)
    target_compile_options(app_boot_test PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

    # The leave prompt (Shift held alone 5 s, then encoder 1).
    add_executable(exit_prompt_test test/ui/exit_prompt_test.c src/ui/exit_prompt.c src/ui/font.c)
    target_include_directories(exit_prompt_test PRIVATE src/ui)
    target_compile_options(exit_prompt_test PRIVATE -Wall -Wextra)

    # The drone app's DSP (apps/drone/core: portable C, no api calls, no
    # libm): bounds and NaN fuzz, pitch, timing; --wav writes the demo.
    add_executable(drone_test test/drone/drone_test.c apps/drone/core/drone.c apps/drone/core/drone_save.c)
    target_include_directories(drone_test PRIVATE apps/drone/core)
    target_compile_options(drone_test PRIVATE -O2 -Wall -Wextra)
    set_source_files_properties(apps/drone/core/drone.c PROPERTIES
        COMPILE_OPTIONS "-Wfloat-conversion;-Wdouble-promotion")
    target_link_libraries(drone_test PRIVATE m)

    # x0x-lite (apps/x0x/core: portable C, GPL-3.0-only like the rest of
    # apps/x0x): pitch, kick, sequencer timing / swing / clock, edits,
    # TB-3PO, the saved state, bounds; --wav writes the demo.
    add_executable(x0x_test test/x0x/x0x_test.c apps/x0x/core/x0x.c apps/x0x/core/seq.c
        apps/x0x/core/pattern.c apps/x0x/core/tb3po.c apps/x0x/core/bass303.c
        apps/x0x/core/drum808.c apps/x0x/core/x0x_save.c)
    target_include_directories(x0x_test PRIVATE apps/x0x/core)
    target_compile_options(x0x_test PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(x0x_test PRIVATE m)
else()
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/external/E16-SDK/cmake/arm-none-eabi.cmake)
    project(e16_launcher C CXX ASM)
    if(NOT CMAKE_BUILD_TYPE)
        # Unoptimized firmware builds don't fit the 512 KB flash budget.
        set(CMAKE_BUILD_TYPE MinSizeRel)
    endif()
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    add_subdirectory(external/E16-SDK e16_sdk)
    include(cmake/lua.cmake)
    set(RT_SOURCES src/rt/rt.c src/rt/rt_api_out.c src/rt/rt_api_clock.c src/rt/rt_api_ctl.c
        src/rt/rt_api_var.c src/rt/rt_api_store.c src/rt/rt_heap.c
        src/rt/rt_api_draw.c src/rt/rt_api_ring.c src/rt/rt_api_launcher.c src/rt/rt_api_audio.c src/rt/rt_api_midiin.c
        src/rt/canvas.c)
    set(PORT_SOURCES src/port_e16.c src/port_e16_palette.c src/port_e16_canvas.c src/port_e16_audio.c
        src/port_e16_usbaudio.c)
    set(UI_SOURCES src/main.c src/console.c src/persist.c src/scenes.c src/ui/font.c src/ui/screen.c src/ui/exit_prompt.c
        src/ui/launcher.c src/noise_lab.c src/webcam/webcam.c src/webcam/wc_ring.c
        src/mirror/mirror.c src/mirror/mirror_core.c)
    # Webcam mode's video synth: line() runs in the USB service interrupt,
    # so it's optimized for speed (-O2 after the build type's -Os wins).
    set(WEBCAM_RENDER_SOURCES src/webcam/wc_render.c)
    # The block device and its SPI-flash backing (src/disk/flash_ops.h);
    # the persistence reserve's name-keyed slot table (persist_slots.c,
    # bound to the runtime by src/persist.c in UI_SOURCES) and the portable
    # fatal hook both this and blockdev.c use for an out-of-bounds flash
    # access (disk_fatal.h); the FAT volume (volume.c over FatFs, diskio.c
    # over the block device) and Disk mode (src/disk_mode.c, the USB
    # mass-storage side). FatFs itself (FATFS_SOURCES) is vendored code and
    # builds with the SDK's flags only, not the strict ones below. The
    # disk-script header parser (scene_parse.c) reads disk scripts' headers
    # for the scene list (src/scenes.c).
    set(DISK_SOURCES src/disk/blockdev.c src/disk/flash_e16.c src/disk/flash_budget.c src/disk/persist_slots.c
        src/disk/disk_fatal_e16.c src/disk/volume.c src/disk/diskio.c src/disk/disk_ops.c src/disk_mode.c
        src/disk/scene_parse.c)
    # The synth voice, rendered in the audio interrupt (src/port_e16_audio.c),
    # and a native app's own stream between its callback and that interrupt.
    set(SYNTH_SOURCES src/synth/voice.c src/synth/app_stream.c)
    # Native apps: the loader, the api table apps get, and the clean-boot
    # app mode that loads and runs them (src/apps/app_mode.h).
    set(APP_SOURCES src/apps/loader.c src/apps/api.c src/apps/app_files.c src/apps/app_mode.c src/apps/app_boot.c)

    # The embedded scripts: scripts/manifest.txt + external/oxi-e16-lua.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS scripts/manifest.txt)
    file(STRINGS scripts/manifest.txt MANIFEST_LINES REGEX "^[^#]*\\.lua")
    set(SCRIPT_FILES)
    foreach(line ${MANIFEST_LINES})
        string(REGEX MATCH "^[ \t]*([^ \t]+)" _ "${line}")
        # Copied first: the if(MATCHES) below resets CMAKE_MATCH_<n>, which
        # left only the directories as dependencies (an edited script
        # wasn't re-embedded).
        set(_file "${CMAKE_MATCH_1}")
        # A file with a '/' is relative to scripts/ (the launcher's own
        # demos), a bare name is in external/oxi-e16-lua (embed_scripts.py).
        if(_file MATCHES "/")
            list(APPEND SCRIPT_FILES ${CMAKE_CURRENT_SOURCE_DIR}/scripts/${_file})
        else()
            list(APPEND SCRIPT_FILES ${CMAKE_CURRENT_SOURCE_DIR}/external/oxi-e16-lua/${_file})
        endif()
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/scripts_gen.c
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_scripts.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/manifest.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/external/oxi-e16-lua
            ${CMAKE_BINARY_DIR}/scripts_gen.c
        DEPENDS tools/embed_scripts.py scripts/manifest.txt ${SCRIPT_FILES}
        COMMENT "Embedding scripts from scripts/manifest.txt")

    # The mirror's web page, kept on the disk (src/mirror/mirror_page.h).
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/mirror_page.c
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_file.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/mirror/index.html ${CMAKE_BINARY_DIR}/mirror_page.c mirror_page
        DEPENDS tools/embed_file.py tools/mirror/index.html
        COMMENT "Embedding tools/mirror/index.html")
    set(MIRROR_PAGE_SOURCES)
    if(E16_MIRROR)
        set(MIRROR_PAGE_SOURCES ${CMAKE_BINARY_DIR}/mirror_page.c)
    endif()

    # The launcher's own low-level pieces: the stack canary, newlib's heap
    # hook, the log, the RAM layout's linker-symbol accessors.
    set(CORE_SOURCES src/stack_guard.c src/sbrk.c src/log.c src/mem_layout.c src/usb_functions.c)

    # The USB drive, audio input and camera are the SDK's examples
    # (examples/usb_disk, usb_tone, usb_cam), built unchanged except that
    # their usb_add_function() goes to src/usb_functions.c, which fits the
    # three into the SDK's two function slots. TinyUSB's classes for them
    # are switched on in src/tusb_config_app.h.
    set(SDK_EXAMPLES ${CMAKE_CURRENT_SOURCE_DIR}/external/E16-SDK/examples)
    set(USB_FUNCTION_SOURCES ${SDK_EXAMPLES}/usb_disk/usb_msc.c ${SDK_EXAMPLES}/usb_tone/usb_audio.c
        ${SDK_EXAMPLES}/usb_cam/usb_video.c)
    set_source_files_properties(${USB_FUNCTION_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS usb_add_function=launcher_usb_add_function)
    set(TUSB_CLASS_SOURCES ${E16_TINYUSB_DIR}/class/msc/msc_device.c ${E16_TINYUSB_DIR}/class/audio/audio_device.c
        ${E16_TINYUSB_DIR}/class/video/video_device.c)
    e16_add_firmware(launcher ${CORE_SOURCES}
        ${UI_SOURCES} ${RT_SOURCES} ${PORT_SOURCES} ${DISK_SOURCES} ${SYNTH_SOURCES} ${APP_SOURCES} external/tlsf/tlsf.c
        ${WEBCAM_RENDER_SOURCES} ${USB_FUNCTION_SOURCES} ${TUSB_CLASS_SOURCES}
        ${CMAKE_BINARY_DIR}/scripts_gen.c ${MIRROR_PAGE_SOURCES} ${FATFS_SOURCES}
        USB_NAME "E16 Launcher")

    # Safety gate: OXI's only hard rule is that nothing but OXI's own
    # bootloader writes the E16's internal (MCU) flash at runtime
    # (docs/safety.md). This fails the build if any internal-
    # flash HAL symbol actually linked into launcher.elf, or any raw
    # FLASH register access in src/ (tools/check_no_flash_writes.sh).
    # The SDK's HAL_FLASH module may still be compiled into e16_bsp;
    # --gc-sections (arm-none-eabi.cmake) is what keeps an unused
    # symbol there from ever reaching the check in the first place.
    add_custom_command(TARGET launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E env
            bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_flash_writes.sh
                $<TARGET_FILE:launcher> ${CMAKE_NM} ${CMAKE_CURRENT_SOURCE_DIR}/src
        VERBATIM
        COMMENT "Checking no internal-flash write path is linked into launcher"
    )
    # The RAM layout (src/mem_layout.h): nothing in the CCM may reach the
    # reset-surviving records at its top, and the normal-mode stack keeps
    # its headroom. Also prints the app-mode capacity.
    add_custom_command(TARGET launcher POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mem_layout.py
            $<TARGET_FILE:launcher> ${CMAKE_NM} ${CMAKE_CURRENT_SOURCE_DIR}/src
        VERBATIM
        COMMENT "Checking the RAM layout against src/mem_layout.h"
    )
    target_include_directories(launcher PRIVATE src src/rt
        ${SDK_EXAMPLES}/usb_disk ${SDK_EXAMPLES}/usb_tone ${SDK_EXAMPLES}/usb_cam src/ui src/disk src/synth src/apps src/webcam src/mirror
        external/tlsf
        external/fatfs/source)
    target_compile_definitions(launcher PRIVATE E16_DEV_CONSOLE=$<BOOL:${E16_DEV_CONSOLE}>
        E16_TRS_AUDIO=$<BOOL:${E16_TRS_AUDIO}> E16_MIRROR=$<BOOL:${E16_MIRROR}>)
    target_link_libraries(launcher PRIVATE lua m)
    # Lua prints floats with %g: newlib-nano needs this to support it.
    target_link_options(launcher PRIVATE -u _printf_float)
    # The runtime, the port and the UI build warning-clean for the device too
    # (rt_time_t is double here, float on the host).
    set_source_files_properties(${CORE_SOURCES} ${RT_SOURCES} ${PORT_SOURCES} ${UI_SOURCES} ${DISK_SOURCES}
        ${APP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-Wall;-Wextra;-Wfloat-conversion;-Wdouble-promotion")
    # The voice runs in the audio interrupt (port_e16_audio.c): optimize it for speed
    # rather than size (-O2 after the build type's -Os wins).
    set_source_files_properties(${SYNTH_SOURCES} ${WEBCAM_RENDER_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-Wall;-Wextra;-Wfloat-conversion;-Wdouble-promotion;-O2")
endif()
