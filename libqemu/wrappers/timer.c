/*
 * libqemu
 *
 * Copyright (c) 2019 Luc Michel <luc.michel@greensocs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/timer/arm_arch_timer_mmio.h"
#include "hw/timer/sse-counter.h"
#include "hw/timer/sse-timer.h"

#include "timer.h"

int64_t libqemu_clock_virtual_get_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

QemuTimer *libqemu_timer_new_virtual_ns(LibQemuTimerCb cb, void *opaque)
{
    QemuTimer *ret = (QemuTimer *) timer_new_ns(QEMU_CLOCK_VIRTUAL, cb, opaque);
    return ret;
}

void libqemu_sse_counter_set_snapshot(Object *obj, uint64_t count,
                                      bool running)
{
    sse_counter_set_snapshot(SSE_COUNTER(obj), count, running);
}

uint64_t libqemu_sse_counter_get_value(Object *obj)
{
    return sse_counter_value(SSE_COUNTER(obj));
}

void libqemu_arm_arch_timer_mmio_set_snapshot(
    Object *obj, uint64_t count, bool running, uint32_t frequency_hz)
{
    arm_arch_timer_mmio_set_counter_snapshot(
        ARM_ARCH_TIMER_MMIO(obj), count, running, frequency_hz);
}

uint64_t libqemu_arm_arch_timer_mmio_get_value(Object *obj)
{
    return arm_arch_timer_mmio_get_counter_value(
        ARM_ARCH_TIMER_MMIO(obj));
}

bool libqemu_arm_arch_timer_mmio_get_frame_snapshot(
    Object *obj, uint32_t frame,
    LibQemuArmArchTimerMMIOFrameSnapshot *snapshot)
{
    ArmArchTimerMMIOFrameSnapshot internal;

    if (snapshot == NULL ||
        !arm_arch_timer_mmio_get_frame_snapshot(
            ARM_ARCH_TIMER_MMIO(obj), frame, &internal)) {
        return false;
    }
    snapshot->count = internal.count;
    snapshot->cval = internal.cval;
    snapshot->cntfrq = internal.cntfrq;
    snapshot->ctl = internal.ctl;
    snapshot->irq_level = internal.irq_level;
    return true;
}

bool libqemu_sse_timer_get_snapshot(
    Object *counter, Object *timer, LibQemuArmSSETimerSnapshot *snapshot)
{
    ArmSSETimerSnapshot internal;

    if (snapshot == NULL ||
        !sse_timer_get_snapshot(SSE_COUNTER(counter), SSE_TIMER(timer),
                                &internal)) {
        return false;
    }
    snapshot->count = internal.count;
    snapshot->cval = internal.cval;
    snapshot->counter_frequency_hz = internal.counter_frequency_hz;
    snapshot->cntfrq = internal.cntfrq;
    snapshot->ctl = internal.ctl;
    return true;
}
