/*
 * Arm SSE Subsystem System Timer
 *
 * Copyright (c) 2020 Linaro Limited
 * Written by Peter Maydell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

/*
 * This is a model of the "System timer" which is documented in
 * the Arm SSE-123 Example Subsystem Technical Reference Manual:
 * https://developer.arm.com/documentation/101370/latest/
 *
 * QEMU interface:
 *  + QOM property "counter": link property to be set to the
 *    TYPE_SSE_COUNTER timestamp counter device this timer runs off
 *  + sysbus MMIO region 0: the register bank
 *  + sysbus IRQ 0: timer interrupt
 */

#ifndef SSE_TIMER_H
#define SSE_TIMER_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/timer/sse-counter.h"

typedef struct ArmSSETimerSnapshot {
    int64_t qemu_virtual_ns;
    uint64_t count;
    uint64_t cval;
    uint64_t counter_frequency_hz;
    uint32_t cntfrq;
    uint32_t ctl;
    uint32_t irq_level;
} ArmSSETimerSnapshot;

#define TYPE_SSE_TIMER "sse-timer"
OBJECT_DECLARE_SIMPLE_TYPE(SSETimer, SSE_TIMER)

struct SSETimer {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    SSECounter *counter;
    QEMUTimer timer;
    Notifier counter_notifier;
    bool counter_notifier_registered;
    bool irq_level;

    uint32_t cntfrq;
    uint32_t cntp_ctl;
    uint64_t cntp_cval;
    uint64_t cntp_aival;
    uint32_t cntp_aival_ctl;
    uint32_t cntp_aival_reload;
};

bool sse_timer_get_snapshot(SSECounter *counter, SSETimer *timer,
                            ArmSSETimerSnapshot *snapshot);

#endif
