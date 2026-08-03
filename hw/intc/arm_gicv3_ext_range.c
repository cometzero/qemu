/*
 * ARM GICv3 extended interrupt range state and migration
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/intc/gicv3_internal.h"
#include "migration/vmstate.h"

bool gicv3_intid_to_irq(const GICv3State *s, uint32_t intid,
                        uint32_t cpu, GICv3IRQ *out)
{
    GICv3IRQ irq = {
        .intid = intid,
        .cpu = UINT32_MAX,
    };

    if (intid >= GIC_INTERNAL && intid < s->num_irq) {
        irq.type = GICV3_IRQ_SPI;
        irq.index = intid;
    } else if (intid >= GICV3_ESPI_INTID_START &&
               intid - GICV3_ESPI_INTID_START < s->num_espi) {
        irq.type = GICV3_IRQ_ESPI;
        irq.index = intid - GICV3_ESPI_INTID_START;
    } else if (intid < GIC_INTERNAL && cpu < s->num_cpu) {
        irq.type = GICV3_IRQ_PPI;
        irq.index = intid;
        irq.cpu = cpu;
    } else if (intid >= GICV3_EPPI_INTID_START &&
               intid - GICV3_EPPI_INTID_START < s->num_eppi &&
               cpu < s->num_cpu) {
        irq.type = GICV3_IRQ_EPPI;
        irq.index = intid - GICV3_EPPI_INTID_START;
        irq.cpu = cpu;
    } else {
        return false;
    }

    if (out) {
        *out = irq;
    }
    return true;
}

bool gicv3_gpio_count(const GICv3State *s, int *count)
{
    uint64_t total = s->num_irq - GIC_INTERNAL;

    total += s->num_espi;
    total += (uint64_t)GIC_INTERNAL * s->num_cpu;
    total += (uint64_t)s->num_eppi * s->num_cpu;
    if (total > INT_MAX) {
        return false;
    }

    *count = total;
    return true;
}

bool gicv3_gpio_to_irq(const GICv3State *s, uint32_t gpio, GICv3IRQ *out)
{
    uint64_t offset = gpio;
    uint64_t count = s->num_irq - GIC_INTERNAL;

    if (offset < count) {
        return gicv3_intid_to_irq(s, offset + GIC_INTERNAL, 0, out);
    }
    offset -= count;

    count = s->num_espi;
    if (offset < count) {
        return gicv3_intid_to_irq(s, offset + GICV3_ESPI_INTID_START,
                                  0, out);
    }
    offset -= count;

    count = (uint64_t)GIC_INTERNAL * s->num_cpu;
    if (offset < count) {
        return gicv3_intid_to_irq(s, offset % GIC_INTERNAL,
                                  offset / GIC_INTERNAL, out);
    }
    offset -= count;

    count = (uint64_t)s->num_eppi * s->num_cpu;
    if (offset < count) {
        return gicv3_intid_to_irq(s,
                                  GICV3_EPPI_INTID_START +
                                  offset % s->num_eppi,
                                  offset / s->num_eppi, out);
    }

    return false;
}

/* Process a change in an external IRQ input. */
void gicv3_set_irq(void *opaque, int irq, int level)
{
    GICv3State *s = opaque;
    GICv3IRQ decoded;

    g_assert(gicv3_gpio_to_irq(s, irq, &decoded));
    switch (decoded.type) {
    case GICV3_IRQ_SPI:
        gicv3_dist_set_irq(s, decoded.intid, level);
        break;
    case GICV3_IRQ_ESPI:
        if (level == test_bit32(decoded.index, s->espi_level)) {
            return;
        }
        gic_bmp_replace_bit(decoded.index, s->espi_level, level);
        if (level && test_bit32(decoded.index, s->espi_edge_trigger)) {
            set_bit32(decoded.index, s->espi_pending);
        }
        gicv3_full_update(s);
        break;
    case GICV3_IRQ_PPI:
        g_assert(decoded.intid >= GIC_NR_SGIS);
        gicv3_redist_set_irq(&s->cpu[decoded.cpu], decoded.intid, level);
        break;
    case GICV3_IRQ_EPPI:
        if (level == test_bit32(decoded.index,
                                s->cpu[decoded.cpu].eppi_level)) {
            return;
        }
        gic_bmp_replace_bit(decoded.index,
                            s->cpu[decoded.cpu].eppi_level, level);
        if (level && test_bit32(decoded.index,
                                s->cpu[decoded.cpu].eppi_edge_trigger)) {
            set_bit32(decoded.index, s->cpu[decoded.cpu].eppi_pending);
        }
        gicv3_redist_update(&s->cpu[decoded.cpu]);
        break;
    default:
        g_assert_not_reached();
    }
}

void gicv3_ext_range_reset(GICv3State *s)
{
    uint32_t i;

    memset(s->espi_group, 0, sizeof(s->espi_group));
    memset(s->espi_grpmod, 0, sizeof(s->espi_grpmod));
    memset(s->espi_enabled, 0, sizeof(s->espi_enabled));
    memset(s->espi_pending, 0, sizeof(s->espi_pending));
    memset(s->espi_active, 0, sizeof(s->espi_active));
    memset(s->espi_level, 0, sizeof(s->espi_level));
    memset(s->espi_edge_trigger, 0, sizeof(s->espi_edge_trigger));
    memset(s->espi_nmi, 0, sizeof(s->espi_nmi));
    memset(s->espi_priority, 0, sizeof(s->espi_priority));
    memset(s->espi_irouter, 0, sizeof(s->espi_irouter));
    memset(s->espi_irouter_target, 0, sizeof(s->espi_irouter_target));
    memset(s->espi_nsacr, 0, sizeof(s->espi_nsacr));

    if (!s->cpu) {
        return;
    }
    for (i = 0; i < s->num_cpu; i++) {
        GICv3CPUState *cs = &s->cpu[i];

        memset(cs->eppi_level, 0, sizeof(cs->eppi_level));
        memset(cs->eppi_group, 0, sizeof(cs->eppi_group));
        memset(cs->eppi_enabled, 0, sizeof(cs->eppi_enabled));
        memset(cs->eppi_pending, 0, sizeof(cs->eppi_pending));
        memset(cs->eppi_active, 0, sizeof(cs->eppi_active));
        memset(cs->eppi_edge_trigger, 0, sizeof(cs->eppi_edge_trigger));
        memset(cs->eppi_grpmod, 0, sizeof(cs->eppi_grpmod));
        memset(cs->eppi_nmi, 0, sizeof(cs->eppi_nmi));
        memset(cs->eppi_priority, 0, sizeof(cs->eppi_priority));
    }
}

static bool gicv3_espi_needed(void *opaque)
{
    GICv3State *s = opaque;

    return s->num_espi != 0;
}

const VMStateDescription vmstate_gicv3_espi = {
    .name = "arm_gicv3/espi",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = gicv3_espi_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(num_espi, GICv3State, NULL),
        VMSTATE_UINT32_ARRAY(espi_group, GICv3State, GICV3_ESPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(espi_grpmod, GICv3State, GICV3_ESPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(espi_enabled, GICv3State, GICV3_ESPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(espi_pending, GICv3State, GICV3_ESPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(espi_active, GICv3State, GICV3_ESPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(espi_level, GICv3State, GICV3_ESPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(espi_edge_trigger, GICv3State,
                             GICV3_ESPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(espi_nmi, GICv3State, GICV3_ESPI_BMP_SIZE),
        VMSTATE_UINT8_ARRAY(espi_priority, GICv3State, GICV3_MAX_ESPI),
        VMSTATE_UINT64_ARRAY(espi_irouter, GICv3State, GICV3_MAX_ESPI),
        VMSTATE_UINT32_ARRAY(espi_nsacr, GICv3State,
                             DIV_ROUND_UP(GICV3_MAX_ESPI, 16)),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_gicv3_eppi_cpu = {
    .name = "arm_gicv3_eppi_cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(eppi_level, GICv3CPUState,
                             GICV3_EPPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(eppi_group, GICv3CPUState,
                             GICV3_EPPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(eppi_enabled, GICv3CPUState,
                             GICV3_EPPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(eppi_pending, GICv3CPUState,
                             GICV3_EPPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(eppi_active, GICv3CPUState,
                             GICV3_EPPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(eppi_edge_trigger, GICv3CPUState,
                             GICV3_EPPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(eppi_grpmod, GICv3CPUState,
                             GICV3_EPPI_BMP_SIZE),
        VMSTATE_UINT32_ARRAY(eppi_nmi, GICv3CPUState,
                             GICV3_EPPI_BMP_SIZE),
        VMSTATE_UINT8_ARRAY(eppi_priority, GICv3CPUState, GICV3_MAX_EPPI),
        VMSTATE_END_OF_LIST()
    }
};

static bool gicv3_eppi_needed(void *opaque)
{
    GICv3State *s = opaque;

    return s->num_eppi != 0;
}

const VMStateDescription vmstate_gicv3_eppi = {
    .name = "arm_gicv3/eppi",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = gicv3_eppi_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(num_eppi, GICv3State, NULL),
        VMSTATE_UINT32_EQUAL(num_cpu, GICv3State, NULL),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(cpu, GICv3State, num_cpu,
                                             vmstate_gicv3_eppi_cpu,
                                             GICv3CPUState),
        VMSTATE_END_OF_LIST()
    }
};
