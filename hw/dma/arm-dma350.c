/*
 * Arm DMA-350 functional model.
 * Based on the Apollo QBox SystemC dma350 programming model (BSD-3-Clause).
 * Implements 1D copy/fill/wrap and external command/DMA/peripheral flow
 * handshakes. Command links, security attribution and bus timing are absent.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/dma/arm-dma350.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"

#define CHANNELS 8
#define TRIGGERS 256
#define BASE(c) (0x1000 + (c) * 0x100)
#define REG(s, c, r) ((s)->regs[(BASE(c) + (r)) / 4])
#define ACTIVE 4

typedef struct DMA350Trigger {
    bool used, command, peripheral, ack;
    unsigned select, block;
} DMA350Trigger;

typedef struct DMA350Channel {
    bool enabled, paused, started, finish, disable, clear;
    unsigned width, xtype;
    uint64_t src, dst, src_start;
    uint32_t src_left, dst_left, src_length, wrap_left;
    int16_t src_inc, dst_inc;
    DMA350Trigger trigger[2];
} DMA350Channel;

struct ARMDMA350State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq irq[CHANNELS + 1];
    qemu_irq ack[TRIGGERS];
    uint32_t request[TRIGGERS];
    int owner[TRIGGERS];
    uint32_t regs[0x2000 / 4];
    uint32_t channels, triggers;
    DMA350Channel channel[CHANNELS];
};

static void dma350_schedule(ARMDMA350State *s)
{
    if (!timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
    }
}

static void dma350_irq(ARMDMA350State *s)
{
    uint32_t pending = 0;
    unsigned c;
    for (c = 0; c < s->channels; c++) {
        bool level = (REG(s, c, 4) & 0x7ff) != 0;
        qemu_set_irq(s->irq[c], level);
        pending |= level << c;
    }
    s->regs[0x200 / 4] = pending;
    s->regs[0x208 / 4] = !!(pending && (s->regs[0x20c / 4] & 1));
    qemu_set_irq(s->irq[CHANNELS], s->regs[0x208 / 4]);
}

static void dma350_event(ARMDMA350State *s, unsigned c, unsigned bit)
{
    REG(s, c, 4) |= 1u << (16 + bit);
    REG(s, c, 4) |= REG(s, c, 8) & (1u << bit);
    dma350_irq(s);
}

static void dma350_ack(ARMDMA350State *s, DMA350Trigger *t,
                       bool active, unsigned type)
{
    t->ack = active;
    qemu_set_irq(s->ack[t->select], (active ? ACTIVE : 0) | type);
}

static void dma350_release(ARMDMA350State *s, unsigned c)
{
    unsigned i;
    for (i = 0; i < 2; i++) {
        DMA350Trigger *t = &s->channel[c].trigger[i];
        if (t->used && t->select < s->triggers) {
            if (t->ack) {
                dma350_ack(s, t, false, 0);
            }
            if (s->owner[t->select] == c) {
                s->owner[t->select] = -1;
            }
        }
    }
}

static void dma350_reset_channel(ARMDMA350State *s, unsigned c)
{
    dma350_release(s, c);
    memset(&s->channel[c], 0, sizeof(s->channel[c]));
    memset(&REG(s, c, 0), 0, 0x100);
    REG(s, c, 0x0c) = 0x00200200;
    REG(s, c, 0x2c) = 0x000f0400;
    REG(s, c, 0xf0) = 0x3a00043b;
    REG(s, c, 0xf8) = (15u << 26) | (4u << 22) | (47u << 16) | 15;
    REG(s, c, 0xfc) = BIT(7) | BIT(5) | BIT(4) | BIT(1) | BIT(0);
}

static void dma350_finish(ARMDMA350State *s, unsigned c, unsigned bit)
{
    bool clear = s->channel[c].clear;
    dma350_release(s, c);
    s->channel[c].enabled = false;
    s->channel[c].paused = false;
    REG(s, c, 0) = 0;
    dma350_event(s, c, bit);
    if (clear) {
        dma350_reset_channel(s, c);
        dma350_irq(s);
    }
}

static void dma350_error(ARMDMA350State *s, unsigned c, uint32_t error)
{
    REG(s, c, 0x90) = error;
    dma350_finish(s, c, 1);
}

static uint64_t dma350_addr(ARMDMA350State *s, unsigned c, unsigned off)
{
    return REG(s, c, off) | ((uint64_t)REG(s, c, off + 4) << 32);
}

static void dma350_progress(ARMDMA350State *s, unsigned c)
{
    DMA350Channel *ch = &s->channel[c];
    REG(s, c, 0x10) = ch->src;
    REG(s, c, 0x14) = ch->src >> 32;
    REG(s, c, 0x18) = ch->dst;
    REG(s, c, 0x1c) = ch->dst >> 32;
    REG(s, c, 0x20) = (ch->src_left & 0xffff) | (ch->dst_left << 16);
    REG(s, c, 0x24) = (ch->src_left >> 16) | (ch->dst_left & 0xffff0000);
}

static void dma350_start(ARMDMA350State *s, unsigned c)
{
    DMA350Channel *ch = &s->channel[c];
    uint32_t ctrl = REG(s, c, 0x0c);
    unsigned i;
    dma350_release(s, c);
    memset(ch, 0, sizeof(*ch));
    ch->width = 1u << (ctrl & 7);
    ch->xtype = (ctrl >> 9) & 7;
    if (ch->width > 16 || ch->xtype < 1 || ch->xtype > 3) {
        dma350_error(s, c, BIT(1) | BIT(25));
        return;
    }
    ch->src = dma350_addr(s, c, 0x10) & ~(uint64_t)(ch->width - 1);
    ch->dst = dma350_addr(s, c, 0x18) & ~(uint64_t)(ch->width - 1);
    ch->src_start = ch->src;
    ch->src_left = (REG(s, c, 0x20) & 0xffff) |
                   (REG(s, c, 0x24) << 16);
    ch->dst_left = (REG(s, c, 0x20) >> 16) |
                   (REG(s, c, 0x24) & 0xffff0000);
    if (ch->xtype == 3) {
        ch->src_left = 0;
    }
    ch->src_length = ch->src_left;
    ch->wrap_left = MAX(ch->src_left, ch->dst_left);
    ch->src_inc = REG(s, c, 0x30);
    ch->dst_inc = REG(s, c, 0x30) >> 16;
    ch->started = true;
    for (i = 0; i < 2; i++) {
        DMA350Trigger *t = &ch->trigger[i];
        uint32_t cfg = REG(s, c, 0x4c + 4 * i);
        unsigned mode = (cfg >> 10) & 3;
        if (!(ctrl & BIT(25 + i))) {
            continue;
        }
        t->used = true;
        t->select = cfg & 255;
        t->block = ((cfg >> 16) & 255) + 1;
        t->command = mode == 0;
        t->peripheral = mode == 3;
        if (((cfg >> 8) & 3) != 2 || mode == 1 ||
            t->select >= s->triggers || s->owner[t->select] != -1 ||
            (i == 0 && ch->xtype == 3)) {
            dma350_error(s, c, BIT(1) | BIT(25) | BIT(2 + i));
            return;
        }
        s->owner[t->select] = c;
        ch->started &= !t->command;
    }
    if (ch->xtype == 2 && !ch->src_left && ch->dst_left) {
        dma350_error(s, c, BIT(1) | BIT(25));
        return;
    }
    ch->enabled = true;
    REG(s, c, 0) = 1;
    REG(s, c, 4) = 0;
    REG(s, c, 0x90) = 0;
    dma350_progress(s, c);
    dma350_irq(s);
    dma350_schedule(s);
}

static bool dma350_service(ARMDMA350State *s, unsigned c)
{
    DMA350Channel *ch = &s->channel[c];
    unsigned i, units = 256 / ch->width;
    bool changed = false, last = false, peripheral = false, complete;
    uint8_t data[16];
    for (i = 0; i < 2; i++) {
        DMA350Trigger *t = &ch->trigger[i];
        if (t->used && t->ack && !(s->request[t->select] & ACTIVE)) {
            dma350_ack(s, t, false, 0);
            changed = true;
        }
    }
    if (ch->finish) {
        if (ch->trigger[0].ack || ch->trigger[1].ack) {
            return changed;
        }
        dma350_finish(s, c, ch->disable ? 2 : 0);
        return true;
    }
    if (!ch->started) {
        for (i = 0; i < 2; i++) {
            DMA350Trigger *t = &ch->trigger[i];
            if (t->used && t->command &&
                !(s->request[t->select] & ACTIVE)) {
                dma350_event(s, c, 8 + i);
                return changed;
            }
        }
        ch->started = true;
        for (i = 0; i < 2; i++) {
            DMA350Trigger *t = &ch->trigger[i];
            if (t->used && t->command) {
                dma350_ack(s, t, true, 0);
            }
        }
    }
    for (i = 0; i < 2; i++) {
        DMA350Trigger *t = &ch->trigger[i];
        unsigned req;
        if (!t->used || t->command) {
            continue;
        }
        req = s->request[t->select];
        if (t->ack || !(req & ACTIVE)) {
            if (!t->ack) {
                dma350_event(s, c, 8 + i);
            }
            return changed;
        }
        units = MIN(units, (req & 2) ? t->block : 1);
        last |= t->peripheral && (req & 1);
        peripheral |= t->peripheral;
    }
    if (ch->xtype == 3) {
        units = MIN(units, ch->dst_left);
    } else if (ch->xtype == 2) {
        units = MIN(units, MIN(ch->wrap_left, ch->src_left));
    } else {
        units = MIN(units, MIN(ch->src_left, ch->dst_left));
    }
    REG(s, c, 4) &= ~(BIT(24) | BIT(25) | BIT(8) | BIT(9));
    for (i = 0; i < units; i++) {
        unsigned b;
        if (ch->xtype == 3) {
            for (b = 0; b < ch->width; b++) {
                data[b] = REG(s, c, 0x38) >> ((b & 3) * 8);
            }
        } else {
            if (address_space_read(&address_space_memory, ch->src,
                                   MEMTXATTRS_UNSPECIFIED, data,
                                   ch->width) != MEMTX_OK) {
                dma350_error(s, c, BIT(0) | BIT(16));
                return true;
            }
            ch->src += (int64_t)ch->src_inc * ch->width;
            ch->src_left--;
        }
        if (ch->dst_left) {
            if (address_space_write(&address_space_memory, ch->dst,
                                    MEMTXATTRS_UNSPECIFIED, data,
                                    ch->width) != MEMTX_OK) {
                dma350_error(s, c, BIT(0) | BIT(17));
                return true;
            }
            ch->dst += (int64_t)ch->dst_inc * ch->width;
            ch->dst_left--;
        }
        if (ch->xtype == 2) {
            ch->wrap_left--;
            if (!ch->src_left && ch->wrap_left) {
                ch->src = ch->src_start;
                ch->src_left = ch->src_length;
            }
        }
    }
    if (last) {
        if (ch->trigger[0].peripheral) {
            ch->src_left = 0;
        }
        if (ch->trigger[1].peripheral) {
            ch->dst_left = 0;
        }
    }
    dma350_progress(s, c);
    complete = ch->xtype == 2 ? !ch->wrap_left :
               (!ch->dst_left || (ch->xtype != 3 && !ch->src_left));
    if (complete && peripheral && !last) {
        dma350_error(s, c, BIT(1) | BIT(26));
        return true;
    }
    complete |= last;
    for (i = 0; i < 2; i++) {
        DMA350Trigger *t = &ch->trigger[i];
        if (t->used && !t->command && units) {
            dma350_ack(s, t, true, complete ? 2 : 0);
        }
    }
    if (complete) {
        ch->finish = true;
    }
    dma350_irq(s);
    return true;
}

static void dma350_tick(void *opaque)
{
    ARMDMA350State *s = opaque;
    unsigned c;
    bool progress = false;
    MemReentrancyGuard *guard = &DEVICE(s)->mem_reentrancy_guard;

    /* Timer DMA must not reenter its own registers through guest addresses. */
    if (guard->engaged_in_io) {
        dma350_schedule(s);
        return;
    }
    guard->engaged_in_io = true;
    for (c = 0; c < s->channels; c++) {
        if (s->channel[c].enabled && !s->channel[c].paused) {
            progress |= dma350_service(s, c);
        }
    }
    guard->engaged_in_io = false;
    if (progress) {
        dma350_schedule(s);
    }
}

static void dma350_req(void *opaque, int n, int level)
{
    ARMDMA350State *s = opaque;
    s->request[n] = level;
    dma350_schedule(s);
}

static uint64_t dma350_read(void *opaque, hwaddr off, unsigned size)
{
    ARMDMA350State *s = opaque;
    return s->regs[off / 4];
}

static void dma350_write(void *opaque, hwaddr off, uint64_t value,
                         unsigned size)
{
    ARMDMA350State *s = opaque;
    uint32_t v = value;
    if (off >= 0x1000) {
        unsigned c = (off - 0x1000) / 0x100, r = off & 255;
        DMA350Channel *ch;
        if (c >= s->channels) {
            return;
        }
        ch = &s->channel[c];
        if (r == 0) {
            if (v & 1) {
                if (!ch->enabled) {
                    dma350_start(s, c);
                }
                return;
            }
            if (v & 2) {
                if (!ch->enabled) {
                    dma350_reset_channel(s, c);
                } else {
                    ch->clear = true;
                }
            }
            if (ch->enabled) {
                if (v & 8) {
                    dma350_finish(s, c, 3);
                } else {
                    ch->disable |= !!(v & 4);
                    if (v & 16) {
                        ch->paused = true;
                        REG(s, c, 0) |= 16;
                        REG(s, c, 4) |= BIT(20) | BIT(21);
                    }
                    if (v & 32) {
                        ch->paused = false;
                        REG(s, c, 0) &= ~16;
                        REG(s, c, 4) &= ~(BIT(20) | BIT(21));
                    }
                    dma350_schedule(s);
                }
            }
        } else if (r == 4) {
            uint32_t clear = v & 0x7ff;
            unsigned b;
            for (b = 0; b < 4; b++) {
                if (v & BIT(16 + b)) {
                    clear |= BIT(b) | BIT(16 + b);
                }
            }
            REG(s, c, 4) &= ~clear;
            if (v & BIT(17)) {
                REG(s, c, 0x90) = 0;
            }
        } else if (r == 8) {
            /* Interrupt masks remain writable while a transfer is active. */
            REG(s, c, 8) = v;
        } else if (!ch->enabled && r != 0x90 && r < 0xf0) {
            s->regs[off / 4] = v;
        }
    } else if (off == 0x20c) {
        s->regs[off / 4] = v & 1;
    } else if (off != 0x100 && off != 0x200 && off != 0x208 &&
               !(off >= 0xfb0 && off <= 0xfcc)) {
        s->regs[off / 4] = v;
    }
    dma350_irq(s);
}

static const MemoryRegionOps dma350_ops = {
    .read = dma350_read,
    .write = dma350_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void dma350_reset(DeviceState *dev)
{
    ARMDMA350State *s = ARM_DMA350(dev);
    unsigned c;
    timer_del(s->timer);
    for (c = 0; c < s->channels; c++) {
        dma350_release(s, c);
    }
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->owner, -1, sizeof(s->owner));
    for (c = 0; c < s->channels; c++) {
        dma350_reset_channel(s, c);
    }
    s->regs[0xfb0 / 4] = (4 << 16) | (47 << 10) | ((s->channels - 1) << 4);
    s->regs[0xfb4 / 4] = BIT(16) | s->triggers;
    s->regs[0xfc8 / 4] = 0x3a00043b;
    dma350_irq(s);
}

static void dma350_realize(DeviceState *dev, Error **errp)
{
    ARMDMA350State *s = ARM_DMA350(dev);
    if (!s->channels || s->channels > CHANNELS || s->triggers > TRIGGERS) {
        error_setg(errp, "DMA350 needs 1..8 channels and at most 256 triggers");
    }
}

static void dma350_init(Object *obj)
{
    ARMDMA350State *s = ARM_DMA350(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned i;
    memory_region_init_io(&s->iomem, obj, &dma350_ops, s,
                          TYPE_ARM_DMA350, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    for (i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), dma350_req, "dma-req", TRIGGERS);
    qdev_init_gpio_out_named(DEVICE(obj), s->ack, "dma-ack", TRIGGERS);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dma350_tick, s);
}

static void dma350_finalize(Object *obj)
{
    timer_free(ARM_DMA350(obj)->timer);
}

static const VMStateDescription dma350_vmstate = {
    .name = TYPE_ARM_DMA350,
    .unmigratable = 1,
};

static const Property dma350_properties[] = {
    DEFINE_PROP_UINT32("channel-count", ARMDMA350State, channels, 8),
    DEFINE_PROP_UINT32("trigger-count", ARMDMA350State, triggers, 8),
};

static void dma350_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = dma350_realize;
    device_class_set_legacy_reset(dc, dma350_reset);
    dc->vmsd = &dma350_vmstate;
    device_class_set_props(dc, dma350_properties);
}

static const TypeInfo dma350_info = {
    .name = TYPE_ARM_DMA350,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ARMDMA350State),
    .instance_init = dma350_init,
    .instance_finalize = dma350_finalize,
    .class_init = dma350_class_init,
};

static void dma350_register_types(void)
{
    type_register_static(&dma350_info);
}
type_init(dma350_register_types)
