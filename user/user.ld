OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
PHDRS {
 text PT_LOAD FLAGS(5);
 rodata PT_LOAD FLAGS(4);
 font PT_LOAD FLAGS(4);
 data PT_LOAD FLAGS(6);
}
SECTIONS {
 . = 0x40000000;
 .text ALIGN(4096) : { *(.text.start) *(.text .text.*) } :text
 .rodata ALIGN(4096) : { *(.rodata .rodata.*) } :rodata
 .arkfont ALIGN(4096) : { *(.arkfont) } :font
 .data ALIGN(4096) : { *(.data .data.*) } :data
 .bss ALIGN(4096) (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
 /DISCARD/ : { *(.eh_frame .eh_frame.*) *(.comment) *(.note*) }
}
