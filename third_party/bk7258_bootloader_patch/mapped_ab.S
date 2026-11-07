.syntax unified
.cpu cortex-m33
.thumb
.section .text.branch,"ax",%progbits
.global bk_mapped_ab_branch
.type bk_mapped_ab_branch, %function
.thumb_func
bk_mapped_ab_branch:
    /* The native linker puts this at the original checker entry. The C
       function follows the same (partition, name) call/return ABI. */
    b.w bk_mapped_ab_check
