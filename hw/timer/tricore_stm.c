/*
 * TriCore TC3x System Timer.
 * Copyright (c) 2017 David Brenken
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapted from linumiz/qemu-tricore v1.0.0 (9e888198363d), with both
 * compare channels, correct TIM slices and reset/cancel support.
 * The board supplies a fixed functional clock; no SCU/PLL model is required.
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/timer/tricore_stm.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/bitops.h"

enum {
    CLC = 0x00 / 4,
    ID = 0x08 / 4,
    TIM0 = 0x10 / 4,
    TIM6 = 0x28 / 4,
    CAP = 0x2c / 4,
    CMP0 = 0x30 / 4,
    CMP1 = 0x34 / 4,
    CMCON = 0x38 / 4,
    ICR = 0x3c / 4,
    ISCR = 0x40 / 4,
    TIM0SV = 0x50 / 4,
    CAPSV = 0x54 / 4,
    ACCEN0 = 0xfc / 4,
};

#define CMP_ENABLE(n) BIT((n) * 4)
#define CMP_PENDING(n) BIT((n) * 4 + 1)
#define CMP_OUTPUT(n) BIT((n) * 4 + 2)
#define CLC_DISABLE BIT(0)

static uint64_t tricore_stm_counter(TriCoreSTMState *s)
{
    if (s->regs[CLC] & CLC_DISABLE) {
        return s->counter;
    }
    return s->counter + clock_ns_to_ticks(s->fstm,
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->base_ns);
}

static void tricore_stm_sync(TriCoreSTMState *s)
{
    s->counter = tricore_stm_counter(s);
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void tricore_stm_schedule(TriCoreSTMState *s, unsigned channel)
{
    unsigned start = extract32(s->regs[CMCON], channel * 16 + 8, 5);
    unsigned bits = extract32(s->regs[CMCON], channel * 16, 5) + 1;
    uint64_t counter, target, delta, delay;
    int64_t now;

    if (!s->timer[channel]) {
        return;
    }
    timer_del(s->timer[channel]);
    if (!clock_get(s->fstm) || (s->regs[CLC] & CLC_DISABLE) ||
        !(s->regs[ICR] & CMP_ENABLE(channel))) {
        return;
    }

    counter = tricore_stm_counter(s);
    target = deposit64(counter, 0, start + bits,
                       (uint64_t)(s->regs[CMP0 + channel] &
                                  MAKE_64BIT_MASK(0, bits)) << start);
    delta = target - counter;
    if (target <= counter) {
        delta += 1ULL << (start + bits);
    }
    /* Fractional ticks are retained between register reads. */
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    delay = MAX(clock_ticks_to_ns(s->fstm, delta), 1);
    timer_mod(s->timer[channel], delay > INT64_MAX - now ?
              INT64_MAX : now + delay);
}

static void tricore_stm_update(TriCoreSTMState *s)
{
    tricore_stm_schedule(s, 0);
    tricore_stm_schedule(s, 1);
}

static void tricore_stm_pulse(TriCoreSTMState *s, unsigned channel)
{
    if (s->regs[ICR] & CMP_ENABLE(channel)) {
        unsigned output = !!(s->regs[ICR] & CMP_OUTPUT(channel));
        qemu_irq_pulse(s->irq[output]);
    }
}

static void tricore_stm_hit(TriCoreSTMState *s, unsigned channel)
{
    s->regs[ICR] |= CMP_PENDING(channel);
    tricore_stm_pulse(s, channel);
    tricore_stm_schedule(s, channel);
}

static void tricore_stm_hit0(void *opaque)
{
    tricore_stm_hit(opaque, 0);
}

static void tricore_stm_hit1(void *opaque)
{
    tricore_stm_hit(opaque, 1);
}

static uint64_t tricore_stm_read(void *opaque, hwaddr offset, unsigned size)
{
    TriCoreSTMState *s = opaque;
    unsigned reg = offset / 4;

    if (reg >= TIM0 && reg <= TIM6) {
        static const unsigned shifts[] = { 0, 4, 8, 12, 16, 20, 32 };
        uint64_t counter = tricore_stm_counter(s);

        s->regs[CAP] = counter >> 32;
        return (uint32_t)(counter >> shifts[reg - TIM0]);
    }
    switch (reg) {
    case TIM0SV: {
        uint64_t counter = tricore_stm_counter(s);

        s->regs[CAPSV] = counter >> 32;
        return (uint32_t)counter;
    }
    case ISCR:
        return 0;
    case CLC:
    case ID:
    case CAP:
    case CAPSV:
    case CMP0:
    case CMP1:
    case CMCON:
    case ICR:
    case ACCEN0:
        return s->regs[reg];
    default:
        qemu_log_mask(LOG_UNIMP, "tricore-stm: read offset 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void tricore_stm_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    TriCoreSTMState *s = opaque;
    unsigned reg = offset / 4;
    uint32_t data = value;

    switch (reg) {
    case CLC:
        tricore_stm_sync(s);
        s->regs[reg] = (data & ~BIT(1)) | ((data & BIT(0)) << 1);
        tricore_stm_update(s);
        break;
    case CMP0:
    case CMP1:
        s->regs[reg] = data;
        tricore_stm_schedule(s, reg - CMP0);
        break;
    case CMCON:
        s->regs[reg] = data & 0x1f1f1f1f;
        tricore_stm_update(s);
        break;
    case ICR: {
        uint32_t old = s->regs[ICR];

        s->regs[ICR] = (old & (CMP_PENDING(0) | CMP_PENDING(1))) |
                      (data & 0x55);
        for (unsigned i = 0; i < 2; i++) {
            if (!(old & CMP_ENABLE(i)) && (s->regs[ICR] & CMP_PENDING(i))) {
                tricore_stm_pulse(s, i);
            }
        }
        tricore_stm_update(s);
        break;
    }
    case ISCR:
        for (unsigned i = 0; i < 2; i++) {
            if (data & BIT(i * 2)) {
                s->regs[ICR] &= ~CMP_PENDING(i);
            }
            if (data & BIT(i * 2 + 1)) {
                s->regs[ICR] |= CMP_PENDING(i);
                tricore_stm_pulse(s, i);
            }
        }
        break;
    case ACCEN0:
        /* Access protection is outside this functional subset. */
        s->regs[reg] = data;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "tricore-stm: write offset 0x%"
                      HWADDR_PRIx "\n", offset);
        break;
    }
}

static const MemoryRegionOps tricore_stm_ops = {
    .read = tricore_stm_read,
    .write = tricore_stm_write,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void tricore_stm_clock_update(void *opaque, ClockEvent event)
{
    TriCoreSTMState *s = opaque;

    if (event == ClockPreUpdate) {
        tricore_stm_sync(s);
    } else {
        tricore_stm_update(s);
    }
}

static void tricore_stm_reset(DeviceState *dev)
{
    TriCoreSTMState *s = TRICORE_STM(dev);

    for (unsigned i = 0; i < 2; i++) {
        timer_del(s->timer[i]);
        qemu_irq_lower(s->irq[i]);
    }
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[ID] = 0x0000c000;
    s->regs[ACCEN0] = UINT32_MAX;
    s->counter = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void tricore_stm_realize(DeviceState *dev, Error **errp)
{
    TriCoreSTMState *s = TRICORE_STM(dev);

    if (!clock_has_source(s->fstm)) {
        error_setg(errp, "TriCore STM requires the fstm clock input");
        return;
    }
    s->timer[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, tricore_stm_hit0, s);
    s->timer[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, tricore_stm_hit1, s);
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void tricore_stm_init(Object *obj)
{
    TriCoreSTMState *s = TRICORE_STM(obj);

    memory_region_init_io(&s->iomem, obj, &tricore_stm_ops, s,
                          "tricore-stm", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned i = 0; i < 2; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    s->fstm = qdev_init_clock_in(DEVICE(obj), "fstm", tricore_stm_clock_update,
                                 s, ClockPreUpdate | ClockUpdate);
}

static void tricore_stm_finalize(Object *obj)
{
    TriCoreSTMState *s = TRICORE_STM(obj);

    for (unsigned i = 0; i < 2; i++) {
        timer_free(s->timer[i]);
    }
}

static const VMStateDescription tricore_stm_vmstate = {
    .name = "tricore-stm",
    .unmigratable = true,
};

static void tricore_stm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tricore_stm_realize;
    dc->vmsd = &tricore_stm_vmstate;
    device_class_set_legacy_reset(dc, tricore_stm_reset);
}

static const TypeInfo tricore_stm_info = {
    .name = TYPE_TRICORE_STM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreSTMState),
    .instance_init = tricore_stm_init,
    .instance_finalize = tricore_stm_finalize,
    .class_init = tricore_stm_class_init,
};

static void tricore_stm_register_types(void)
{
    type_register_static(&tricore_stm_info);
}

type_init(tricore_stm_register_types)
