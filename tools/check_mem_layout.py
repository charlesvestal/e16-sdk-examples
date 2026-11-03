#!/usr/bin/env python3
"""Check the linked firmware against src/mem_layout.h (run after every build).

  check_mem_layout.py <launcher.elf> <nm> <src-dir>

Fails the build if
  - the .ccmram section (its size from the ELF's section headers, via
    readelf next to nm) reaches the reset-surviving records at the top of
    the CCM (the linker only knows the whole 64 KB),
  - an object file not on CCM_ALLOWED below put anything in .ccmram (from
    the link map, launcher.map next to the ELF): .ccmram is normal-mode
    only (mem_layout.h) -- app mode gives the whole CCM to the app, so
    nothing app mode runs may keep state there, or
  - the normal-mode stack above the Lua SRAM pool, newlib's heap and the
    canary is smaller than MIN_STACK_BYTES.
Prints the CCM use, the normal-mode stack room and the app-mode capacity
(text: SRAM from _ebss to the app stack, less the api's buffers, which
the device's "app: ..." log line accounts for exactly; data: the CCM
below the records).
"""
import os
import re
import subprocess
import sys

# The objects allowed to place symbols in .ccmram (CCM_BSS, ccm.h): code
# that only runs in normal mode. Anything app mode uses (the api, app
# mode itself, the block device, the volume, FatFs, the flash budget, the
# drivers, the console, the log) must keep its state in SRAM.
CCM_ALLOWED = {
    "src/scenes.c",
    "src/ui/screen.c",
    "src/rt/rt_api_store.c",
    "src/port_e16.c",
    "src/port_e16_canvas.c",
    "src/disk/persist_slots.c",
}


def section(readelf, elf, name):
    """(address, size) of a section, from the ELF's section headers."""
    out = subprocess.run([readelf, "-S", "-W", elf], check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)", line)
        if m and m.group(1) == name:
            return int(m.group(2), 16), int(m.group(3), 16)
    return None


def ccm_objects(map_path):
    """The object files the link map shows contributing to .ccmram."""
    objs = []
    in_ccm = False
    with open(map_path) as f:
        for line in f:
            if line.startswith(".ccmram"):
                in_ccm = True
                continue
            if in_ccm and re.match(r"^\.\S", line):
                break
            m = re.match(r"^\s*\.ccmram\S*\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S+)", line)
            if in_ccm and m and int(m.group(1), 16) > 0:
                objs.append(m.group(2))
    return objs


def source_of(obj):
    """CMakeFiles/launcher.dir/src/ui/screen.c.obj -> src/ui/screen.c"""
    m = re.search(r"launcher\.dir/(.*)\.obj$", obj)
    return m.group(1) if m else obj


def defines(path):
    out = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)u?\b", line)
            if m:
                out[m.group(1)] = int(m.group(2), 0)
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elf, nm, src = sys.argv[1:]
    d = defines(f"{src}/mem_layout.h")
    d.update(defines(f"{src}/stack_guard.h"))
    ccm_base, ccm_size = d["CCM_BASE"], d["CCM_SIZE"]
    records = ccm_base + ccm_size - d["CCM_RECORDS_SIZE"]

    readelf = re.sub(r"nm(\.exe)?$", r"readelf\1", nm)
    sec = section(readelf, elf, ".ccmram")
    if sec is None:
        sys.exit(f"check_mem_layout: no .ccmram section in {elf} ({readelf} -S)")
    sec_addr, sec_size = sec
    if sec_addr != ccm_base:
        sys.exit(f"check_mem_layout: .ccmram at 0x{sec_addr:08x}, not CCM_BASE 0x{ccm_base:08x}")
    ccm_end = sec_addr + sec_size

    map_path = os.path.join(os.path.dirname(elf), "launcher.map")
    if not os.path.exists(map_path):
        sys.exit(f"check_mem_layout: no link map at {map_path}")
    objs = ccm_objects(map_path)
    if not objs and sec_size > 0:
        sys.exit(f"check_mem_layout: .ccmram is {sec_size} bytes but {map_path} lists nothing in it")
    stray = sorted({source_of(o) for o in objs} - CCM_ALLOWED)

    syms = {}
    lines = subprocess.run([nm, "-S", "--defined-only", elf], check=True, capture_output=True,
                           text=True).stdout.splitlines()
    for line in lines:
        f = line.split()
        if len(f) == 4:
            addr, size, name = int(f[0], 16), int(f[1], 16), f[3]
        elif len(f) == 3:
            addr, size, name = int(f[0], 16), 0, f[2]
        else:
            continue
        syms[name] = addr

    ok = True
    if ccm_end > records:
        print(f"check_mem_layout: .ccmram ends at 0x{ccm_end:08x}, past the boot records "
              f"at 0x{records:08x}: grow CCM_STATIC_BYTES in src/mem_layout.h", file=sys.stderr)
        ok = False
    if stray:
        print(f"check_mem_layout: {', '.join(stray)} put symbols in .ccmram, which is normal-mode only "
              f"(mem_layout.h): app mode hands the CCM to the app. Move them to SRAM, or, if they're "
              f"normal-mode only, add them to CCM_ALLOWED in tools/check_mem_layout.py", file=sys.stderr)
        ok = False

    ebss, estack = syms["_ebss"], syms["_estack"]
    free = (ebss + 7) & ~7
    reserve = d["STACK_GUARD_NEWLIB_HEAP"] + d["STACK_GUARD_SIZE"]
    stack = estack - (free + d["LUA_SRAM_POOL_BYTES"] + reserve)
    if stack < d["MIN_STACK_BYTES"]:
        print(f"check_mem_layout: normal-mode stack {stack} bytes < MIN_STACK_BYTES "
              f"{d['MIN_STACK_BYTES']}: shrink LUA_SRAM_POOL_BYTES (or .bss)", file=sys.stderr)
        ok = False

    text_region = estack - d["APP_STACK_BYTES"] - reserve - free
    data_cap = records - ccm_base
    print(f"check_mem_layout: {'OK' if ok else 'FAILED'} -- CCM used {ccm_end - ccm_base} of "
          f"{data_cap} below the records; .bss ends 0x{ebss:08x}; normal-mode stack {stack} bytes; "
          f"app mode: text region {text_region} bytes (less the api buffers), data {data_cap} bytes")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
