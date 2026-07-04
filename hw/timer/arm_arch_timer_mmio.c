/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/arm_arch_timer_mmio.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define CNTCTL_SIZE 0x1000
#define DEFAULT_VIEW_SIZE 0x10000
#define DEFAULT_CNTFRQ 62500000U

static uint64_t arm_arch_timer_mmio_counter(ArmArchTimerMMIOState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->cntfrq,
                    NANOSECONDS_PER_SECOND);
}

static int64_t arm_arch_timer_mmio_counter_to_ns(ArmArchTimerMMIOState *s,
                                                 uint64_t count)
{
    return muldiv64(count, NANOSECONDS_PER_SECOND, s->cntfrq);
}

static bool arm_arch_timer_mmio_frame_pending(ArmArchTimerMMIOState *s,
                                              ArmArchTimerMMIOFrame *f)
{
    return (f->ctl & ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE) &&
           arm_arch_timer_mmio_counter(s) >= f->cval;
}

static void arm_arch_timer_mmio_update_frame(ArmArchTimerMMIOState *s,
                                             unsigned int idx)
{
    ArmArchTimerMMIOFrame *f = &s->frame[idx];
    bool pending = arm_arch_timer_mmio_frame_pending(s, f);
    bool irq_level = pending &&
                     !(f->ctl & ARM_ARCH_TIMER_MMIO_CNTP_CTL_IMASK);

    qemu_set_irq(f->irq, irq_level);
    timer_del(f->timer);

    if (!pending && (f->ctl & ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE)) {
        timer_mod(f->timer, arm_arch_timer_mmio_counter_to_ns(s, f->cval));
    }
}

static void arm_arch_timer_mmio_tick(void *opaque)
{
    ArmArchTimerMMIOFrame *f = opaque;
    ArmArchTimerMMIOState *s = f->parent;

    arm_arch_timer_mmio_update_frame(s, f->index);
}

static uint64_t arm_arch_timer_mmio_read_counter_half(ArmArchTimerMMIOState *s,
                                                      hwaddr offset)
{
    uint64_t count = arm_arch_timer_mmio_counter(s);

    return offset == ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_HI ?
           extract64(count, 32, 32) : extract64(count, 0, 32);
}

static uint64_t arm_arch_timer_mmio_read_frame(ArmArchTimerMMIOState *s,
                                               unsigned int idx,
                                               hwaddr offset)
{
    ArmArchTimerMMIOFrame *f = &s->frame[idx];
    uint32_t ctl = f->ctl & ~ARM_ARCH_TIMER_MMIO_CNTP_CTL_ISTAT;

    if (arm_arch_timer_mmio_frame_pending(s, f)) {
        ctl |= ARM_ARCH_TIMER_MMIO_CNTP_CTL_ISTAT;
    }

    switch (offset) {
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_LO:
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_HI:
        return arm_arch_timer_mmio_read_counter_half(s, offset);
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTFID:
        return s->frame_id[idx];
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTFRQ:
        return s->cntfrq;
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPL0ACR:
        return f->cntpl0acr;
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO:
        return extract64(f->cval, 0, 32);
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_HI:
        return extract64(f->cval, 32, 32);
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_TVAL:
        return (uint32_t)(f->cval - arm_arch_timer_mmio_counter(s));
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL:
        return ctl;
    default:
        return 0;
    }
}

static void arm_arch_timer_mmio_write_frame(ArmArchTimerMMIOState *s,
                                            unsigned int idx,
                                            hwaddr offset, uint64_t value)
{
    ArmArchTimerMMIOFrame *f = &s->frame[idx];

    switch (offset) {
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPL0ACR:
        f->cntpl0acr = value;
        break;
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO:
        f->cval = deposit64(f->cval, 0, 32, value);
        break;
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_HI:
        f->cval = deposit64(f->cval, 32, 32, value);
        break;
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_TVAL:
        f->cval = arm_arch_timer_mmio_counter(s) + (int32_t)value;
        break;
    case ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL:
        f->ctl = value & (ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE |
                          ARM_ARCH_TIMER_MMIO_CNTP_CTL_IMASK);
        break;
    default:
        return;
    }

    arm_arch_timer_mmio_update_frame(s, idx);
}

static int arm_arch_timer_mmio_frame_at(ArmArchTimerMMIOState *s,
                                        hwaddr offset, hwaddr *frame_offset)
{
    unsigned int i;

    for (i = 0; i < s->nr_frames; i++) {
        if (offset >= s->frame_offset[i] &&
            offset < s->frame_offset[i] + s->view_size) {
            *frame_offset = offset - s->frame_offset[i];
            return i;
        }
    }

    return -1;
}

static uint64_t arm_arch_timer_mmio_read(void *opaque, hwaddr offset,
                                         unsigned int size)
{
    ArmArchTimerMMIOState *s = opaque;
    hwaddr frame_off;
    int frame;

    if (size != 4) {
        return 0;
    }

    if (offset < CNTCTL_SIZE) {
        switch (offset) {
        case ARM_ARCH_TIMER_MMIO_CNTCTL_CNTFRQ:
            return s->cntfrq;
        case ARM_ARCH_TIMER_MMIO_CNTCTL_CNTSR:
            return 0;
        case ARM_ARCH_TIMER_MMIO_CNTCTL_CNTTID:
            return s->nr_frames;
        default:
            if (offset >= ARM_ARCH_TIMER_MMIO_CNTCTL_CNTACR_BASE &&
                offset < ARM_ARCH_TIMER_MMIO_CNTCTL_CNTACR_BASE +
                         s->nr_frames * sizeof(uint32_t)) {
                return s->cntacr[(offset -
                       ARM_ARCH_TIMER_MMIO_CNTCTL_CNTACR_BASE) / 4];
            }
            return 0;
        }
    }

    frame = arm_arch_timer_mmio_frame_at(s, offset, &frame_off);
    if (frame < 0) {
        return 0;
    }

    return arm_arch_timer_mmio_read_frame(s, frame, frame_off);
}

static void arm_arch_timer_mmio_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned int size)
{
    ArmArchTimerMMIOState *s = opaque;
    hwaddr frame_off;
    int frame;

    if (size != 4) {
        return;
    }

    if (offset < CNTCTL_SIZE) {
        if (offset >= ARM_ARCH_TIMER_MMIO_CNTCTL_CNTACR_BASE &&
            offset < ARM_ARCH_TIMER_MMIO_CNTCTL_CNTACR_BASE +
                     s->nr_frames * sizeof(uint32_t)) {
            s->cntacr[(offset - ARM_ARCH_TIMER_MMIO_CNTCTL_CNTACR_BASE) / 4] =
                value;
        }
        return;
    }

    frame = arm_arch_timer_mmio_frame_at(s, offset, &frame_off);
    if (frame >= 0) {
        arm_arch_timer_mmio_write_frame(s, frame, frame_off, value);
    }
}

static const MemoryRegionOps arm_arch_timer_mmio_ops = {
    .read = arm_arch_timer_mmio_read,
    .write = arm_arch_timer_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static uint64_t arm_arch_timer_mmio_region_size(ArmArchTimerMMIOState *s)
{
    uint64_t size = CNTCTL_SIZE;
    unsigned int i;

    for (i = 0; i < s->nr_frames; i++) {
        size = MAX(size, s->frame_offset[i] + s->view_size);
    }

    return size;
}

static bool arm_arch_timer_mmio_validate(ArmArchTimerMMIOState *s, Error **errp)
{
    unsigned int i, j;

    if (!s->cntfrq) {
        error_setg(errp, "CNTFRQ must be nonzero");
        return false;
    }

    if (!s->nr_frames || s->nr_frames > ARM_ARCH_TIMER_MMIO_MAX_FRAMES) {
        error_setg(errp, "nr-frames must be 1..%u",
                   ARM_ARCH_TIMER_MMIO_MAX_FRAMES);
        return false;
    }

    if (s->view_size < 0x1000) {
        error_setg(errp, "view-size must be at least 0x1000");
        return false;
    }

    for (i = 0; i < s->nr_frames; i++) {
        if (s->frame_offset[i] < CNTCTL_SIZE) {
            error_setg(errp, "frame-offset-%u overlaps CNTCTL", i);
            return false;
        }
        for (j = i + 1; j < s->nr_frames; j++) {
            if (s->frame_offset[i] < s->frame_offset[j] + s->view_size &&
                s->frame_offset[j] < s->frame_offset[i] + s->view_size) {
                error_setg(errp, "frame offsets %u and %u overlap", i, j);
                return false;
            }
        }
    }

    return true;
}

static void arm_arch_timer_mmio_realize(DeviceState *dev, Error **errp)
{
    ArmArchTimerMMIOState *s = ARM_ARCH_TIMER_MMIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    unsigned int i;

    if (!arm_arch_timer_mmio_validate(s, errp)) {
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &arm_arch_timer_mmio_ops, s,
                          "arm_arch_timer_mmio",
                          arm_arch_timer_mmio_region_size(s));
    sysbus_init_mmio(sbd, &s->iomem);

    for (i = 0; i < s->nr_frames; i++) {
        s->frame[i].parent = s;
        s->frame[i].index = i;
        s->frame[i].cval = UINT64_MAX;
        sysbus_init_irq(sbd, &s->frame[i].irq);
        s->frame[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         arm_arch_timer_mmio_tick,
                                         &s->frame[i]);
    }
}

static void arm_arch_timer_mmio_reset(DeviceState *dev)
{
    ArmArchTimerMMIOState *s = ARM_ARCH_TIMER_MMIO(dev);
    unsigned int i;

    for (i = 0; i < s->nr_frames; i++) {
        s->frame[i].cval = UINT64_MAX;
        s->frame[i].ctl = 0;
        s->frame[i].cntpl0acr = 0;
        s->cntacr[i] = 0;
        timer_del(s->frame[i].timer);
        qemu_set_irq(s->frame[i].irq, 0);
    }
}

static const VMStateDescription vmstate_arm_arch_timer_mmio_frame = {
    .name = "arm_arch_timer_mmio/frame",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(cval, ArmArchTimerMMIOFrame),
        VMSTATE_UINT32(ctl, ArmArchTimerMMIOFrame),
        VMSTATE_UINT32(cntpl0acr, ArmArchTimerMMIOFrame),
        VMSTATE_TIMER_PTR(timer, ArmArchTimerMMIOFrame),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_arm_arch_timer_mmio = {
    .name = "arm_arch_timer_mmio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(cntfrq, ArmArchTimerMMIOState),
        VMSTATE_UINT32(nr_frames, ArmArchTimerMMIOState),
        VMSTATE_UINT64(view_size, ArmArchTimerMMIOState),
        VMSTATE_UINT64_ARRAY(frame_offset, ArmArchTimerMMIOState,
                             ARM_ARCH_TIMER_MMIO_MAX_FRAMES),
        VMSTATE_UINT32_ARRAY(frame_id, ArmArchTimerMMIOState,
                             ARM_ARCH_TIMER_MMIO_MAX_FRAMES),
        VMSTATE_UINT32_ARRAY(cntacr, ArmArchTimerMMIOState,
                             ARM_ARCH_TIMER_MMIO_MAX_FRAMES),
        VMSTATE_STRUCT_ARRAY(frame, ArmArchTimerMMIOState,
                             ARM_ARCH_TIMER_MMIO_MAX_FRAMES, 1,
                             vmstate_arm_arch_timer_mmio_frame,
                             ArmArchTimerMMIOFrame),
        VMSTATE_END_OF_LIST()
    }
};

static const Property arm_arch_timer_mmio_properties[] = {
    DEFINE_PROP_UINT32("cntfrq", ArmArchTimerMMIOState, cntfrq,
                       DEFAULT_CNTFRQ),
    DEFINE_PROP_UINT32("nr-frames", ArmArchTimerMMIOState, nr_frames, 2),
    DEFINE_PROP_UINT64("view-size", ArmArchTimerMMIOState, view_size,
                       DEFAULT_VIEW_SIZE),
    DEFINE_PROP_UINT64("frame-offset-0", ArmArchTimerMMIOState,
                       frame_offset[0], 0x10000),
    DEFINE_PROP_UINT64("frame-offset-1", ArmArchTimerMMIOState,
                       frame_offset[1], 0x20000),
    DEFINE_PROP_UINT64("frame-offset-2", ArmArchTimerMMIOState,
                       frame_offset[2], 0x30000),
    DEFINE_PROP_UINT64("frame-offset-3", ArmArchTimerMMIOState,
                       frame_offset[3], 0x40000),
    DEFINE_PROP_UINT64("frame-offset-4", ArmArchTimerMMIOState,
                       frame_offset[4], 0x50000),
    DEFINE_PROP_UINT64("frame-offset-5", ArmArchTimerMMIOState,
                       frame_offset[5], 0x60000),
    DEFINE_PROP_UINT64("frame-offset-6", ArmArchTimerMMIOState,
                       frame_offset[6], 0x70000),
    DEFINE_PROP_UINT64("frame-offset-7", ArmArchTimerMMIOState,
                       frame_offset[7], 0x80000),
    DEFINE_PROP_UINT32("frame-id-0", ArmArchTimerMMIOState, frame_id[0], 0),
    DEFINE_PROP_UINT32("frame-id-1", ArmArchTimerMMIOState, frame_id[1], 1),
    DEFINE_PROP_UINT32("frame-id-2", ArmArchTimerMMIOState, frame_id[2], 2),
    DEFINE_PROP_UINT32("frame-id-3", ArmArchTimerMMIOState, frame_id[3], 3),
    DEFINE_PROP_UINT32("frame-id-4", ArmArchTimerMMIOState, frame_id[4], 4),
    DEFINE_PROP_UINT32("frame-id-5", ArmArchTimerMMIOState, frame_id[5], 5),
    DEFINE_PROP_UINT32("frame-id-6", ArmArchTimerMMIOState, frame_id[6], 6),
    DEFINE_PROP_UINT32("frame-id-7", ArmArchTimerMMIOState, frame_id[7], 7),
};

static void arm_arch_timer_mmio_class_init(ObjectClass *klass,
                                           const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = arm_arch_timer_mmio_realize;
    device_class_set_legacy_reset(dc, arm_arch_timer_mmio_reset);
    dc->vmsd = &vmstate_arm_arch_timer_mmio;
    device_class_set_props(dc, arm_arch_timer_mmio_properties);
}

static const TypeInfo arm_arch_timer_mmio_info = {
    .name = TYPE_ARM_ARCH_TIMER_MMIO,
    .parent = TYPE_DYNAMIC_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArmArchTimerMMIOState),
    .class_init = arm_arch_timer_mmio_class_init,
};

static void arm_arch_timer_mmio_register_types(void)
{
    type_register_static(&arm_arch_timer_mmio_info);
}

type_init(arm_arch_timer_mmio_register_types)
