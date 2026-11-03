#!/bin/bash
# OXI's only hard rule for this firmware: nothing but OXI's own bootloader
# ever writes the E16's internal (MCU) flash at runtime -- that's what
# holds the bootloader and the manufacturing calibration the stock
# firmware relies on, and it's where a bad write bricks the device with
# no recovery. SPI (external) flash writes are a separate, softer
# concern (flash_budget.h's erase-rate limiting); this script is only
# about the internal flash.
#
# Two checks, both must pass:
#
#   1. nm on the linked ELF: fails if any of the HAL's flash-program,
#      flash-erase, unlock or option-byte entry points are actually LINKED
#      in (or if nm itself fails: no symbol list is not a pass). The SDK's HAL_
#      FLASH module can still be *compiled* into libe16_bsp.a (it's
#      fine for it to exist in the archive) -- --gc-sections drops
#      anything the firmware never calls, so this only fails if the
#      firmware actually calls one of them, directly or indirectly.
#   2. A source grep over src/ for the raw register names a from-
#      scratch (non-HAL) internal-flash or option-byte write would use
#      instead (FLASH->CR/KEYR/SR/OPTCR/OPTKEYR, FLASH_KEY1/2,
#      FLASH_OPTKEY1/2, a 0x40023Cxx address literal): the HAL check above
#      can't see code that pokes the registers directly without going
#      through HAL_FLASH_*. (app_mode.c's MPU guard names the register
#      block as FLASH_R_BASE, which this deliberately doesn't match: it
#      only makes the registers read-only.)
#
# Usage: check_no_flash_writes.sh <elf> [nm-tool] [src-dir]
set -u
set -o pipefail

elf="${1:?usage: check_no_flash_writes.sh <elf> [nm-tool] [src-dir]}"
nm="${2:-arm-none-eabi-nm}"
src_dir="${3:-$(cd "$(dirname "$0")/.." && pwd)/src}"

if [ ! -f "$elf" ]; then
    echo "check_no_flash_writes: $elf not found" >&2
    exit 1
fi
if ! command -v "$nm" >/dev/null 2>&1; then
    echo "check_no_flash_writes: '$nm' not found (set ARM_TOOLCHAIN_DIR, or pass the nm tool as \$2)" >&2
    exit 1
fi

status=0

# ---- 1. linked symbols ----
#
# nm's defined-symbol letters vary (T/t text, D/d data, W/w weak, ...);
# what matters is that the symbol is DEFINED in this ELF (gc-sections
# already dropped anything unused), so "U" (undefined) is excluded by
# only looking at the symbol-name column once nm's own "U" lines are
# filtered. --defined-only does exactly that and exists in GNU nm.
forbidden_pattern='^(HAL_FLASH_Program|HAL_FLASH_Unlock|HAL_FLASH_Lock|HAL_FLASHEx_Erase|HAL_FLASHEx_Erase_IT|FLASH_Program_(Byte|HalfWord|Word|DoubleWord)|FLASH_Erase_Sector|FLASH_MassErase|HAL_FLASH_OB_Unlock|HAL_FLASH_OB_Lock|HAL_FLASH_OB_Launch|HAL_FLASHEx_OBProgram|HAL_FLASHEx_AdvOBProgram|FLASH_OB_.*)$|^HAL_FLASH(Ex)?_.*(Program|Erase|OB).*$'

if ! syms="$("$nm" --defined-only -C "$elf")"; then
    echo "check_no_flash_writes: '$nm' failed on $elf" >&2
    exit 1
fi
if [ -z "$syms" ]; then
    echo "check_no_flash_writes: '$nm' listed no symbols in $elf" >&2
    exit 1
fi
hits="$(printf '%s\n' "$syms" | awk '{print $3}' | grep -E "$forbidden_pattern" || true)"
if [ -n "$hits" ]; then
    echo "check_no_flash_writes: internal-flash HAL symbols are LINKED into $elf:" >&2
    echo "$hits" | sed 's/^/    /' >&2
    echo "Only OXI's bootloader may write internal flash at runtime -- nothing in" >&2
    echo "this firmware may call these, even indirectly (README's safety model)." >&2
    status=1
fi

# ---- 2. raw register access in source ----
#
# Excludes comments mentioning these names in prose (this file and
# README.md do); a real grep over src/ is cheap enough not to need
# anything fancier than "appears outside a // or a block comment" --
# in practice these registers are never named in this codebase's
# comments either, so a plain grep is enough and avoids false negatives
# a comment-aware parser could introduce by mistake.
if [ -d "$src_dir" ]; then
    reg_hits="$(grep -rnE 'FLASH->(CR|KEYR|SR|OPTCR1?|OPTKEYR)\b|\bFLASH_(OPT)?KEY[12]\b|0[xX]40023[cC][0-9a-fA-F]{2}|\bHAL_FLASH_OB_(Unlock|Launch)\b' "$src_dir" || true)"
    if [ -n "$reg_hits" ]; then
        echo "check_no_flash_writes: direct internal-flash register access found in $src_dir:" >&2
        echo "$reg_hits" | sed 's/^/    /' >&2
        status=1
    fi
else
    echo "check_no_flash_writes: source dir $src_dir not found" >&2
    status=1
fi

if [ $status -eq 0 ]; then
    echo "check_no_flash_writes: OK -- no internal-flash write path linked or found in $elf"
fi
exit $status
