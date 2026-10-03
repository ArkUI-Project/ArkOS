.section .text
.code64
.global process_enter_frame
.type process_enter_frame,@function
/* All entries and stacks were constructed and checked by the kernel. */
process_enter_frame:
    cli
    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es
    movq %rdi, %rsp
    jmp interrupt_return
.size process_enter_frame,.-process_enter_frame
.section .note.GNU-stack,"",@progbits
