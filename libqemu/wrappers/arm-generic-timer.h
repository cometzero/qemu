/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIBQEMU_WRAPPERS_ARM_GENERIC_TIMER_H
#define LIBQEMU_WRAPPERS_ARM_GENERIC_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#define LIBQEMU_ARM_GENERIC_TIMER_COUNTER_ABI 1U
#define LIBQEMU_ARM_GENERIC_TIMER_COUNTER_SNAPSHOT_ABI 1U
#define LIBQEMU_ARM_TIMER_SNAPSHOT_ABI 1U

typedef struct LibQemuArmGenericTimerCounterSnapshot {
    uint32_t size;
    uint32_t version;
    int64_t qemu_virtual_ns;
    uint64_t count;
    uint64_t nominal_frequency_hz;
    uint64_t reported_frequency_hz;
    uint32_t enabled;
    uint32_t halted;
} LibQemuArmGenericTimerCounterSnapshot;

typedef struct LibQemuArmGenericTimerCounterCallbacks {
    uint32_t size;
    uint32_t version;
    uint64_t (*count_at_ns)(void *opaque, int64_t now_ns);
    bool (*deadline_ns)(void *opaque, uint64_t target_count,
                        int64_t from_ns, int64_t *deadline_ns);
    bool (*snapshot)(void *opaque, int64_t now_ns,
                     LibQemuArmGenericTimerCounterSnapshot *snapshot);
} LibQemuArmGenericTimerCounterCallbacks;

typedef enum LibQemuArmGenericTimerOutput {
    LIBQEMU_ARM_GENERIC_TIMER_PHYS = 0,
    LIBQEMU_ARM_GENERIC_TIMER_VIRT = 1,
    LIBQEMU_ARM_GENERIC_TIMER_HYP = 2,
    LIBQEMU_ARM_GENERIC_TIMER_SEC = 3,
    LIBQEMU_ARM_GENERIC_TIMER_HYPVIRT = 4,
    LIBQEMU_ARM_GENERIC_TIMER_S_EL2_PHYS = 5,
    LIBQEMU_ARM_GENERIC_TIMER_S_EL2_VIRT = 6,
    LIBQEMU_ARM_GENERIC_TIMER_OUTPUT_COUNT = 7,
} LibQemuArmGenericTimerOutput;

typedef struct LibQemuArmCpuGenericTimerSnapshot {
    uint32_t size;
    uint32_t version;
    int64_t qemu_virtual_ns;
    uint64_t physical_count;
    uint64_t cval;
    uint32_t cntfrq;
    uint32_t ctl;
    uint32_t irq_level;
} LibQemuArmCpuGenericTimerSnapshot;

typedef struct LibQemuArmArchTimerMMIOFrameSnapshot {
    uint32_t size;
    uint32_t version;
    int64_t qemu_virtual_ns;
    uint64_t count;
    uint64_t cval;
    uint32_t cntfrq;
    uint32_t cntacr;
    uint32_t cntpl0acr;
    uint32_t cntnsar;
    uint32_t cntnsar_implemented;
    uint32_t ctl;
    uint32_t irq_level;
    uint32_t count_accessible;
    uint32_t frequency_accessible;
    uint32_t timer_accessible;
} LibQemuArmArchTimerMMIOFrameSnapshot;

typedef struct LibQemuArmSSETimerSnapshot {
    uint32_t size;
    uint32_t version;
    int64_t qemu_virtual_ns;
    uint64_t count;
    uint64_t cval;
    uint64_t counter_frequency_hz;
    uint32_t cntfrq;
    uint32_t ctl;
    uint32_t irq_level;
} LibQemuArmSSETimerSnapshot;

#endif
