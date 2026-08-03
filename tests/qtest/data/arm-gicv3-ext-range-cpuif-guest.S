.section .text
.global _start
.org 0
_start:
    mrs x0, CurrentEL
    cmp x0, #12
    b.eq from_el3
    cmp x0, #8
    b.ne at_el1
    adr x0, at_el1
    msr ELR_EL2, x0
    mov x0, #0x3c5
    msr SPSR_EL2, x0
    eret

from_el3:
    adr x1, at_el1
    mrs x0, SCR_EL3
    orr x0, x0, #(1 << 10)
    bic x0, x0, #0xf
    msr SCR_EL3, x0
    msr ELR_EL3, x1
    mov x0, #0x3c5
    msr SPSR_EL3, x0
    eret

at_el1:
    msr SPSel, #1
    adr x0, vectors
    msr VBAR_EL1, x0
    mov x0, #1
    msr ICC_SRE_EL1, x0
    isb
    mov x0, #0xff
    msr ICC_PMR_EL1, x0
    mov x0, #1
    msr ICC_IGRPEN1_EL1, x0
    mrs x9, MPIDR_EL1
    and x9, x9, #0xff
    movz x8, #0x4060, lsl #16
    cbnz x9, mark_ready
    ldr x0, [x8, #0x170]
    cbnz x0, mark_ready
    movz x0, #3
    movk x0, #0xc400, lsl #16
    mov x1, #1
    adr x2, _start
    mov x3, xzr
    hvc #0
mark_ready:
    add x10, x8, #0x100
    add x10, x10, x9, lsl #3
    mov x11, #1
    str x11, [x10]
    msr DAIFClr, #3

command_loop:
    add x10, x8, #0x120
    add x10, x10, x9, lsl #3
    ldr x11, [x10]
    cbz x11, command_loop
    cmp x11, #1
    b.eq command_eoi
    cmp x11, #2
    b.eq command_dir
    cmp x11, #3
    b.eq command_mode0
    cmp x11, #4
    b.eq command_mode1
    cmp x11, #5
    b.eq command_mask
    cmp x11, #6
    b.eq command_unmask
    cmp x11, #7
    b.eq command_iar1
    cmp x11, #8
    b.eq command_iar0
    cmp x11, #9
    b.eq command_eoi0
    cmp x11, #10
    b.eq command_enable_group0
    cmp x11, #11
    b.eq command_config_espi_group0
    cmp x11, #12
    b.eq command_config_eppi_group1s
    b command_done

command_eoi:
    ldr x12, [x8, #0x160]
    msr ICC_EOIR1_EL1, x12
    b command_done

command_dir:
    ldr x12, [x8, #0x160]
    msr ICC_DIR_EL1, x12
    b command_done

command_mode0:
    mrs x12, ICC_CTLR_EL1
    bic x12, x12, #2
    msr ICC_CTLR_EL1, x12
    b command_done

command_mode1:
    mrs x12, ICC_CTLR_EL1
    orr x12, x12, #2
    msr ICC_CTLR_EL1, x12
    b command_done

command_mask:
    msr DAIFSet, #2
    b command_done

command_unmask:
    msr DAIFClr, #3
    b command_done

command_iar1:
    mrs x12, ICC_IAR1_EL1
    add x13, x8, #0x180
    add x13, x13, x9, lsl #3
    str x12, [x13]
    b command_done

command_iar0:
    mrs x12, ICC_IAR0_EL1
    add x13, x8, #0x180
    add x13, x13, x9, lsl #3
    str x12, [x13]
    b command_done

command_eoi0:
    ldr x12, [x8, #0x160]
    msr ICC_EOIR0_EL1, x12
    b command_done

command_enable_group0:
    mov x12, #1
    msr ICC_IGRPEN0_EL1, x12
    b command_done

command_config_espi_group0:
    movz x14, #0x0800, lsl #16
    ldr w12, [x14]
    orr w12, w12, #1
    str w12, [x14]
    mov w12, #0x40
    add x15, x14, #2, lsl #12
    strb w12, [x15]
    mov w12, #2
    str w12, [x14, #0x3000]
    add x15, x14, #8, lsl #12
    str xzr, [x15]
    mov w12, #1
    str w12, [x14, #0x1200]
    str x12, [x8, #0x178]
    b command_done

command_config_eppi_group1s:
    movz x14, #0x080b, lsl #16
    str wzr, [x14, #0x84]
    mov w12, #1
    str w12, [x14, #0xd04]
    mov w12, #0x40
    strb w12, [x14, #0x420]
    mov w12, #2
    str w12, [x14, #0xc08]
    mov w12, #1
    str w12, [x14, #0x104]
    movz x14, #0x0800, lsl #16
    ldr w12, [x14]
    orr w12, w12, #4
    str w12, [x14]
    str xzr, [x8, #0x178]

command_done:
    str xzr, [x10]
    add x12, x8, #0x140
    add x12, x12, x9, lsl #3
    ldr x13, [x12]
    add x13, x13, #1
    str x13, [x12]
    b command_loop

.balign 2048
vectors:
    b exception_hang
    .balign 128
    b irq_handler
    .balign 128
    b irq_handler
    .balign 128
    b exception_hang
    .balign 128
    b exception_hang
    .balign 128
    b irq_handler
    .balign 128
    b irq_handler
    .balign 128
    b exception_hang
    .balign 128
    b exception_hang
    .balign 128
    b irq_handler
    .balign 128
    b exception_hang
    .balign 128
    b exception_hang
    .balign 128
    b exception_hang
    .balign 128
    b irq_handler
    .balign 128
    b exception_hang
    .balign 128
    b exception_hang

irq_handler:
    ldr x15, [x8, #0x178]
    cbnz x15, irq_iar0
    mrs x10, ICC_IAR1_EL1
    b irq_iar_done
irq_iar0:
    mrs x10, ICC_IAR0_EL1
irq_iar_done:
    add x11, x8, #0x1a0
    add x11, x11, x9, lsl #3
    ldr x12, [x11]
    cbnz x12, irq_eoi
    mov x12, #1
    str x12, [x11]
    add x11, x8, #0x180
    add x11, x11, x9, lsl #3
    str x10, [x11]
    add x11, x8, #0x1a0
    add x11, x11, x9, lsl #3
irq_eoi:
    cbnz x15, irq_eoi0
    msr ICC_EOIR1_EL1, x10
    b irq_eoi_done
irq_eoi0:
    msr ICC_EOIR0_EL1, x10
irq_eoi_done:
    cbz x15, irq_eoi_observed
    movz x16, #0x0800, lsl #16
    ldr w17, [x16, #0x1a00]
    str x17, [x8, #0x1e0]
irq_eoi_observed:
    mov x12, #2
    str x12, [x11]
    ldr x12, [x8, #0x168]
    cbz x12, irq_done
irq_wait_dir:
    add x13, x8, #0x1c0
    add x13, x13, x9, lsl #3
    ldr x14, [x13]
    cbz x14, irq_wait_dir
    msr ICC_DIR_EL1, x10
    cbz x15, irq_dir_observed
    movz x16, #0x0800, lsl #16
    ldr w17, [x16, #0x1a00]
    str x17, [x8, #0x1e0]
irq_dir_observed:
    mov x12, #3
    str x12, [x11]
irq_done:
    eret

exception_hang:
    wfe
    b exception_hang
