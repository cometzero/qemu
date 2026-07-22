/*
 * libqemu
 *
 * Copyright (c) 2019 Luc Michel <luc.michel@greensocs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _LIBQEMU_WRAPPERS_TARGET_ARM_H
#define _LIBQEMU_WRAPPERS_TARGET_ARM_H

#include <stdbool.h>
#include <stdint.h>
#include "libqemu/wrappers/arm-generic-timer.h"

void libqemu_cpu_arm_set_cp15_cbar(Object *cpu, uint64_t cbar);
void libqemu_cpu_arm_set_imp_buildoptr(Object *obj, uint32_t imp_buildoptr_val);
void libqemu_cpu_aarch64_set_aarch64_mode(Object *cpu, bool aarch64_mode);

int libqemu_arm_set_cpu_on_and_reset(Object *cpu);
int libqemu_arm_set_cpu_off(Object *cpu);

void libqemu_cpu_arm_add_nvic_link(Object *cpu);
void libqemu_arm_nvic_add_cpu_link(Object *cpu);

uint64_t libqemu_cpu_arm_get_exclusive_addr(const Object *cpu);
uint64_t libqemu_cpu_arm_get_exclusive_val(const Object *cpu);
void libqemu_cpu_arm_set_exclusive_val(Object *cpu, uint64_t val);
void libqemu_cpu_arm_set_power_state(Object *cpu, bool powered_on);
int libqemu_cpu_arm_get_power_state(Object *cpu);
int libqemu_cpu_arm_power_on_and_reset(Object *cpu);

void libqemu_cpu_arm_post_init(Object *obj);
void libqemu_cpu_arm_register_reset(Object *cpu);
uint64_t libqemu_cpu_arm_v7m_get_state(Object *cpu, int field);
bool libqemu_cpu_arm_v7m_set_state(Object *cpu, int field, uint64_t value);
uint64_t libqemu_cpu_arm_aarch64_get_state(Object *cpu, int field);
Object *libqemu_arm_generic_timer_counter_proxy_new(
    const LibQemuArmGenericTimerCounterCallbacks *callbacks, void *opaque);
void libqemu_arm_generic_timer_counter_proxy_clear(Object *obj);
void libqemu_arm_generic_timer_counter_notify(Object *obj);
void libqemu_cpu_arm_connect_generic_timer_output(
    Object *cpu, LibQemuArmGenericTimerOutput output, struct IRQState *sink);
bool libqemu_cpu_arm_generic_timer_snapshot(
    Object *cpu, LibQemuArmGenericTimerOutput output,
    LibQemuArmCpuGenericTimerSnapshot *snapshot);
bool libqemu_arm_arch_timer_mmio_frame_snapshot(
    Object *timer, uint32_t frame,
    LibQemuArmArchTimerMMIOFrameSnapshot *snapshot);
bool libqemu_arm_sse_timer_snapshot(
    Object *counter, Object *timer, LibQemuArmSSETimerSnapshot *snapshot);

#endif
