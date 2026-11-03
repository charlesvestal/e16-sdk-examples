# e16_add_app(<name> <sources>...)
#
# Builds a native E16 app: compiles the sources position-independent
# (GOT through r9), links them with apps/sdk/app.ld and the entry stub
# crt0.c, and turns the ELF into <name>.e16app with tools/mkapp.py, which
# also refuses relocations the loader couldn't redo. Outputs land in
# ${CMAKE_BINARY_DIR}/ : <name>.elf, <name>.e16app, <name>.map.
#
# Use from a project configured with the arm-none-eabi toolchain file
# (apps/CMakeLists.txt).

set(E16_APP_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(E16_APP_SDK_DIR ${E16_APP_SDK_DIR} ABSOLUTE)
get_filename_component(E16_APP_REPO_DIR ${E16_APP_SDK_DIR}/../.. ABSOLUTE)
set(E16_MKAPP ${E16_APP_REPO_DIR}/tools/mkapp.py)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(E16_APP_C_FLAGS
    -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
    -fpic -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative
    -ffreestanding -fno-common -ffunction-sections -fdata-sections
    -fno-tree-loop-distribute-patterns
    -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
    -Os -Wall -Wextra)

# -fno-tree-loop-distribute-patterns: no libc, so the compiler mustn't
# turn a plain loop into a call to memset() or memcpy().

function(e16_add_app name)
    set(elf ${name}.elf)
    add_executable(${elf} ${ARGN} ${E16_APP_SDK_DIR}/crt0.c)
    target_include_directories(${elf} PRIVATE ${E16_APP_SDK_DIR} ${E16_APP_REPO_DIR}/src/apps)
    target_compile_options(${elf} PRIVATE ${E16_APP_C_FLAGS})
    # No libc: -nostdlib, then only libgcc (apps/CMakeLists.txt also
    # drops the toolchain file's newlib-nano link flags). --emit-relocs
    # keeps the relocations in the final ELF for mkapp.py to check.
    set_target_properties(${elf} PROPERTIES LINK_DEPENDS ${E16_APP_SDK_DIR}/app.ld)
    target_link_options(${elf} PRIVATE
        -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
        -nostdlib -nostartfiles
        -T${E16_APP_SDK_DIR}/app.ld
        -Wl,--emit-relocs -Wl,--gc-sections -Wl,--no-undefined
        -Wl,-Map=${CMAKE_BINARY_DIR}/${name}.map)
    target_link_libraries(${elf} PRIVATE gcc)

    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${name}.e16app
        COMMAND ${Python3_EXECUTABLE} ${E16_MKAPP} --name ${name}
            $<TARGET_FILE:${elf}> ${CMAKE_BINARY_DIR}/${name}.e16app
        DEPENDS ${elf} ${E16_MKAPP}
        COMMENT "mkapp: ${name}.e16app")
    add_custom_target(${name} ALL DEPENDS ${CMAKE_BINARY_DIR}/${name}.e16app)
endfunction()
