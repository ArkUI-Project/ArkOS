.section .text.start,"ax",@progbits
.global _start
.type _start,@function
_start:
    xor %rbp,%rbp
    and $-16,%rsp
    call main
    mov %rax,%rdi
    xor %rax,%rax
    int $0x80
1:  mov $1,%eax
    int $0x80
    jmp 1b
.section .note.GNU-stack,"",@progbits
