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

#ifndef _LIBQEMU_H
#define _LIBQEMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef QEMU_BUILD_BUG_ON
#define QEMU_BUILD_BUG_ON(x) static_assert(!(x), "not expecting " #x)
#endif

#ifdef __cplusplus
extern "C" {
#endif


#ifdef _WIN32
#ifdef LIBQEMU_BUILD
#define LIBQEMU_API __declspec(dllexport)
#else
#define LIBQEMU_API __declspec(dllimport)
#endif
#else
#define LIBQEMU_API
#endif

#include "libqemu/exports/struct.h"
#include "libqemu/exports/typedefs.h"

#define libqemu_strify_(a) #a
#define libqemu_strify(a) libqemu_strify_(a)
#define LIBQEMU_INIT_SYM libqemu_init
#define LIBQEMU_INIT_SYM_STR libqemu_strify(LIBQEMU_INIT_SYM)
#define LIBQEMU_ABI_VERSION 2U
#define LIBQEMU_INIT_V2_SYM libqemu_init_v2
#define LIBQEMU_INIT_V2_SYM_STR libqemu_strify(LIBQEMU_INIT_V2_SYM)
#define LIBQEMU_V2_MIN_STRUCT_SIZE \
    (offsetof(LibQemuExports, error_get_pretty) + \
     sizeof(((LibQemuExports *)0)->error_get_pretty))
#define LIBQEMU_ARM_TIMER_REQUIRED_STRUCT_SIZE \
    (offsetof(LibQemuExports, arm_sse_timer_snapshot) + \
     sizeof(((LibQemuExports *)0)->arm_sse_timer_snapshot))


typedef LibQemuExports *(*LibQemuInitFct)(int argc, char **argv);
typedef LibQemuExports *(*LibQemuInitV2Fct)(int argc, char **argv,
                                            uint32_t requested_abi,
                                            size_t caller_struct_size,
                                            size_t *actual_size);

LIBQEMU_API LibQemuExports *LIBQEMU_INIT_SYM(int argc, char **argv);
LIBQEMU_API LibQemuExports *LIBQEMU_INIT_V2_SYM(int argc, char **argv,
                                                uint32_t requested_abi,
                                                size_t caller_struct_size,
                                                size_t *actual_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
