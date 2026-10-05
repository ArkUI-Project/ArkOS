/* module_stack_call(stack_top, fn, arg1, arg2)
 * Switch rsp onto a module's private stack, call fn(arg1, arg2) and restore
 * the caller's frame around it. The new stack top is page-aligned; anding to
 * 16 keeps the SysV call-site alignment rule (rsp%16 == 0 at call). The
 * callee runs in kernel mode; a fault inside it is a kernel fault. */
.text
.globl module_stack_call
module_stack_call:
    push %rbp
    mov %rsp, %rbp
    mov %rsi, %r11          /* callee */
    mov %rdx, %r10          /* arg1 */
    mov %rdi, %rsp          /* switch */
    and $-16, %rsp
    mov %r10, %rdi
    mov %rcx, %rsi          /* arg2 */
    call *%r11
    mov %rbp, %rsp
    pop %rbp
    ret
