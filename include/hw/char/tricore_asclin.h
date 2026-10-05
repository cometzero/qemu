/*
 * TriCore TC3x ASCLIN UART.
 * Copyright (c) 2017 David Brenken <david.brenken@efs-auto.de>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TRICORE_ASCLIN_H
#define HW_TRICORE_ASCLIN_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qemu/fifo8.h"

#define TYPE_TRICORE_ASCLIN "tricore_asclin"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreASCLINState, TRICORE_ASCLIN)

struct TriCoreASCLINState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq[3]; /* RX, TX, error */
    uint32_t regs[21];
    Fifo8 rx_fifo;
    Fifo8 tx_fifo;
    guint watch_tag;
};

#endif
