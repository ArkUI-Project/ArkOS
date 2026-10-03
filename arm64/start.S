.section .text.boot,"ax"
.global _start,arm_secondary
_start:
 msr daifset,#15
 mov x20,x0
 ldr x1,=__bss_start
 ldr x2,=__bss_end
1: cmp x1,x2
 b.hs 2f
 str xzr,[x1],#8
 b 1b
2: ldr x1,=arm_stacks
 add x1,x1,#32768
 mov sp,x1
 mov x0,x20
 bl arm_main
3: wfe
 b 3b
arm_secondary:
 msr daifset,#15
 ldr x1,=arm_stacks
 add x2,x0,#1
 lsl x2,x2,#15
 add x1,x1,x2
 mov sp,x1
 bl arm_ap_main
 b 3b
.section .bss,"aw",%nobits
.balign 4096
.global arm_stacks
arm_stacks: .skip 8*32768
.section .note.GNU-stack,"",%progbits
