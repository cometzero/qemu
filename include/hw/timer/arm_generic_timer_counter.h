/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_ARM_GENERIC_TIMER_COUNTER_H
#define HW_TIMER_ARM_GENERIC_TIMER_COUNTER_H

#include "qemu/notify.h"
#include "qom/object.h"

typedef struct ArmGenericTimerCounterSnapshot {
    int64_t qemu_virtual_ns;
    uint64_t count;
    uint64_t nominal_frequency_hz;
    uint64_t reported_frequency_hz;
    bool enabled;
    bool halted;
} ArmGenericTimerCounterSnapshot;

typedef struct ArmGenericTimerCounterCallbacks {
    uint64_t (*count_at_ns)(void *opaque, int64_t now_ns);
    bool (*deadline_ns)(void *opaque, uint64_t target_count,
                        int64_t from_ns, int64_t *deadline_ns);
    bool (*snapshot)(void *opaque, int64_t now_ns,
                     ArmGenericTimerCounterSnapshot *snapshot);
    void (*free_opaque)(void *opaque);
} ArmGenericTimerCounterCallbacks;

#define TYPE_ARM_GENERIC_TIMER_COUNTER "arm-generic-timer-counter"
OBJECT_DECLARE_TYPE(ArmGenericTimerCounter, ArmGenericTimerCounterClass,
                    ARM_GENERIC_TIMER_COUNTER)

struct ArmGenericTimerCounterClass {
    InterfaceClass parent_class;

    uint64_t (*count_at_ns)(ArmGenericTimerCounter *counter, int64_t now_ns);
    bool (*deadline_ns)(ArmGenericTimerCounter *counter, uint64_t target_count,
                        int64_t from_ns, int64_t *deadline_ns);
    bool (*snapshot)(ArmGenericTimerCounter *counter, int64_t now_ns,
                     ArmGenericTimerCounterSnapshot *snapshot);
    void (*register_consumer)(ArmGenericTimerCounter *counter,
                              Notifier *notifier);
    void (*unregister_consumer)(ArmGenericTimerCounter *counter,
                                Notifier *notifier);
};

struct ArmGenericTimerCounter {
    Object parent_obj;
};

uint64_t arm_generic_timer_counter_count_at_ns(ArmGenericTimerCounter *counter,
                                                int64_t now_ns);
bool arm_generic_timer_counter_deadline_ns(ArmGenericTimerCounter *counter,
                                           uint64_t target_count,
                                           int64_t from_ns,
                                           int64_t *deadline_ns);
bool arm_generic_timer_counter_snapshot(
    ArmGenericTimerCounter *counter, int64_t now_ns,
    ArmGenericTimerCounterSnapshot *snapshot);
void arm_generic_timer_counter_register_consumer(
    ArmGenericTimerCounter *counter, Notifier *notifier);
void arm_generic_timer_counter_unregister_consumer(
    ArmGenericTimerCounter *counter, Notifier *notifier);
Object *arm_generic_timer_counter_proxy_new(
    const ArmGenericTimerCounterCallbacks *callbacks, void *opaque);
void arm_generic_timer_counter_proxy_clear(Object *obj);
void arm_generic_timer_counter_notify(Object *obj);

#endif
