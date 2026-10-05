/* SPDX-License-Identifier: GPL-2.0-or-later */
OUTPUT_FORMAT("elf32-tricore")
OUTPUT_ARCH(tricore)
ENTRY(_start)
MEMORY {
    /* Deliberately different from reset PC 0xa0000000. */
    flash (rx) : ORIGIN = 0xa0000100, LENGTH = 3M - 0x100
    ram (rw) : ORIGIN = 0xd0004000, LENGTH = 16K
}
SECTIONS {
    .startup : { KEEP(*(.startup)) } > flash
    .text : { *(.text .text.*) } > flash
    .vectors : { KEEP(*(.vectors)) } > flash
    .rodata : {
        *(.rodata .rodata.* .sdata2 .sdata2.*)
        __small_const = ADDR(.rodata) + 0x8000;
    } > flash
    .data : { *(.data .data.* .sdata .sdata.*) } > ram
    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
        __small_data = ADDR(.bss) + 0x8000;
    } > ram
    /DISCARD/ : { *(.comment .version_info .version.htc) }
}
