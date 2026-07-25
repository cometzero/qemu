/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIBQEMU_WRAPPERS_CLOCK_H
#define LIBQEMU_WRAPPERS_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Clock Clock;
typedef struct DeviceState DeviceState;
typedef struct Object Object;

Clock *libqemu_clock_new(Object *parent, const char *name);
bool libqemu_clock_update_hz(Clock *clock, uint64_t hz);
void libqemu_qdev_connect_clock_in(DeviceState *dev, const char *name,
                                   Clock *source);
void libqemu_device_cold_reset(DeviceState *dev);

#endif
