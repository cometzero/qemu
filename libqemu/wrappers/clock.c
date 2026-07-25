/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev.h"
#include "clock.h"

Clock *libqemu_clock_new(Object *parent, const char *name)
{
    return clock_new(parent, name);
}

bool libqemu_clock_update_hz(Clock *clock, uint64_t hz)
{
    if (hz > UINT_MAX) {
        return false;
    }
    clock_update_hz(clock, hz);
    return true;
}

void libqemu_qdev_connect_clock_in(DeviceState *dev, const char *name,
                                   Clock *source)
{
    qdev_connect_clock_in(dev, name, source);
}

void libqemu_device_cold_reset(DeviceState *dev)
{
    device_cold_reset(dev);
}
