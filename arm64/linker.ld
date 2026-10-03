ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x40200000;
 .text : { __text_start = .; KEEP(*(.text.boot)) *(.text*) . = ALIGN(4096); __text_end = .; } :text
 .rodata : ALIGN(4096) { *(.rodata*) . = ALIGN(4096); __rodata_end = .; } :ro
 .data : ALIGN(16) { *(.data*) } :data
 .bss (NOLOAD) : ALIGN(4096) { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); __bss_end = .; } :data
 /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
