.section .text,"ax"
.balign 2048
.global arm_vectors
arm_vectors:
.rept 8
 b arm_bad_vector
 .balign 128
.endr
 b arm_lower_sync
 .balign 128
.rept 7
 b arm_bad_vector
 .balign 128
.endr
arm_bad_vector:
 bl arm_exception_fatal
 b arm_bad_vector
arm_lower_sync:
 sub sp,sp,#304
 stp x0,x1,[sp,#0]
 stp x2,x3,[sp,#16]
 stp x4,x5,[sp,#32]
 stp x6,x7,[sp,#48]
 stp x8,x9,[sp,#64]
 stp x10,x11,[sp,#80]
 stp x12,x13,[sp,#96]
 stp x14,x15,[sp,#112]
 stp x16,x17,[sp,#128]
 stp x18,x19,[sp,#144]
 stp x20,x21,[sp,#160]
 stp x22,x23,[sp,#176]
 stp x24,x25,[sp,#192]
 stp x26,x27,[sp,#208]
 stp x28,x29,[sp,#224]
 str x30,[sp,#240]
 mrs x0,elr_el1
 mrs x1,spsr_el1
 stp x0,x1,[sp,#248]
 mrs x0,sp_el0
 mrs x1,esr_el1
 stp x0,x1,[sp,#264]
 mrs x0,far_el1
 str x0,[sp,#280]
 mov x19,sp
 mov x0,sp
 bl arm_el0_exception
 cbnz x0,arm_back_to_kernel
 mov sp,x19
 ldp x0,x1,[sp,#248]
 msr elr_el1,x0
 msr spsr_el1,x1
 ldr x0,[sp,#264]
 msr sp_el0,x0
 ldp x0,x1,[sp,#0]
 ldp x2,x3,[sp,#16]
 ldp x4,x5,[sp,#32]
 ldp x6,x7,[sp,#48]
 ldp x8,x9,[sp,#64]
 ldp x10,x11,[sp,#80]
 ldp x12,x13,[sp,#96]
 ldp x14,x15,[sp,#112]
 ldp x16,x17,[sp,#128]
 ldp x18,x19,[sp,#144]
 ldp x20,x21,[sp,#160]
 ldp x22,x23,[sp,#176]
 ldp x24,x25,[sp,#192]
 ldp x26,x27,[sp,#208]
 ldp x28,x29,[sp,#224]
 ldr x30,[sp,#240]
 add sp,sp,#304
 eret
.global arm_el0_enter
arm_el0_enter:
 sub sp,sp,#96
 stp x19,x20,[sp,#0]
 stp x21,x22,[sp,#16]
 stp x23,x24,[sp,#32]
 stp x25,x26,[sp,#48]
 stp x27,x28,[sp,#64]
 stp x29,x30,[sp,#80]
 ldr x9,=arm_return_sp
 mov x10,sp
 str x10,[x9]
 mov x9,#0x50000000
 msr elr_el1,x9
 mov x9,#0x2000
 movk x9,#0x5000,lsl #16
 msr sp_el0,x9
 mov x9,#0x3c0
 msr spsr_el1,x9
 eret
arm_back_to_kernel:
 ldr x9,=arm_return_sp
 ldr x9,[x9]
 mov sp,x9
 ldp x19,x20,[sp,#0]
 ldp x21,x22,[sp,#16]
 ldp x23,x24,[sp,#32]
 ldp x25,x26,[sp,#48]
 ldp x27,x28,[sp,#64]
 ldp x29,x30,[sp,#80]
 add sp,sp,#96
 ret
.section .bss,"aw",%nobits
.balign 8
arm_return_sp: .skip 8
.section .rodata.userprobe,"a"
.balign 4096
.global arm_user_code
arm_user_code:
 cmp x0,#1
 b.eq 1f
 cmp x0,#2
 b.eq 2f
 cmp x0,#3
 b.eq 3f
 mov x8,#1
 svc #0
 mov x8,#42
 svc #0
 b .
1: mov x1,#0x40200000
 ldr x0,[x1]
 b .
2: mov x1,#0x50000000
 str xzr,[x1]
 b .
3: mov x1,#0x1000
 movk x1,#0x5000,lsl #16
 br x1
.balign 4096
.section .note.GNU-stack,"",%progbits
