/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/arm_generic_timer_counter.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/qtest.h"

#define TYPE_ARM_GENERIC_TIMER_COUNTER_TEST \
    "arm-generic-timer-counter-test"

typedef struct ArmGenericTimerCounterTest {
    DeviceState parent_obj;
    NotifierList consumers;
    uint64_t anchor_count;
    int64_t anchor_ns;
    uint64_t visible_frequency;
} ArmGenericTimerCounterTest;

typedef struct ArmGenericTimerCounterProxy {
    Object parent_obj;
    ArmGenericTimerCounterCallbacks callbacks;
    void *opaque;
    NotifierList consumers;
} ArmGenericTimerCounterProxy;

OBJECT_DECLARE_SIMPLE_TYPE(ArmGenericTimerCounterTest,
                           ARM_GENERIC_TIMER_COUNTER_TEST)

#define TYPE_ARM_GENERIC_TIMER_COUNTER_PROXY \
    "arm-generic-timer-counter-proxy"
OBJECT_DECLARE_SIMPLE_TYPE(ArmGenericTimerCounterProxy,
                           ARM_GENERIC_TIMER_COUNTER_PROXY)

uint64_t arm_generic_timer_counter_count_at_ns(ArmGenericTimerCounter *counter,
                                                int64_t now_ns)
{
    ArmGenericTimerCounterClass *klass =
        ARM_GENERIC_TIMER_COUNTER_GET_CLASS(counter);

    return klass->count_at_ns(counter, now_ns);
}

bool arm_generic_timer_counter_deadline_ns(ArmGenericTimerCounter *counter,
                                           uint64_t target_count,
                                           int64_t from_ns,
                                           int64_t *deadline_ns)
{
    ArmGenericTimerCounterClass *klass =
        ARM_GENERIC_TIMER_COUNTER_GET_CLASS(counter);

    return klass->deadline_ns(counter, target_count, from_ns, deadline_ns);
}

bool arm_generic_timer_counter_snapshot(
    ArmGenericTimerCounter *counter, int64_t now_ns,
    ArmGenericTimerCounterSnapshot *snapshot)
{
    ArmGenericTimerCounterClass *klass =
        ARM_GENERIC_TIMER_COUNTER_GET_CLASS(counter);

    return klass->snapshot(counter, now_ns, snapshot);
}

void arm_generic_timer_counter_register_consumer(
    ArmGenericTimerCounter *counter, Notifier *notifier)
{
    ArmGenericTimerCounterClass *klass =
        ARM_GENERIC_TIMER_COUNTER_GET_CLASS(counter);

    klass->register_consumer(counter, notifier);
}

void arm_generic_timer_counter_unregister_consumer(
    ArmGenericTimerCounter *counter, Notifier *notifier)
{
    ArmGenericTimerCounterClass *klass =
        ARM_GENERIC_TIMER_COUNTER_GET_CLASS(counter);

    klass->unregister_consumer(counter, notifier);
}

static uint64_t test_counter_count_at_ns(ArmGenericTimerCounter *counter,
                                         int64_t now_ns)
{
    ArmGenericTimerCounterTest *s = ARM_GENERIC_TIMER_COUNTER_TEST(counter);

    return s->anchor_count +
           muldiv64(now_ns - s->anchor_ns, s->visible_frequency,
                    NANOSECONDS_PER_SECOND);
}

static bool test_counter_deadline_ns(ArmGenericTimerCounter *counter,
                                     uint64_t target_count, int64_t from_ns,
                                     int64_t *deadline_ns)
{
    ArmGenericTimerCounterTest *s = ARM_GENERIC_TIMER_COUNTER_TEST(counter);
    uint64_t current = test_counter_count_at_ns(counter, from_ns);
    uint64_t delta_ns;

    if (current >= target_count) {
        *deadline_ns = from_ns;
        return true;
    }

    delta_ns = muldiv64_round_up(target_count - current,
                                 NANOSECONDS_PER_SECOND,
                                 s->visible_frequency);
    if (delta_ns > INT64_MAX - from_ns) {
        return false;
    }

    *deadline_ns = from_ns + delta_ns;
    return true;
}

static bool test_counter_snapshot(ArmGenericTimerCounter *counter,
                                  int64_t now_ns,
                                  ArmGenericTimerCounterSnapshot *snapshot)
{
    ArmGenericTimerCounterTest *s = ARM_GENERIC_TIMER_COUNTER_TEST(counter);

    snapshot->qemu_virtual_ns = now_ns;
    snapshot->count = test_counter_count_at_ns(counter, now_ns);
    snapshot->nominal_frequency_hz = s->visible_frequency;
    snapshot->reported_frequency_hz = s->visible_frequency;
    snapshot->enabled = true;
    snapshot->halted = false;
    return true;
}

static void test_counter_register_consumer(ArmGenericTimerCounter *counter,
                                           Notifier *notifier)
{
    ArmGenericTimerCounterTest *s = ARM_GENERIC_TIMER_COUNTER_TEST(counter);

    notifier_list_add(&s->consumers, notifier);
}

static void test_counter_unregister_consumer(ArmGenericTimerCounter *counter,
                                             Notifier *notifier)
{
    notifier_remove(notifier);
}

static uint64_t proxy_count_at_ns(ArmGenericTimerCounter *counter,
                                  int64_t now_ns)
{
    ArmGenericTimerCounterProxy *s =
        ARM_GENERIC_TIMER_COUNTER_PROXY(counter);

    return s->callbacks.count_at_ns ?
           s->callbacks.count_at_ns(s->opaque, now_ns) : 0;
}

static bool proxy_deadline_ns(ArmGenericTimerCounter *counter,
                              uint64_t target_count, int64_t from_ns,
                              int64_t *deadline_ns)
{
    ArmGenericTimerCounterProxy *s =
        ARM_GENERIC_TIMER_COUNTER_PROXY(counter);

    return s->callbacks.deadline_ns &&
           s->callbacks.deadline_ns(s->opaque, target_count, from_ns,
                                    deadline_ns);
}

static bool proxy_snapshot(ArmGenericTimerCounter *counter, int64_t now_ns,
                           ArmGenericTimerCounterSnapshot *snapshot)
{
    ArmGenericTimerCounterProxy *s =
        ARM_GENERIC_TIMER_COUNTER_PROXY(counter);

    return s->callbacks.snapshot &&
           s->callbacks.snapshot(s->opaque, now_ns, snapshot);
}

static void proxy_register_consumer(ArmGenericTimerCounter *counter,
                                    Notifier *notifier)
{
    ArmGenericTimerCounterProxy *s =
        ARM_GENERIC_TIMER_COUNTER_PROXY(counter);

    notifier_list_add(&s->consumers, notifier);
}

static void proxy_unregister_consumer(ArmGenericTimerCounter *counter,
                                      Notifier *notifier)
{
    notifier_remove(notifier);
}

static void proxy_init(Object *obj)
{
    ArmGenericTimerCounterProxy *s = ARM_GENERIC_TIMER_COUNTER_PROXY(obj);

    notifier_list_init(&s->consumers);
}

static void proxy_finalize(Object *obj)
{
    ArmGenericTimerCounterProxy *s = ARM_GENERIC_TIMER_COUNTER_PROXY(obj);

    if (s->callbacks.free_opaque) {
        s->callbacks.free_opaque(s->opaque);
    }
}

static void proxy_class_init(ObjectClass *klass, const void *data)
{
    ArmGenericTimerCounterClass *counter_class =
        ARM_GENERIC_TIMER_COUNTER_CLASS(klass);

    counter_class->count_at_ns = proxy_count_at_ns;
    counter_class->deadline_ns = proxy_deadline_ns;
    counter_class->snapshot = proxy_snapshot;
    counter_class->register_consumer = proxy_register_consumer;
    counter_class->unregister_consumer = proxy_unregister_consumer;
}

Object *arm_generic_timer_counter_proxy_new(
    const ArmGenericTimerCounterCallbacks *callbacks, void *opaque)
{
    ArmGenericTimerCounterProxy *s = ARM_GENERIC_TIMER_COUNTER_PROXY(
        object_new(TYPE_ARM_GENERIC_TIMER_COUNTER_PROXY));

    s->callbacks = *callbacks;
    s->opaque = opaque;
    return OBJECT(s);
}

void arm_generic_timer_counter_proxy_clear(Object *obj)
{
    ArmGenericTimerCounterProxy *s = ARM_GENERIC_TIMER_COUNTER_PROXY(obj);

    if (s->callbacks.free_opaque) {
        s->callbacks.free_opaque(s->opaque);
    }
    memset(&s->callbacks, 0, sizeof(s->callbacks));
    s->opaque = NULL;
    notifier_list_notify(&s->consumers, NULL);
}

void arm_generic_timer_counter_notify(Object *obj)
{
    ArmGenericTimerCounterProxy *s = ARM_GENERIC_TIMER_COUNTER_PROXY(obj);

    notifier_list_notify(&s->consumers, NULL);
}

static void test_counter_get_count(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    ArmGenericTimerCounterTest *s = ARM_GENERIC_TIMER_COUNTER_TEST(obj);
    uint64_t value = test_counter_count_at_ns(
        ARM_GENERIC_TIMER_COUNTER(s),
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    visit_type_uint64(v, name, &value, errp);
}

static void test_counter_set_count(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    ArmGenericTimerCounterTest *s = ARM_GENERIC_TIMER_COUNTER_TEST(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }

    s->anchor_count = value;
    s->anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    notifier_list_notify(&s->consumers, NULL);
}

static void test_counter_realize(DeviceState *dev, Error **errp)
{
    ArmGenericTimerCounterTest *s = ARM_GENERIC_TIMER_COUNTER_TEST(dev);

    if (!qtest_enabled()) {
        error_setg(errp, TYPE_ARM_GENERIC_TIMER_COUNTER_TEST
                   " is available only under qtest");
        return;
    }
    if (!s->visible_frequency || s->visible_frequency > UINT32_MAX) {
        error_setg(errp, "visible-frequency must be 1..%u", UINT32_MAX);
        return;
    }
    s->anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void test_counter_init(Object *obj)
{
    ArmGenericTimerCounterTest *s = ARM_GENERIC_TIMER_COUNTER_TEST(obj);

    notifier_list_init(&s->consumers);
    object_property_add(obj, "count", "uint64", test_counter_get_count,
                        test_counter_set_count, NULL, NULL);
}

static const Property test_counter_properties[] = {
    DEFINE_PROP_UINT64("visible-frequency", ArmGenericTimerCounterTest,
                       visible_frequency, 100000000),
};

static void test_counter_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ArmGenericTimerCounterClass *counter_class =
        ARM_GENERIC_TIMER_COUNTER_CLASS(klass);

    dc->realize = test_counter_realize;
    device_class_set_props(dc, test_counter_properties);
    counter_class->count_at_ns = test_counter_count_at_ns;
    counter_class->deadline_ns = test_counter_deadline_ns;
    counter_class->snapshot = test_counter_snapshot;
    counter_class->register_consumer = test_counter_register_consumer;
    counter_class->unregister_consumer = test_counter_unregister_consumer;
}

static const TypeInfo counter_interface_info = {
    .name = TYPE_ARM_GENERIC_TIMER_COUNTER,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(ArmGenericTimerCounterClass),
};

static const TypeInfo test_counter_info = {
    .name = TYPE_ARM_GENERIC_TIMER_COUNTER_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ArmGenericTimerCounterTest),
    .instance_init = test_counter_init,
    .class_init = test_counter_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_ARM_GENERIC_TIMER_COUNTER },
        { }
    },
};

static const TypeInfo proxy_info = {
    .name = TYPE_ARM_GENERIC_TIMER_COUNTER_PROXY,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(ArmGenericTimerCounterProxy),
    .instance_init = proxy_init,
    .instance_finalize = proxy_finalize,
    .class_init = proxy_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_ARM_GENERIC_TIMER_COUNTER },
        { }
    },
};

static void arm_generic_timer_counter_register_types(void)
{
    type_register_static(&counter_interface_info);
    type_register_static(&test_counter_info);
    type_register_static(&proxy_info);
}

type_init(arm_generic_timer_counter_register_types)
