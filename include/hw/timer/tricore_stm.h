/*
 * TriCore TC3x System Timer.
 * Copyright (c) 2017 David Brenken
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TRICORE_STM_H
#define HW_TRICORE_STM_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"

#define TYPE_TRICORE_STM "tricore_stm"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreSTMState, TRICORE_STM)

struct TriCoreSTMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Clock *fstm;
    qemu_irq irq[2];
    QEMUTimer *timer[2];
    uint32_t regs[64];
    uint64_t counter;
    int64_t base_ns;
};

#endif
