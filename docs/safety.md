# Safety model

**Internal flash**: OXI's bootloader is the only thing that ever writes
the E16's *internal* (MCU) flash at runtime — that's where the bootloader
itself and manufacturing calibration live, and a bad write there has no
recovery path. This firmware never does, and a CMake post-build step
enforces it: `tools/check_no_flash_writes.sh` runs `arm-none-eabi-nm` on
the linked `launcher.elf` and fails the build if any of
`HAL_FLASH_Program`, `HAL_FLASH_Unlock`, `HAL_FLASHEx_Erase[_IT]`,
`FLASH_Program_*`, `FLASH_Erase_Sector`, `FLASH_MassErase`, or anything
matching `HAL_FLASH(Ex)?_*Program*`/`*Erase*` is actually linked in (not
merely compiled — the SDK's HAL_FLASH module can still live unused inside
`libe16_bsp.a`; `--gc-sections` is what keeps it from ever being pulled
into the firmware the check inspects) or any option-byte entry point
(`HAL_FLASH_OB_Unlock`/`_Launch`, `HAL_FLASHEx_OBProgram`, ...), plus a
source grep over `src/` for direct register access that wouldn't go
through the HAL at all (`FLASH->CR`/`KEYR`/`SR`/`OPTCR`/`OPTKEYR`,
`FLASH_KEY1/2`, `FLASH_OPTKEY1/2`, any `0x40023Cxx` address literal). A
failing `nm` fails the check too. Native apps are the one exception to
"nothing else runs on the CPU": see [app-runtime.md](app-runtime.md) for how app mode
guards against a stray write from one. Separately, the SPI-flash side
(`src/disk/flash_e16.c`) refuses any access below `0x400000`, where the
stock firmware keeps its own data.

**External (SPI) flash** is a different, deliberately softer story:
writes there are fine, just rate-limited. Every sector erase on the SPI
flash — persistence saves and the FAT volume's write-back cache alike —
goes through a shared token-bucket limiter, `src/disk/flash_budget.c`:
about 1 erase/s sustained with a burst of up to 16, and any single sector
capped at 4 erases per rolling minute, so no bug or runaway script can
wear out the chip even on top of the append-only record format below.
Disk mode raises the budget for as long as it's active (~40 erases/s,
burst 256, no per-sector cap) because the host computer legitimately
rewrites FAT tables and directory sectors often while copying files; it
drops back to normal on leaving. An erase refused for being over budget
is deferred, not performed: persistence keeps the save in RAM and retries
on the next debounce tick, and the block device keeps its cache dirty and
retries the same way (disk mode's write path retries a refused erase
for a few seconds before failing the host's request outright; a verify
failure isn't retried). Three bounded jobs run under the Disk-mode budget
because the normal one can't cover them: the first boot's format (it
needs all 16 of the normal burst on non-blank flash), the flush before
every reset into or out of app mode, and Disk mode itself, which also
writes its cache back after 300 ms without I/O, so a copy that ends
without an eject (a pulled cable) doesn't lose its last sector. An app's
`file_write()` starts only if the normal budget can cover its worst case,
then runs to the end without the per-sector cap cutting it short
(`src/apps/app_files.h` has the measured numbers).

**Append-only persistence records**: var's 32 slots and store's buffers
live in RAM outside the Lua heap, bound to the running script's name
(`src/rt/rt.h`). `src/persist.c` saves them into a 512 KB reserve at the
top of the SPI flash, `0xF80000–0xFFFFFF` — never anywhere else, in
particular never into the FAT volume that occupies the rest of the chip.
The reserve holds 64 slots of two 4 KB sectors each (vars, store): slot 0
is the shared store, and a script's slot is found by hashing its name
with linear probing, so up to 63 scripts can each keep saved state
regardless of manifest order. A script only takes a slot once it has
something to keep (empty vars or an empty store claim nothing), and at
boot, if the disk mounted, the slots of scripts that no longer exist
(neither built in nor in `/scripts`) are freed (`persist: freed N
slots`). Within a slot's sector, saves are
append-only — a save writes a new record right after whatever's already
there, rather than erasing every time, and the sector is only erased (and
the new record written at offset 0) once the next record wouldn't fit. A
record's commit marker is written last, in a flash write of its own, so a
power loss mid-save leaves a torn, uncommitted record that's ignored in
favor of whatever was there before. A load reads the *last* valid record
in the sector. Vars save 5 s after a change and on exit; store only saves
on exit, so store changes are lost if the E16 loses power while a script
is running. At a typical vars record size this gives roughly one erase
every 75 s of continuous encoder turning, which works out to on the order
of 2,200 hours of nonstop turning before a slot's vars sector nears a
typical 100,000-erase NOR rating.

**Flash map** (16 MB SPI NOR):

| Range | Contents |
|---|---|
| `0x000000-0x3FFFFF` | Stock firmware data. Never touched. |
| `0x400000-0xF7FFFF` | FAT volume (disk mode): scripts, apps, and anything else copied there. |
| `0xF80000-0xFFFFFF` | Persistence reserve: the name-keyed slot table above. |
