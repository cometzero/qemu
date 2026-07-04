/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_ARM_ARCH_TIMER_MMIO_H
#define HW_TIMER_ARM_ARCH_TIMER_MMIO_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_ARM_ARCH_TIMER_MMIO "arm_arch_timer_mmio"
OBJECT_DECLARE_SIMPLE_TYPE(ArmArchTimerMMIOState, ARM_ARCH_TIMER_MMIO)

#define ARM_ARCH_TIMER_MMIO_MAX_FRAMES 8
#define ARM_ARCH_TIMER_MMIO_CNTBaseN_NAME "CNTBase"

#define ARM_ARCH_TIMER_MMIO_CNTCTL_CNTFRQ      0x000
#define ARM_ARCH_TIMER_MMIO_CNTCTL_CNTSR       0x004
#define ARM_ARCH_TIMER_MMIO_CNTCTL_CNTTID      0x008
#define ARM_ARCH_TIMER_MMIO_CNTCTL_CNTACR_BASE 0x040

#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_LO    0x000
#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_HI    0x004
#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTFID       0x00c
#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTFRQ       0x010
#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPL0ACR    0x014
#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO 0x020
#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_HI 0x024
#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_TVAL    0x028
#define ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL     0x02c

#define ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE 0x1
#define ARM_ARCH_TIMER_MMIO_CNTP_CTL_IMASK  0x2
#define ARM_ARCH_TIMER_MMIO_CNTP_CTL_ISTAT  0x4

typedef struct ArmArchTimerMMIOFrame {
    void *parent;
    uint32_t index;
    uint64_t cval;
    uint32_t ctl;
    uint32_t cntpl0acr;
    qemu_irq irq;
    QEMUTimer *timer;
} ArmArchTimerMMIOFrame;

struct ArmArchTimerMMIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t cntfrq;
    uint32_t nr_frames;
    uint64_t view_size;
    uint64_t frame_offset[ARM_ARCH_TIMER_MMIO_MAX_FRAMES];
    uint32_t frame_id[ARM_ARCH_TIMER_MMIO_MAX_FRAMES];
    uint32_t cntacr[ARM_ARCH_TIMER_MMIO_MAX_FRAMES];
    ArmArchTimerMMIOFrame frame[ARM_ARCH_TIMER_MMIO_MAX_FRAMES];
};

#endif
