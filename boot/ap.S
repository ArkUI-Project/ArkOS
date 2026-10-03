/* Relocatable real-mode SIPI stub, copied to a reserved conventional RAM page. */
.section .rodata.ap,"a"
.global ap_stub_start,ap_stub_end
.set AP_BASE,0x8000
.code16
ap_stub_start:
 cli
 cld
 xorw %ax,%ax
 movw %ax,%ds
 movw %ax,%es
 movw %ax,%ss
 lgdt AP_BASE+(ap_gdtr-ap_stub_start)
 movl %cr0,%eax
 orl $1,%eax
 movl %eax,%cr0
 ljmpl $0x18,$AP_BASE+(ap_pm-ap_stub_start)
.code32
ap_pm:
 movw $0x10,%ax
 movw %ax,%ds
 movw %ax,%es
 movw %ax,%ss
 movl $0x20,%eax
 movl %eax,%cr4
 movl 0x8f00,%eax
 movl %eax,%cr3
 movl $0xc0000080,%ecx
 rdmsr
 orl $0x900,%eax
 wrmsr
 movl %cr0,%eax
 orl $0x80000001,%eax
 movl %eax,%cr0
 ljmpl $8,$AP_BASE+(ap_lm-ap_stub_start)
.code64
ap_lm:
 movw $0x10,%ax
 movw %ax,%ds
 movw %ax,%es
 movw %ax,%ss
 movq 0x8f08,%rsp
 xorq %rbp,%rbp
 movq 0x8f10,%rdi
 movq 0x8f18,%rax
 call *%rax
1: cli
 hlt
 jmp 1b
.balign 8
ap_gdt:
 .quad 0,0x00af9a000000ffff,0x00cf92000000ffff,0x00cf9a000000ffff
ap_gdtr:
 .word 31
 .long AP_BASE+(ap_gdt-ap_stub_start)
ap_stub_end:
.section .note.GNU-stack,"",@progbits
