OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS
{
    /* OVMF keeps ACPI NVS around 8--9 MiB. A large static kernel/page pool
     * must not span that firmware reservation; BIOS also supports this base. */
    . = 16M;
    __kernel_start = .;
    .multiboot ALIGN(8) : { KEEP(*(.multiboot)) } :text
    .text ALIGN(4096) : { *(.text.boot) *(.text .text.*) } :text
    .rodata ALIGN(4096) : { *(.rodata.boot) *(.rodata .rodata.*) } :rodata
    .data ALIGN(4096) : { *(.data .data.*) } :data
    .bss ALIGN(4096) (NOLOAD) : {
        __bss_start = .;
        *(.bss.boot) *(.bss .bss.*) *(COMMON)
        __bss_end = .;
    } :data
    __kernel_end = ALIGN(4096);
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
