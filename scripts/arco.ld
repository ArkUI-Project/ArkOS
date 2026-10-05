/* .arco image layout (scripts/arco.ld).
 *
 * One contiguous position-independent image starting at vaddr 0. The loader
 * maps [0, data_off) read+execute and everything from data_off onwards
 * read+write+NX, so data_off must be the first byte after the last read-only
 * section and 4 KiB aligned. __arco_data_off, __arco_image_end and __arco_bss
 * are read by scripts/arco.py to fill the ARCO1 header. */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(arco_entry)
SECTIONS
{
    . = 0;
    __arco_start = .;
    .text : {
        *(.text.boot)
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }
    . = ALIGN(4096);
    __arco_data_off = .;
    .data : {
        *(.data .data.*)
    }
    . = ALIGN(8);
    __arco_image_end = .;
    .bss (NOLOAD) : {
        __arco_bss = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
    }
    __arco_end = .;
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.interp)
        *(.dynamic)
        *(.dynsym .dynstr .dynbss)
        *(.gnu.hash .gnu.version*)
        *(.rela .rela.* .rel .rel.*)
    }
}