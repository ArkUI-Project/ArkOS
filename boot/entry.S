/* Multiboot2 enters in 32-bit protected mode, even for this ELF64 image. */
.section .multiboot,"a"
.balign 8
mb2_header:
    .long 0xe85250d6
    .long 0
    .long mb2_header_end-mb2_header
    .long -(0xe85250d6 + (mb2_header_end-mb2_header))
    /* Optional information request: memory map and RGB framebuffer. */
    .short 1, 1
    .long 16
    .long 6, 8
    /* GRUB may select the closest mode; the kernel validates the result. */
    .short 5, 0
    .long 20
    .long 1280, 800, 32
    .balign 8
    .short 0, 0
    .long 8
mb2_header_end:

.section .text.boot,"ax"
.code32
.global _start
.type _start,@function
_start:
    cli
    cld
    movl $boot_stack_top, %esp
    movl %eax, saved_magic
    movl %ebx, saved_info
    call serial_init32
    movl $msg_entry, %esi
    call serial_puts32

    /* Check CPUID, then the long-mode feature before touching its MSR. */
    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $0x00200000, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    pushl %ecx
    popfl
    xorl %ecx, %eax
    testl $0x00200000, %eax
    jz boot_no_long_mode
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb boot_no_long_mode
    movl $0x80000001, %eax
    cpuid
    testl $0x20000000, %edx
    jz boot_no_long_mode

    /* Six page-table pages: PML4, PDPT, and four page directories. */
    movl $pml4, %edi
    xorl %eax, %eax
    movl $(6*4096/4), %ecx
    rep stosl
    movl $pdpt, %eax
    orl $3, %eax
    movl %eax, pml4
    movl $page_directories, %eax
    orl $3, %eax
    movl $pdpt, %edi
    movl $4, %ecx
1:
    movl %eax, (%edi)
    addl $4096, %eax
    addl $8, %edi
    loop 1b
    /* 2048 large (2 MiB) pages cover physical addresses [0, 4 GiB). */
    movl $page_directories, %edi
    movl $0x83, %eax
    movl $2048, %ecx
2:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 2b
    movl %cr4, %eax
    orl $0x20, %eax                 /* CR4.PAE */
    movl %eax, %cr4
    movl $pml4, %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx          /* IA32_EFER */
    rdmsr
    orl $0x100, %eax               /* EFER.LME */
    wrmsr
    lgdt gdt_pointer
    movl %cr0, %eax
    orl $0x80000001, %eax          /* CR0.PG | CR0.PE */
    movl %eax, %cr0
    ljmp $0x08, $long_mode_entry

boot_no_long_mode:
    movl $msg_cpu_error, %esi
    call serial_puts32
3:
    cli
    hlt
    jmp 3b

serial_init32:
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3f8, %dx
    movb $1, %al
    outb %al, %dx
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $3, %al
    outb %al, %dx
    movw $0x3fa, %dx
    movb $0xc7, %al
    outb %al, %dx
    movw $0x3fc, %dx
    movb $0x0b, %al
    outb %al, %dx
    ret
serial_puts32:
    lodsb
    testb %al, %al
    jz 6f
    movb %al, %bl
    movl $100000, %ecx
    movw $0x3fd, %dx
4:
    inb %dx, %al
    testb $0x20, %al
    jnz 5f
    loop 4b
5:
    movw $0x3f8, %dx
    movb %bl, %al
    outb %al, %dx
    jmp serial_puts32
6:
    ret

.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    leaq boot_stack_top(%rip), %rsp
    xorq %rbp, %rbp
    movl saved_magic(%rip), %edi
    movl saved_info(%rip), %esi
    call kernel_main
7:
    cli
    hlt
    jmp 7b
.size _start, .-_start

.section .rodata.boot,"a"
.balign 8
gdt:
    .quad 0
    .quad 0x00af9a000000ffff       /* 64-bit code, ring 0 */
    .quad 0x00cf92000000ffff       /* data, ring 0 */
gdt_end:
gdt_pointer:
    .word gdt_end-gdt-1
    .long gdt
msg_entry:
    .asciz "[boot] Multiboot2 entry; preparing x86_64\r\n"
msg_cpu_error:
    .asciz "[boot] FATAL: x86_64 long mode is required\r\n"

.section .bss.boot,"aw",@nobits
.balign 4
saved_magic: .skip 4
saved_info: .skip 4
.balign 4096
pml4: .skip 4096
pdpt: .skip 4096
page_directories: .skip 16384
.balign 16
boot_stack_bottom: .skip 131072
boot_stack_top:
.section .note.GNU-stack,"",@progbits
