/*
 * Link script for native E16 apps (.e16app, src/apps/e16_app_format.h).
 *
 * The app is position-independent and the loader puts text and data
 * wherever there's room, so these addresses only have to be distinct: a
 * link-time address in [TEXT] is a text address, one in [DATA] a data
 * address, and the loader rebases each by its range. 0x80000000 isn't
 * RAM on the E16, so a pointer that somehow escaped rebasing faults
 * instead of quietly reading something.
 *
 * TEXT has no real size limit here: the loader checks the image against
 * the RAM it actually has. DATA (GOT + .data + .bss) is capped at the
 * 64 KB of CCM the loader puts it in.
 */
ENTRY(_e16_start)

MEMORY
{
    TEXT (rx)  : ORIGIN = 0x00000000, LENGTH = 16M
    DATA (rw)  : ORIGIN = 0x80000000, LENGTH = 64K
}

SECTIONS
{
    .text : ALIGN(8)
    {
        __e16_text_start = .;
        KEEP(*(.text.e16_entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __e16_text_end = .;
    } > TEXT

    /* The GOT first, so r9 (its origin) is at a fixed spot near the start. */
    .got : ALIGN(8)
    {
        __e16_data_start = .;
        __e16_got_start = .;
        _GLOBAL_OFFSET_TABLE_ = .;
        *(.got.plt) *(.igot.plt) *(.got) *(.igot)
        . = ALIGN(4);
        __e16_got_end = .;
    } > DATA

    /* Initialized data, including the pointer tables -fpic puts in
     * .data.rel* (the loader rebases those through the fixup table). */
    .data : ALIGN(4)
    {
        *(.data .data.*)
        *(.data.rel .data.rel.* .data.rel.ro .data.rel.ro.*)
        . = ALIGN(8);
        __e16_data_end = .;
    } > DATA

    .bss (NOLOAD) : ALIGN(8)
    {
        __e16_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __e16_bss_end = .;
    } > DATA

    /* Not supported: constructors/destructors. KEEP so neither
     * --gc-sections nor this script drops them silently: tools/mkapp.py
     * refuses an app that has any. */
    .init_array : { KEEP(*(.preinit_array .preinit_array.* .init_array .init_array.* .ctors .ctors.*)) } > DATA
    .fini_array : { KEEP(*(.fini_array .fini_array.* .dtors .dtors.*)) } > DATA

    /DISCARD/ :
    {
        *(.ARM.exidx .ARM.exidx.* .ARM.extab .ARM.extab.*)
        *(.comment .note .note.*)
        *(.init .fini)
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
