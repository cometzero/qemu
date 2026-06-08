/*
 * libqemu
 *
 * Copyright (c) 2021 Luc Michel <luc.michel@greensocs.com>
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

#ifndef _LIBQEMU_WRAPPERS_LIBQEMU_H
#define _LIBQEMU_WRAPPERS_LIBQEMU_H

#include <stdbool.h>

typedef struct QemuObject QemuObject;

typedef void (*LibQemuCpuEndOfLoopFn)(QemuObject *cpu, void *opaque);
typedef bool (*LibQemuCpuPcEntryFn)(QemuObject *cpu, uint64_t pc, void *opaque);
typedef void (*LibQemuCpuKickFn)(QemuObject *cpu, void *opaque);

void libqemu_set_cpu_end_of_loop_cb(LibQemuCpuEndOfLoopFn cb, void *opaque);
void libqemu_set_cpu_pc_entry_cb(LibQemuCpuPcEntryFn cb, void *opaque);
void libqemu_add_cpu_pc_entry_watch(uint64_t pc);
void libqemu_clear_cpu_pc_entry_watches(void);
uint64_t libqemu_get_cpu_pc_entry_watch_count(void);
uint64_t libqemu_get_cpu_pc_entry_watch_add_calls(void);
uint64_t libqemu_get_cpu_pc_entry_watch_clear_calls(void);
uint64_t libqemu_get_cpu_pc_entry_watch_match_queries(void);
uint64_t libqemu_get_cpu_pc_entry_watch_match_hits(void);
uint64_t libqemu_get_cpu_pc_entry_watch_last_pc(void);
uint64_t libqemu_get_cpu_pc_entry_watch_last_watch_pc(void);
void libqemu_set_cpu_kick_cb(LibQemuCpuKickFn cb, void *opaque);
void libqemu_set_iommu_translate_cb(LibQemuIOMMUTranslateFn cb, void *opaque);

void libqemu_enable_opengl(void);
void libqemu_set_autostart(int);

#endif
