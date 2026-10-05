/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal CPU0 startup; no BootROM, SCU or vendor runtime dependency. */
    .section .startup,"ax",@progbits
    .global _start
_start:
    /* Record reset CSFRs before software initializes them. */
    movh.a %a2,0xd001
    lea %a2,[%a2]-4080
    mfcr %d0,$icr
    st.w [%a2]0,%d0
    mfcr %d0,$fcx
    st.w [%a2]4,%d0
    mfcr %d0,$pcxi
    st.w [%a2]8,%d0
    movh.a %sp,0xd001
    lea %sp,[%sp]-16384
    movh %d0,0xd001
    addi %d0,%d0,-8192
    mtcr $isp,%d0
    mov.u %d0,0xb7f
    mtcr $psw,%d0
    isync

    /* Build 128 linked, 64-byte context save areas at DSPR0 + 0. */
    movh.a %a2,0xd000
    mov %d0,0
    movh %d1,0xd
    mov %d2,128
1:
    st.w [%a2],%d0
    mov %d0,%d1
    add %d1,1
    lea %a2,[%a2]64
    add %d2,-1
    jnz %d2,1b
    mtcr $fcx,%d0
    movh %d0,0xd
    add %d0,1
    mtcr $lcx,%d0
    mov %d0,0
    mtcr $pcxi,%d0

    movh.a %a0,hi:__small_data
    lea %a0,[%a0]lo:__small_data
    movh.a %a1,hi:__small_const
    lea %a1,[%a1]lo:__small_const
    movh.a %a8,hi:__small_data
    lea %a8,[%a8]lo:__small_data
    movh.a %a9,hi:__small_const
    lea %a9,[%a9]lo:__small_const
    movh.a %a2,hi:__bss_start
    lea %a2,[%a2]lo:__bss_start
    movh.a %a3,hi:__bss_end
    lea %a3,[%a3]lo:__bss_end
2:
    jeq.a %a2,%a3,3f
    st.w [%a2+]4,%d0
    j 2b
3:
    movh %d0,hi:irq_vectors
    addi %d0,%d0,lo:irq_vectors
    mtcr $biv,%d0
    movh %d0,hi:trap_vectors
    addi %d0,%d0,lo:trap_vectors
    mtcr $btv,%d0
    isync
    call main
4:
    j 4b

    .section .vectors,"ax",@progbits
    .balign 8192
irq_vectors:
    .rept 256
    j irq_entry
    .balign 32
    .endr

    .section .text,"ax",@progbits
irq_entry:
    svlcx
    mfcr %d4,$icr
    and %d4,%d4,255
    call tc397_irq
    rslcx
    rfe

    .balign 256
trap_vectors:
    .rept 8
    j trap_entry
    .balign 32
    .endr
trap_entry:
    movh.a %a2,0xf000
    lea %a2,[%a2]0x644
    movh.a %a3,hi:trap_message
    lea %a3,[%a3]lo:trap_message
5:
    ld.bu %d0,[%a3+]1
    jz %d0,6f
    st.w [%a2],%d0
    j 5b
6:
    j 6b
    .section .rodata,"a",@progbits
trap_message:
    .asciz "TC397:FAIL:trap\n"
