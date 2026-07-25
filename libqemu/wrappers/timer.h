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

#ifndef _LIBQEMU_WRAPPERS_TIMER_H
#define _LIBQEMU_WRAPPERS_TIMER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct QemuTimer QemuTimer;
typedef struct Object Object;

typedef struct LibQemuArmArchTimerMMIOFrameSnapshot {
    uint64_t count;
    uint64_t cval;
    uint32_t cntfrq;
    uint32_t ctl;
    uint32_t irq_level;
} LibQemuArmArchTimerMMIOFrameSnapshot;

typedef struct LibQemuArmSSETimerSnapshot {
    uint64_t count;
    uint64_t cval;
    uint64_t counter_frequency_hz;
    uint32_t cntfrq;
    uint32_t ctl;
} LibQemuArmSSETimerSnapshot;

typedef void (*LibQemuTimerCb)(void *);

int64_t libqemu_clock_virtual_get_ns(void);
QemuTimer *libqemu_timer_new_virtual_ns(LibQemuTimerCb cb, void *opaque);
void libqemu_sse_counter_set_snapshot(Object *counter, uint64_t count,
                                      bool running);
uint64_t libqemu_sse_counter_get_value(Object *counter);
void libqemu_arm_arch_timer_mmio_set_snapshot(
    Object *timer, uint64_t count, bool running, uint32_t frequency_hz);
uint64_t libqemu_arm_arch_timer_mmio_get_value(Object *timer);
bool libqemu_arm_arch_timer_mmio_get_frame_snapshot(
    Object *timer, uint32_t frame,
    LibQemuArmArchTimerMMIOFrameSnapshot *snapshot);
bool libqemu_sse_timer_get_snapshot(
    Object *counter, Object *timer, LibQemuArmSSETimerSnapshot *snapshot);


#endif
