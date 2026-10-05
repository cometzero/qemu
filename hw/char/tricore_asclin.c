/*
 * TriCore TC3x ASCLIN UART, byte-oriented functional model.
 * Copyright (c) 2017 David Brenken <david.brenken@efs-auto.de>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapted from linumiz/qemu-tricore v1.0.0 (9e888198363d).
 * No TC4x registers, custom block transfers, LIN or baud timing model.
 */
#include "qemu/osdep.h"
#include "hw/char/tricore_asclin.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/bitops.h"

/* Register indices in the TC3x ASCLIN register window. */
enum {
    CLC, IOCR, ID, TXFIFOCON, RXFIFOCON, BITCON, FRAMECON, DATCON,
    BRG, BRD, LINCON, LINBTIMER, LINHTIMER, FLAGS, FLAGSSET,
    FLAGSCLEAR, FLAGSENABLE, TXDATA, RXDATA, CSR, RXDATAD,
};

#define FLAG_TC  BIT(17)
#define FLAG_RFO BIT(26)
#define FLAG_RFU BIT(27)
#define FLAG_RFL BIT(28)
#define FLAG_TFO BIT(30)
#define FLAG_TFL BIT(31)
#define FIFO_FILL_MASK (0x1f << 16)
#define FIFO_FLUSH BIT(0)
#define RX_ENABLE BIT(1)
#define RX_IRQ_MASK (BIT(2) | BIT(3) | FLAG_RFL)
#define TX_IRQ_MASK (BIT(0) | BIT(1) | FLAG_TFL)
#define ERR_IRQ_MASK (BIT(5) | BIT(6) | (0xfff << 16) | FLAG_TFO)

static void asclin_pulse_irq(TriCoreASCLINState *s, uint32_t events)
{
    events &= s->regs[FLAGSENABLE];
    if (events & RX_IRQ_MASK) {
        qemu_irq_pulse(s->irq[0]);
    }
    if (events & TX_IRQ_MASK) {
        qemu_irq_pulse(s->irq[1]);
    }
    if (events & ERR_IRQ_MASK) {
        qemu_irq_pulse(s->irq[2]);
    }
}

static void asclin_event(TriCoreASCLINState *s, uint32_t events)
{
    s->regs[FLAGS] |= events;
    asclin_pulse_irq(s, events);
}

static gboolean asclin_transmit(void *unused, GIOCondition cond, void *opaque)
{
    TriCoreASCLINState *s = opaque;

    s->watch_tag = 0;
    while (!fifo8_is_empty(&s->tx_fifo)) {
        uint8_t ch = fifo8_peek(&s->tx_fifo);
        int ret = qemu_chr_fe_write(&s->chr, &ch, 1);

        if (ret <= 0 && !(cond & G_IO_HUP)) {
            s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                                asclin_transmit, s);
            if (s->watch_tag) {
                return G_SOURCE_REMOVE;
            }
            /* An absent backend consumes output like other QEMU UARTs. */
        }
        fifo8_pop(&s->tx_fifo);
        asclin_event(s, FLAG_TFL | FLAG_TC);
    }
    return G_SOURCE_REMOVE;
}

static void asclin_cancel_watch(TriCoreASCLINState *s)
{
    if (s->watch_tag) {
        g_source_remove(s->watch_tag);
        s->watch_tag = 0;
    }
}

static uint32_t asclin_rxdata(TriCoreASCLINState *s, bool peek)
{
    uint8_t ch;

    if (fifo8_is_empty(&s->rx_fifo)) {
        if (!peek) {
            asclin_event(s, FLAG_RFU);
        }
        return 0;
    }
    if (peek) {
        return fifo8_peek(&s->rx_fifo);
    }
    ch = fifo8_pop(&s->rx_fifo);
    qemu_chr_fe_accept_input(&s->chr);
    return ch;
}

static uint64_t asclin_read(void *opaque, hwaddr offset, unsigned size)
{
    TriCoreASCLINState *s = opaque;
    unsigned reg = offset / 4;
    uint32_t value;

    switch (reg) {
    case TXFIFOCON:
        value = s->regs[reg] | (fifo8_num_used(&s->tx_fifo) << 16);
        break;
    case RXFIFOCON:
        value = s->regs[reg] | (fifo8_num_used(&s->rx_fifo) << 16);
        break;
    case FLAGSSET:
    case FLAGSCLEAR:
    case TXDATA:
        value = 0;
        break;
    case RXDATA:
    case RXDATAD:
        value = asclin_rxdata(s, reg == RXDATAD);
        break;
    case CSR:
        value = s->regs[reg];
        if ((value & 0x1f) && !(s->regs[CLC] & BIT(0))) {
            value |= BIT(31);
        }
        break;
    default:
        if (reg >= ARRAY_SIZE(s->regs)) {
            qemu_log_mask(LOG_UNIMP, "tricore-asclin: read offset 0x%"
                          HWADDR_PRIx "\n", offset);
            return 0;
        }
        value = s->regs[reg];
        break;
    }
    return value;
}

static void asclin_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    TriCoreASCLINState *s = opaque;
    unsigned reg = offset / 4;
    uint32_t data = value;

    switch (reg) {
    case CLC:
        s->regs[reg] = (data & ~BIT(1)) | ((data & BIT(0)) << 1);
        asclin_cancel_watch(s);
        if (!(data & BIT(0))) {
            asclin_transmit(NULL, G_IO_OUT, s);
            qemu_chr_fe_accept_input(&s->chr);
        }
        break;
    case ID:
    case FLAGS:
    case RXDATA:
    case RXDATAD:
        break;
    case TXFIFOCON:
        s->regs[reg] = data & ~(FIFO_FILL_MASK | FIFO_FLUSH);
        if (data & FIFO_FLUSH) {
            asclin_cancel_watch(s);
            fifo8_reset(&s->tx_fifo);
        }
        break;
    case RXFIFOCON:
        s->regs[reg] = data & ~(FIFO_FILL_MASK | FIFO_FLUSH);
        if (data & FIFO_FLUSH) {
            fifo8_reset(&s->rx_fifo);
        }
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case FLAGSSET:
        asclin_event(s, data);
        break;
    case FLAGSCLEAR:
        s->regs[FLAGS] &= ~data;
        break;
    case FLAGSENABLE: {
        uint32_t newly_enabled = data & ~s->regs[reg];
        s->regs[reg] = data;
        asclin_pulse_irq(s, newly_enabled & s->regs[FLAGS]);
        break;
    }
    case TXDATA:
        if (s->regs[CLC] & BIT(0)) {
            break;
        }
        if (fifo8_is_full(&s->tx_fifo)) {
            asclin_event(s, FLAG_TFO);
            break;
        }
        fifo8_push(&s->tx_fifo, data);
        if (!s->watch_tag) {
            asclin_transmit(NULL, G_IO_OUT, s);
        }
        break;
    case CSR:
        s->regs[reg] = data & ~BIT(31);
        break;
    default:
        if (reg >= ARRAY_SIZE(s->regs)) {
            qemu_log_mask(LOG_UNIMP, "tricore-asclin: write offset 0x%"
                          HWADDR_PRIx "\n", offset);
            return;
        }
        s->regs[reg] = data;
        break;
    }
}

static const MemoryRegionOps asclin_ops = {
    .read = asclin_read,
    .write = asclin_write,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int asclin_can_receive(void *opaque)
{
    TriCoreASCLINState *s = opaque;

    return ((s->regs[RXFIFOCON] & RX_ENABLE) &&
            !(s->regs[CLC] & BIT(0))) ?
           fifo8_num_free(&s->rx_fifo) : 0;
}

static void asclin_receive(void *opaque, const uint8_t *buf, int size)
{
    TriCoreASCLINState *s = opaque;
    unsigned count = MIN(size, fifo8_num_free(&s->rx_fifo));

    fifo8_push_all(&s->rx_fifo, buf, count);
    if (count < size) {
        asclin_event(s, FLAG_RFO);
    }
    if (count) {
        asclin_event(s, FLAG_RFL);
    }
}

static void asclin_reset(DeviceState *dev)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(dev);

    asclin_cancel_watch(s);
    memset(s->regs, 0, sizeof(s->regs));
    fifo8_reset(&s->rx_fifo);
    fifo8_reset(&s->tx_fifo);
    for (unsigned i = 0; i < ARRAY_SIZE(s->irq); i++) {
        qemu_irq_lower(s->irq[i]);
    }
}

static void asclin_realize(DeviceState *dev, Error **errp)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(dev);

    qemu_chr_fe_set_handlers(&s->chr, asclin_can_receive, asclin_receive,
                             NULL, NULL, s, NULL, true);
}

static void asclin_init(Object *obj)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(obj);

    memory_region_init_io(&s->iomem, obj, &asclin_ops, s,
                          "tricore-asclin", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    fifo8_create(&s->rx_fifo, 16);
    fifo8_create(&s->tx_fifo, 16);
}

static void asclin_finalize(Object *obj)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(obj);

    asclin_cancel_watch(s);
    fifo8_destroy(&s->rx_fifo);
    fifo8_destroy(&s->tx_fifo);
}

static const Property asclin_properties[] = {
    DEFINE_PROP_CHR("chardev", TriCoreASCLINState, chr),
};

static const VMStateDescription asclin_vmstate = {
    .name = "tricore-asclin",
    .unmigratable = true,
};

static void asclin_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = asclin_realize;
    dc->vmsd = &asclin_vmstate;
    device_class_set_legacy_reset(dc, asclin_reset);
    device_class_set_props(dc, asclin_properties);
}

static const TypeInfo asclin_info = {
    .name = TYPE_TRICORE_ASCLIN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreASCLINState),
    .instance_init = asclin_init,
    .instance_finalize = asclin_finalize,
    .class_init = asclin_class_init,
};

static void asclin_register_types(void)
{
    type_register_static(&asclin_info);
}

type_init(asclin_register_types)
