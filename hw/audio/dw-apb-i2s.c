/*
 * Synopsys DesignWare APB I2S functional model.
 * Port of the Apollo QBox BSD-licensed dw-apb-i2s SystemC model.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/audio/dw-apb-i2s.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define FIFO_DEPTH 16
#define ISR_RXDA (1u << 0)
#define ISR_RXFO (1u << 1)
#define ISR_TXFE (1u << 4)
#define ISR_TXFO (1u << 5)
#define DMACR_RXBLOCK (1u << 16)
#define DMACR_TXBLOCK (1u << 17)
#define COMPONENT_VERSION 0x3131312a
#define COMPONENT_TYPE 0x445701a0
#define DMA_ACTIVE 4

enum {
    IER = 0x000,
    IRER = 0x004,
    ITER = 0x008,
    CER = 0x00c,
    CCR = 0x010,
    RXFFR = 0x014,
    TXFFR = 0x018,
    LRBR_LTHR0 = 0x020,
    RRBR_RTHR0 = 0x024,
    RER0 = 0x028,
    TER0 = 0x02c,
    RCR0 = 0x030,
    TCR0 = 0x034,
    ISR0 = 0x038,
    IMR0 = 0x03c,
    ROR0 = 0x040,
    TOR0 = 0x044,
    RFCR0 = 0x048,
    TFCR0 = 0x04c,
    RFF0 = 0x050,
    TFF0 = 0x054,
    RXDMA = 0x1c0,
    RRXDMA = 0x1c4,
    TXDMA = 0x1c8,
    RTXDMA = 0x1cc,
    COMP_PARAM_2 = 0x1f0,
    COMP_PARAM_1 = 0x1f4,
    COMP_VERSION = 0x1f8,
    COMP_TYPE = 0x1fc,
    DMACR = 0x200,
};

typedef struct I2SFrame {
    uint32_t left, right;
} I2SFrame;

typedef struct I2SFifo {
    I2SFrame frames[FIFO_DEPTH];
    unsigned head, count;
} I2SFifo;

typedef struct I2SRequest {
    qemu_irq irq;
    bool asserted, wait_ack_low;
    uint32_t ack;
} I2SRequest;

struct DWAPBI2SState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    QEMUBH *bh;
    DWAPBI2SState *peer;
    bool master_mode, transmitter_enabled, receiver_enabled;
    bool functional_pacing;
    uint64_t frame_period_ns;
    I2SFifo tx_fifo, rx_fifo;
    I2SRequest dma_tx, dma_rx;
    uint32_t ccr, rcr, tcr, imr, rfcr, tfcr, dmacr;
    uint32_t tx_pio_left, tx_dma_left;
    bool tx_left_valid, tx_pio_drop_right, rx_pio_left;
    bool tx_dma_left_valid, tx_dma_drop_right, rx_dma_left;
    bool tx_overrun, rx_overrun;
    bool global_enabled, rx_block_enabled, tx_block_enabled;
    bool clock_enabled, rx_channel_enabled, tx_channel_enabled;
    bool dma_tx_force_idle, dma_rx_force_idle;
};

static void fifo_clear(I2SFifo *f)
{
    f->head = f->count = 0;
}

static I2SFrame *fifo_front(I2SFifo *f)
{
    return &f->frames[f->head];
}

static void fifo_push(I2SFifo *f, uint32_t left, uint32_t right)
{
    unsigned tail = (f->head + f->count++) % FIFO_DEPTH;
    f->frames[tail] = (I2SFrame) { left, right };
}

static void fifo_pop(I2SFifo *f)
{
    f->head = (f->head + 1) % FIFO_DEPTH;
    f->count--;
}

static bool tx_active(DWAPBI2SState *s)
{
    return s->transmitter_enabled && s->global_enabled &&
        s->tx_block_enabled && s->tx_channel_enabled &&
        (!s->master_mode || s->clock_enabled);
}

static bool rx_active(DWAPBI2SState *s)
{
    return s->receiver_enabled && s->global_enabled &&
        s->rx_block_enabled && s->rx_channel_enabled;
}

static void state_changed(DWAPBI2SState *s)
{
    qemu_bh_schedule(s->bh);
    if (tx_active(s) && (!s->functional_pacing || s->tx_fifo.count)) {
        if (!timer_pending(s->timer)) {
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      MAX(1, s->frame_period_ns));
        }
    } else {
        timer_del(s->timer);
    }
}

/* Four-phase single-transfer handshake, matching QBox dma-trigger.h. */
static void request_update(I2SRequest *r, bool needed, bool force_idle)
{
    bool ack = r->ack & DMA_ACTIVE;
    if (force_idle) {
        r->asserted = false;
        r->wait_ack_low = ack;
        qemu_set_irq(r->irq, 0);
    }
    if (r->asserted) {
        if (ack) {
            r->asserted = false;
            r->wait_ack_low = true;
            qemu_set_irq(r->irq, 0);
        }
        return;
    }
    if (r->wait_ack_low) {
        if (ack) {
            return;
        }
        r->wait_ack_low = false;
    }
    if (needed && !ack) {
        r->asserted = true;
        qemu_set_irq(r->irq, DMA_ACTIVE);
    }
}
static uint32_t component_parameter_1(DWAPBI2SState *s)
{
    return (4u << 16) | (s->receiver_enabled ? 1u << 6 : 0) |
        (s->transmitter_enabled ? 1u << 5 : 0) |
        (s->master_mode ? 1u << 4 : 0) | (3u << 2) | 2;
}

static uint32_t interrupt_status(DWAPBI2SState *s)
{
    uint32_t status = 0;
    if (s->receiver_enabled &&
        s->rx_fifo.count >= MIN(FIFO_DEPTH, (s->rfcr & 0xf) + 1)) {
        status |= ISR_RXDA;
    }
    if (s->rx_overrun) {
        status |= ISR_RXFO;
    }
    if (s->transmitter_enabled &&
        s->tx_fifo.count <= MIN(FIFO_DEPTH - 1, s->tfcr & 0xf)) {
        status |= ISR_TXFE;
    }
    if (s->tx_overrun) {
        status |= ISR_TXFO;
    }
    return status;
}

static void write_tx_left(DWAPBI2SState *s, uint32_t value)
{
    if (s->tx_fifo.count == FIFO_DEPTH) {
        s->tx_overrun = true;
        s->tx_left_valid = false;
        s->tx_pio_drop_right = true;
    } else {
        s->tx_pio_left = value;
        s->tx_left_valid = true;
        s->tx_pio_drop_right = false;
    }
    state_changed(s);
}

static void write_tx_right(DWAPBI2SState *s, uint32_t value)
{
    if (s->tx_pio_drop_right) {
        s->tx_pio_drop_right = false;
        state_changed(s);
        return;
    }
    if (!s->tx_left_valid) {
        return;
    }

    if (s->tx_fifo.count == FIFO_DEPTH) {
        s->tx_overrun = true;
    } else {
        fifo_push(&s->tx_fifo, s->tx_pio_left, value);
    }
    s->tx_left_valid = false;
    state_changed(s);
}

static void write_tx_dma(DWAPBI2SState *s, uint32_t value)
{
    if (!s->tx_dma_left_valid) {
        if (s->tx_fifo.count == FIFO_DEPTH) {
            s->tx_overrun = true;
            s->tx_dma_drop_right = true;
        } else {
            s->tx_dma_left = value;
            s->tx_dma_drop_right = false;
        }
        s->tx_dma_left_valid = true;
        state_changed(s);
        return;
    }

    if (!s->tx_dma_drop_right) {
        if (s->tx_fifo.count == FIFO_DEPTH) {
            s->tx_overrun = true;
        } else {
            fifo_push(&s->tx_fifo, s->tx_dma_left, value);
        }
    }
    s->tx_dma_left_valid = false;
    s->tx_dma_drop_right = false;
    state_changed(s);
}

static uint32_t read_rx_sample(DWAPBI2SState *s, bool dma)
{
    bool *left = dma ? &s->rx_dma_left : &s->rx_pio_left;
    uint32_t value;

    if (!s->rx_fifo.count) {
        return 0;
    }
    value = *left ? fifo_front(&s->rx_fifo)->left :
                    fifo_front(&s->rx_fifo)->right;
    if (*left) {
        *left = false;
    } else {
        *left = true;
        fifo_pop(&s->rx_fifo);
        state_changed(s);
    }
    return value;
}

static void write_register(DWAPBI2SState *s, uint32_t offset, uint32_t value)
{
    switch (offset) {
    case IER:
        s->global_enabled = value & 1;
        if (!s->global_enabled) {
            fifo_clear(&s->tx_fifo);
            fifo_clear(&s->rx_fifo);
            s->tx_left_valid = false;
            s->tx_pio_drop_right = false;
            s->tx_dma_left_valid = false;
            s->tx_dma_drop_right = false;
            s->rx_pio_left = true;
            s->rx_dma_left = true;
            s->tx_overrun = false;
            s->rx_overrun = false;
            s->dma_tx_force_idle = true;
            s->dma_rx_force_idle = true;
        }
        break;
    case IRER:
        s->rx_block_enabled = value & 1;
        if (!s->rx_block_enabled) {
            s->dma_rx_force_idle = true;
        }
        break;
    case ITER:
        s->tx_block_enabled = value & 1;
        if (!s->tx_block_enabled) {
            s->dma_tx_force_idle = true;
        }
        break;
    case CER:
        s->clock_enabled = value & 1;
        break;
    case CCR:
        if (!s->clock_enabled) {
            s->ccr = value & 0x1f;
        }
        break;
    case RXFFR:
        if ((value & 1) && !s->rx_block_enabled) {
            fifo_clear(&s->rx_fifo);
            s->rx_pio_left = true;
            s->rx_dma_left = true;
        }
        break;
    case TXFFR:
        if ((value & 1) && !s->tx_block_enabled) {
            fifo_clear(&s->tx_fifo);
            s->tx_left_valid = false;
            s->tx_pio_drop_right = false;
            s->tx_dma_left_valid = false;
            s->tx_dma_drop_right = false;
        }
        break;
    case LRBR_LTHR0:
        if (s->transmitter_enabled) {
            write_tx_left(s, value);
        }
        return;
    case RRBR_RTHR0:
        if (s->transmitter_enabled) {
            write_tx_right(s, value);
        }
        return;
    case RER0:
        s->rx_channel_enabled = value & 1;
        if (!s->rx_channel_enabled) {
            s->dma_rx_force_idle = true;
        }
        break;
    case TER0:
        s->tx_channel_enabled = value & 1;
        if (!s->tx_channel_enabled) {
            s->dma_tx_force_idle = true;
        }
        break;
    case RCR0:
        if (!s->rx_channel_enabled) {
            s->rcr = (value & 7) <= 5 ? value & 7 : 5;
        }
        break;
    case TCR0:
        if (!s->tx_channel_enabled) {
            s->tcr = (value & 7) <= 5 ? value & 7 : 5;
        }
        break;
    case IMR0:
        s->imr = value & 0x33;
        break;
    case RFCR0:
        if (!s->rx_channel_enabled) {
            s->rfcr = MIN(value & 0xf, 15u);
        }
        break;
    case TFCR0:
        if (!s->tx_channel_enabled) {
            s->tfcr = MIN(value & 0xf, 15u);
        }
        break;
    case RFF0:
        if ((value & 1) && (!s->rx_channel_enabled || !s->rx_block_enabled)) {
            fifo_clear(&s->rx_fifo);
            s->rx_pio_left = true;
            s->rx_dma_left = true;
        }
        break;
    case TFF0:
        if ((value & 1) && (!s->tx_channel_enabled || !s->tx_block_enabled)) {
            fifo_clear(&s->tx_fifo);
            s->tx_left_valid = false;
            s->tx_pio_drop_right = false;
            s->tx_dma_left_valid = false;
            s->tx_dma_drop_right = false;
        }
        break;
    case RRXDMA:
        if ((value & 1) && s->rx_dma_left) {
            s->rx_dma_left = true;
        }
        break;
    case TXDMA:
        if (s->transmitter_enabled) {
            write_tx_dma(s, value);
        }
        return;
    case RTXDMA:
        break;
    case DMACR:
        value &= DMACR_RXBLOCK | DMACR_TXBLOCK;
        if ((value ^ s->dmacr) & DMACR_TXBLOCK) {
            s->dma_tx_force_idle = true;
        }
        if ((value ^ s->dmacr) & DMACR_RXBLOCK) {
            s->dma_rx_force_idle = true;
        }
        s->dmacr = value;
        break;
    default:
        return;
    }
    state_changed(s);
}

static uint32_t read_register(DWAPBI2SState *s, uint32_t offset)
{
    switch (offset) {
    case IER:
        return s->global_enabled;
    case IRER:
        return s->rx_block_enabled;
    case ITER:
        return s->tx_block_enabled;
    case CER:
        return s->clock_enabled;
    case CCR:
        return s->ccr;
    case LRBR_LTHR0:
        return read_rx_sample(s, false);
    case RRBR_RTHR0:
        return read_rx_sample(s, false);
    case RER0:
        return s->rx_channel_enabled;
    case TER0:
        return s->tx_channel_enabled;
    case RCR0:
        return s->rcr;
    case TCR0:
        return s->tcr;
    case ISR0:
        return interrupt_status(s);
    case IMR0:
        return s->imr;
    case ROR0: {
        const uint32_t value = s->rx_overrun;
        s->rx_overrun = false;
        state_changed(s);
        return value;
    }
    case TOR0: {
        const uint32_t value = s->tx_overrun;
        s->tx_overrun = false;
        state_changed(s);
        return value;
    }
    case RFCR0:
        return s->rfcr;
    case TFCR0:
        return s->tfcr;
    case RXDMA:
        return read_rx_sample(s, true);
    case COMP_PARAM_2:
        return 4;
    case COMP_PARAM_1:
        return component_parameter_1(s);
    case COMP_VERSION:
        return COMPONENT_VERSION;
    case COMP_TYPE:
        return COMPONENT_TYPE;
    case DMACR:
        return s->dmacr;
    default:
        return 0;
    }
}

static void drive_outputs(void *opaque)
{
    DWAPBI2SState *s = opaque;
    uint32_t enabled = 0;
    bool force_tx = s->dma_tx_force_idle;
    bool force_rx = s->dma_rx_force_idle;

    s->dma_tx_force_idle = s->dma_rx_force_idle = false;
    if (s->global_enabled && s->rx_block_enabled && s->rx_channel_enabled) {
        enabled |= ISR_RXDA | ISR_RXFO;
    }
    if (s->global_enabled && s->tx_block_enabled && s->tx_channel_enabled) {
        enabled |= ISR_TXFE | ISR_TXFO;
    }
    qemu_set_irq(s->irq, !!(interrupt_status(s) & enabled & ~s->imr));
    request_update(&s->dma_tx, tx_active(s) &&
                   (s->dmacr & DMACR_TXBLOCK) && s->tx_fifo.count < FIFO_DEPTH,
                   force_tx);
    request_update(&s->dma_rx, rx_active(s) &&
                   (s->dmacr & DMACR_RXBLOCK) && s->rx_fifo.count,
                   force_rx);
}

/*
 * Functional pacing retries a full receiver FIFO without losing a frame.
 * This models the QBox validation profile, not physical I2S wire timing.
 * In unpaced mode an empty transmitter emits silence and RX overflow drops.
 */
static void frame_tick(void *opaque)
{
    DWAPBI2SState *s = opaque;
    DWAPBI2SState *p = s->peer;
    I2SFrame frame = { 0, 0 };
    bool accepted = true;

    if (!tx_active(s)) {
        return;
    }
    if (s->tx_fifo.count) {
        frame = *fifo_front(&s->tx_fifo);
    }
    if (p && rx_active(p)) {
        if (p->rx_fifo.count == FIFO_DEPTH) {
            if (p->functional_pacing) {
                accepted = false;
            } else {
                p->rx_overrun = true;
            }
        } else {
            fifo_push(&p->rx_fifo, frame.left, frame.right);
        }
        state_changed(p);
    }
    if (accepted && s->tx_fifo.count) {
        fifo_pop(&s->tx_fifo);
    }
    state_changed(s);
}

static void tx_ack(void *opaque, int n, int level)
{
    DWAPBI2SState *s = opaque;
    s->dma_tx.ack = level;
    qemu_bh_schedule(s->bh);
}

static void rx_ack(void *opaque, int n, int level)
{
    DWAPBI2SState *s = opaque;
    s->dma_rx.ack = level;
    qemu_bh_schedule(s->bh);
}

static uint64_t i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    return read_register(opaque, offset);
}

static void i2s_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    write_register(opaque, offset, value);
}

static bool i2s_accepts(void *opaque, hwaddr offset, unsigned size,
                        bool is_write, MemTxAttrs attrs)
{
    return offset <= DMACR && !(offset & (size - 1)) &&
        (size == 4 || (size == 2 &&
                      ((is_write && offset == TXDMA) ||
                       (!is_write && offset == RXDMA))));
}

static const MemoryRegionOps i2s_ops = {
    .read = i2s_read,
    .write = i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4,
               .accepts = i2s_accepts },
    .impl = { .min_access_size = 2, .max_access_size = 4 },
};

static void i2s_reset(DeviceState *dev)
{
    DWAPBI2SState *s = DW_APB_I2S(dev);

    timer_del(s->timer);
    qemu_bh_cancel(s->bh);
    s->dma_tx.ack = s->dma_rx.ack = 0;
    s->dma_tx.asserted = s->dma_rx.asserted = false;
    s->dma_tx.wait_ack_low = s->dma_rx.wait_ack_low = false;
    qemu_set_irq(s->irq, 0);
    qemu_set_irq(s->dma_tx.irq, 0);
    qemu_set_irq(s->dma_rx.irq, 0);
    s->ccr = s->rfcr = s->tfcr = s->dmacr = 0;
    s->rcr = s->tcr = 5;
    s->imr = 0x33;
    s->tx_pio_left = s->tx_dma_left = 0;
    s->rx_block_enabled = s->tx_block_enabled = false;
    s->clock_enabled = false;
    s->rx_channel_enabled = s->tx_channel_enabled = false;
    write_register(s, IER, 0);
}

static void i2s_init(Object *obj)
{
    DWAPBI2SState *s = DW_APB_I2S(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    s->bh = aio_bh_new_guarded(qemu_get_aio_context(), drive_outputs, s,
                               &dev->mem_reentrancy_guard);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, frame_tick, s);
    memory_region_init_io(&s->iomem, obj, &i2s_ops, s, TYPE_DW_APB_I2S,
                          0x10000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(dev, &s->dma_tx.irq, "dma-tx-req", 1);
    qdev_init_gpio_out_named(dev, &s->dma_rx.irq, "dma-rx-req", 1);
    qdev_init_gpio_in_named(dev, tx_ack, "dma-tx-ack", 1);
    qdev_init_gpio_in_named(dev, rx_ack, "dma-rx-ack", 1);
}

static void i2s_finalize(Object *obj)
{
    DWAPBI2SState *s = DW_APB_I2S(obj);
    timer_free(s->timer);
    qemu_bh_delete(s->bh);
}

static const Property i2s_properties[] = {
    DEFINE_PROP_BOOL("master-mode", DWAPBI2SState, master_mode, false),
    DEFINE_PROP_BOOL("transmitter-enabled", DWAPBI2SState,
                     transmitter_enabled, true),
    DEFINE_PROP_BOOL("receiver-enabled", DWAPBI2SState,
                     receiver_enabled, true),
    DEFINE_PROP_BOOL("functional-pacing", DWAPBI2SState,
                     functional_pacing, true),
    DEFINE_PROP_UINT64("frame-period-ns", DWAPBI2SState,
                       frame_period_ns, 20833),
    DEFINE_PROP_LINK("peer", DWAPBI2SState, peer, TYPE_DW_APB_I2S,
                     DWAPBI2SState *),
};

static const VMStateDescription i2s_vmstate = {
    .name = TYPE_DW_APB_I2S,
    .unmigratable = 1,
};

static void i2s_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, i2s_reset);
    device_class_set_props(dc, i2s_properties);
    dc->vmsd = &i2s_vmstate;
    dc->desc = "DesignWare APB I2S stereo functional model";
}

static const TypeInfo i2s_info = {
    .name = TYPE_DW_APB_I2S,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWAPBI2SState),
    .instance_init = i2s_init,
    .instance_finalize = i2s_finalize,
    .class_init = i2s_class_init,
};

static void i2s_register_types(void)
{
    type_register_static(&i2s_info);
}
type_init(i2s_register_types)
