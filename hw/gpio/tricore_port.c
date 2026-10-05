/*
 * AURIX TC3x PORT functional GPIO subset.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout: Infineon TC39x-B IfxPort_reg{,def}.h and IfxPort.h,
 * iLLD ac8fb805633894b89819b953516b4e94387056fd.
 * The optional chardev transports physical pin levels, not guest UART data.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/gpio/tricore_port.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"

OBJECT_DECLARE_SIMPLE_TYPE(TriCorePortState, TRICORE_PORT)

struct TriCorePortState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq pins[16];
    uint32_t iocr[4];
    uint16_t out;
    uint16_t external_levels;
    uint16_t external_mask;
    uint16_t drive_mask;
    uint16_t drive_levels;
    uint8_t rx[8];
    unsigned rx_len;
    uint8_t tx[8];
    unsigned tx_pos;
    bool tx_pending;
    bool tx_dirty;
    bool connected;
    guint watch;
    QEMUTimer *snapshot_timer;
};

static unsigned port_mode(TriCorePortState *s, unsigned pin)
{
    return (s->iocr[pin / 4] >> ((pin % 4) * 8)) & 0xf8;
}

static void port_cancel_watch(TriCorePortState *s)
{
    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
}

static gboolean port_transmit(void *unused, GIOCondition cond, void *opaque)
{
    TriCorePortState *s = opaque;

    s->watch = 0;
    while (s->connected && (s->tx_pending || s->tx_dirty)) {
        int written;

        if (!s->tx_pending) {
            memcpy(s->tx, "GP\1O", 4);
            stw_le_p(s->tx + 4, s->drive_levels);
            stw_le_p(s->tx + 6, s->drive_mask);
            s->tx_pos = 0;
            s->tx_pending = true;
            s->tx_dirty = false;
        }
        written = qemu_chr_fe_write(&s->chr, s->tx + s->tx_pos,
                                    sizeof(s->tx) - s->tx_pos);
        if (written <= 0) {
            if (!(cond & G_IO_HUP)) {
                s->watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                                 port_transmit, s);
            }
            return G_SOURCE_REMOVE;
        }
        s->tx_pos += written;
        if (s->tx_pos == sizeof(s->tx)) {
            s->tx_pending = false;
        }
    }
    return G_SOURCE_REMOVE;
}

static void port_update(TriCorePortState *s, bool force)
{
    uint16_t mask = 0, levels;

    for (unsigned pin = 0; pin < 16; pin++) {
        unsigned mode = port_mode(s, pin);

        if (mode == 0x80 || (mode == 0xc0 && !(s->out & BIT(pin)))) {
            mask |= BIT(pin);
        }
    }
    levels = s->out & mask;
    if (force || mask != s->drive_mask || levels != s->drive_levels) {
        s->drive_mask = mask;
        s->drive_levels = levels;
        for (unsigned pin = 0; pin < 16; pin++) {
            qemu_set_irq(s->pins[pin], mask & BIT(pin) ?
                         !!(levels & BIT(pin)) : -1);
        }
        s->tx_dirty = true;
        if (s->connected && !s->watch) {
            port_transmit(NULL, G_IO_OUT, s);
        }
    }
}

static uint16_t port_input(TriCorePortState *s)
{
    uint16_t levels = s->external_levels & s->external_mask;

    for (unsigned pin = 0; pin < 16; pin++) {
        if (!(s->external_mask & BIT(pin)) && port_mode(s, pin) == 0x10) {
            levels |= BIT(pin);
        }
    }
    return (levels & ~s->drive_mask) | s->drive_levels;
}

static void port_snapshot(void *opaque)
{
    TriCorePortState *s = opaque;

    /* Transport liveness only: no guest heartbeat or simulated GPIO edge. */
    port_update(s, true);
    timer_mod(s->snapshot_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
}

static void port_pin_input(void *opaque, int pin, int level)
{
    TriCorePortState *s = opaque;

    s->external_mask = deposit32(s->external_mask, pin, 1, level >= 0);
    s->external_levels = deposit32(s->external_levels, pin, 1, level > 0);
}

static int port_can_receive(void *opaque)
{
    return 256;
}

static void port_receive(void *opaque, const uint8_t *buf, int size)
{
    TriCorePortState *s = opaque;

    for (int i = 0; i < size; i++) {
        s->rx[s->rx_len++] = buf[i];
        while (s->rx_len &&
               memcmp(s->rx, "GP\1I", MIN(s->rx_len, 4))) {
            memmove(s->rx, s->rx + 1, --s->rx_len);
        }
        if (s->rx_len == sizeof(s->rx)) {
            s->external_levels = lduw_le_p(s->rx + 4);
            s->external_mask = lduw_le_p(s->rx + 6);
            s->rx_len = 0;
        }
    }
}

static void port_event(void *opaque, QEMUChrEvent event)
{
    TriCorePortState *s = opaque;

    if (event != CHR_EVENT_OPENED && event != CHR_EVENT_CLOSED) {
        return;
    }
    port_cancel_watch(s);
    s->rx_len = 0;
    s->tx_pending = false;
    s->connected = event == CHR_EVENT_OPENED;
    if (!s->connected) {
        s->external_mask = 0;
        s->external_levels = 0;
    }
    port_update(s, true);
}

static uint64_t port_read(void *opaque, hwaddr offset, unsigned size)
{
    TriCorePortState *s = opaque;

    switch (offset) {
    case 0x00:
        return s->out;
    case 0x04:
        return 0; /* Write-only OMR. */
    case 0x10 ... 0x1c:
        return s->iocr[(offset - 0x10) / 4];
    case 0x24:
        return port_input(s);
    default:
        qemu_log_mask(LOG_UNIMP, "tricore-port: read offset 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void port_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    TriCorePortState *s = opaque;

    switch (offset) {
    case 0x00:
        s->out = value;
        break;
    case 0x04: {
        uint16_t set = value, clear = value >> 16;

        /* Setting PS and PCL together toggles the corresponding latch. */
        s->out = (s->out & ~(set | clear)) | (set & ~clear) |
                 (~s->out & set & clear);
        break;
    }
    case 0x10 ... 0x1c:
        s->iocr[(offset - 0x10) / 4] = value & 0xf8f8f8f8;
        break;
    case 0x24:
        return; /* IN is read-only. */
    default:
        qemu_log_mask(LOG_UNIMP, "tricore-port: write offset 0x%"
                      HWADDR_PRIx "\n", offset);
        return;
    }
    port_update(s, false);
}

static const MemoryRegionOps port_ops = {
    .read = port_read,
    .write = port_write,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void port_reset(DeviceState *dev)
{
    TriCorePortState *s = TRICORE_PORT(dev);

    s->out = 0;
    memset(s->iocr, 0, sizeof(s->iocr));
    s->rx_len = 0;
    /* External pin levels survive a CPU/PORT reset while connected. */
    port_update(s, true);
}

static void port_realize(DeviceState *dev, Error **errp)
{
    TriCorePortState *s = TRICORE_PORT(dev);

    qemu_chr_fe_set_handlers(&s->chr, port_can_receive, port_receive,
                             port_event, NULL, s, NULL, true);
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        timer_mod(s->snapshot_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
    }
}

static void port_init(Object *obj)
{
    TriCorePortState *s = TRICORE_PORT(obj);

    memory_region_init_io(&s->iomem, obj, &port_ops, s, "tricore-port", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in_named(DEVICE(obj), port_pin_input, "pin-in", 16);
    qdev_init_gpio_out_named(DEVICE(obj), s->pins, "pin-out", 16);
    s->snapshot_timer = timer_new_ms(QEMU_CLOCK_REALTIME, port_snapshot, s);
}

static void port_finalize(Object *obj)
{
    port_cancel_watch(TRICORE_PORT(obj));
    timer_free(TRICORE_PORT(obj)->snapshot_timer);
}

static const Property port_properties[] = {
    DEFINE_PROP_CHR("chardev", TriCorePortState, chr),
};

static const VMStateDescription port_vmstate = {
    .name = "tricore-port",
    .unmigratable = true,
};

static void port_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = port_realize;
    dc->vmsd = &port_vmstate;
    device_class_set_legacy_reset(dc, port_reset);
    device_class_set_props(dc, port_properties);
}

static const TypeInfo port_info = {
    .name = TYPE_TRICORE_PORT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCorePortState),
    .instance_init = port_init,
    .instance_finalize = port_finalize,
    .class_init = port_class_init,
};

static void port_register_types(void)
{
    type_register_static(&port_info);
}

type_init(port_register_types)
