/*
 * TriCore TC3x interrupt router, CPU0 service provider only.
 * Copyright (c) 2017 David Brenken <david.brenken@efs-auto.de>
 * Copyright (c) 2026 Parthiban Nallathambi <parthiban@linumiz.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapted from linumiz/qemu-tricore v1.0.0 (9e888198363d): TC3x only,
 * with priority arbitration, request status preservation and reset support.
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/tricore_ir.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

static void tricore_ir_evaluate(TriCoreIRState *s)
{
    unsigned winner = 0;
    unsigned priority = 0;

    for (unsigned i = 0; i < s->num_irqs; i++) {
        uint32_t src = s->src_regs[i];
        unsigned pn = FIELD_EX32(src, SRC, SRPN);

        /* CPU0 only. Priority zero cannot interrupt a TriCore CPU. */
        if ((src & R_SRC_SRR_MASK) && (src & R_SRC_TC3X_SRE_MASK) &&
            FIELD_EX32(src, SRC_TC3X, TOS) == 0 && pn > priority) {
            priority = pn;
            winner = i;
        }
    }

    /* Equal priorities use the lowest SRC index deterministically. */
    s->lwsr[0] = priority ? FIELD_DP32(0, LWSR, PN, priority) |
                           FIELD_DP32(0, LWSR, ID, winner) |
                           R_LWSR_VALID_MASK | R_LWSR_STAT_MASK : 0;
    qemu_set_irq(s->isp_irq, priority != 0);
    qemu_set_irq(s->priority, priority);
}

static void tricore_ir_request(TriCoreIRState *s, unsigned irq)
{
    if (s->src_regs[irq] & R_SRC_SRR_MASK) {
        s->src_regs[irq] |= R_SRC_IOV_MASK;
    }
    s->src_regs[irq] |= R_SRC_SRR_MASK;
}

static void tricore_ir_input(void *opaque, int irq, int level)
{
    TriCoreIRState *s = opaque;

    /* Peripheral service requests are pulses, latched until acknowledged. */
    if (level) {
        tricore_ir_request(s, irq);
        tricore_ir_evaluate(s);
    }
}

void tricore_ir_irq_acknowledge(TriCoreIRState *s, uint16_t irq, uint8_t vm)
{
    if (vm != 0 || irq >= s->num_irqs) {
        return;
    }
    s->lasr = s->lwsr[0] | R_LASR_ENTER_MASK;
    s->src_regs[irq] &= ~R_SRC_SRR_MASK;
    tricore_ir_evaluate(s);
}

static void tricore_ir_ack(void *opaque, int n, int level)
{
    TriCoreIRState *s = opaque;

    if (level && (s->lwsr[0] & R_LWSR_VALID_MASK)) {
        tricore_ir_irq_acknowledge(s, FIELD_EX32(s->lwsr[0], LWSR, ID), 0);
    }
}

static uint64_t tricore_ir_src_read(void *opaque, hwaddr offset, unsigned size)
{
    TriCoreIRState *s = opaque;
    unsigned irq = offset / 4;

    return irq < s->num_irqs ?
        s->src_regs[irq] >> ((offset & 3) * 8) : 0;
}

static void tricore_ir_src_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    TriCoreIRState *s = opaque;
    unsigned irq = offset / 4;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = MAKE_64BIT_MASK(shift, size * 8);
    uint32_t data = value << shift;
    uint32_t config = R_SRC_SRPN_MASK | R_SRC_TC3X_SRE_MASK |
                      R_SRC_TC3X_TOS_MASK;

    if (irq >= s->num_irqs) {
        return;
    }
    s->src_regs[irq] = (s->src_regs[irq] & ~(mask & config)) |
                      (data & config);
    if (data & R_SRC_IOVCLR_MASK) {
        s->src_regs[irq] &= ~R_SRC_IOV_MASK;
    }
    if (data & R_SRC_TC3X_SWSCLR_MASK) {
        s->src_regs[irq] &= ~R_SRC_TC3X_SWS_MASK;
    }
    /* Setting both request command bits leaves the pending state unchanged. */
    if ((data & (R_SRC_CLRR_MASK | R_SRC_SETR_MASK)) == R_SRC_CLRR_MASK) {
        s->src_regs[irq] &= ~R_SRC_SRR_MASK;
    } else if ((data & (R_SRC_CLRR_MASK | R_SRC_SETR_MASK)) ==
               R_SRC_SETR_MASK) {
        tricore_ir_request(s, irq);
        s->src_regs[irq] |= R_SRC_TC3X_SWS_MASK;
    }
    tricore_ir_evaluate(s);
}

static const MemoryRegionOps tricore_ir_src_ops = {
    .read = tricore_ir_src_read,
    .write = tricore_ir_src_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t tricore_ir_int_read(void *opaque, hwaddr offset, unsigned size)
{
    TriCoreIRState *s = opaque;

    switch (offset) {
    case 0x08:
        return 0x00b9c013;
    case 0x200:
        return s->lwsr[0];
    case 0x204:
        return s->lasr;
    default:
        return 0;
    }
}

static void tricore_ir_int_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    /* Implemented arbitration status registers are read-only. */
}

static const MemoryRegionOps tricore_ir_int_ops = {
    .read = tricore_ir_int_read,
    .write = tricore_ir_int_write,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void tricore_ir_reset(DeviceState *dev)
{
    TriCoreIRState *s = TRICORE_IR(dev);

    memset(s->src_regs, 0, sizeof(s->src_regs));
    memset(s->lwsr, 0, sizeof(s->lwsr));
    s->lasr = 0;
    tricore_ir_evaluate(s);
}

static void tricore_ir_realize(DeviceState *dev, Error **errp)
{
    TriCoreIRState *s = TRICORE_IR(dev);

    if (s->num_isps != 1 || !s->num_irqs ||
        s->num_irqs > ARRAY_SIZE(s->src_regs)) {
        error_setg(errp, "TriCore IR requires one ISP and 1..512 sources");
        return;
    }
    qdev_init_gpio_in_named(dev, tricore_ir_input, "irq", s->num_irqs);
    qdev_init_gpio_in_named(dev, tricore_ir_ack, "ack", 1);
    qdev_init_gpio_out_named(dev, &s->isp_irq, "isp", 1);
    qdev_init_gpio_out_named(dev, &s->priority, "priority", 1);
}

static void tricore_ir_init(Object *obj)
{
    TriCoreIRState *s = TRICORE_IR(obj);

    memory_region_init_io(&s->int_region, obj, &tricore_ir_int_ops, s,
                          "tricore-ir.int", 0x1000);
    memory_region_init_io(&s->src_region, obj, &tricore_ir_src_ops, s,
                          "tricore-ir.src", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->int_region);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->src_region);
}

static const Property tricore_ir_properties[] = {
    DEFINE_PROP_UINT8("num-isps", TriCoreIRState, num_isps, 1),
    DEFINE_PROP_UINT16("num-irqs", TriCoreIRState, num_irqs, 256),
};

static const VMStateDescription tricore_ir_vmstate = {
    .name = "tricore-ir",
    .unmigratable = true,
};

static void tricore_ir_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tricore_ir_realize;
    dc->vmsd = &tricore_ir_vmstate;
    device_class_set_legacy_reset(dc, tricore_ir_reset);
    device_class_set_props(dc, tricore_ir_properties);
}

static const TypeInfo tricore_ir_info = {
    .name = TYPE_TRICORE_IR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreIRState),
    .instance_init = tricore_ir_init,
    .class_init = tricore_ir_class_init,
};

static void tricore_ir_register_types(void)
{
    type_register_static(&tricore_ir_info);
}

type_init(tricore_ir_register_types)
