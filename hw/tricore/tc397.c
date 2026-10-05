/*
 * Minimal Infineon AURIX TC397B firmware development machine.
 *
 * Memory layout and peripheral wiring adapted from Linumiz qemu-tricore
 * v1.0.0 (9e888198363d89048baf3776b7784f2680703609), tc39xb_soc.c:
 * Copyright (c) 2020 Andreas Konopik <andreas.konopik@efs-auto.de>
 * Copyright (c) 2020 David Brenken <david.brenken@efs-auto.de>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/tricore_asclin.h"
#include "hw/gpio/tricore_port.h"
#include "hw/net/tc397_can.h"
#include "hw/intc/tricore_ir.h"
#include "hw/timer/tricore_stm.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "cpu.h"

#define TYPE_TC397_MACHINE MACHINE_TYPE_NAME("KIT_AURIX_TC397B_TRB")
OBJECT_DECLARE_SIMPLE_TYPE(TC397MachineState, TC397_MACHINE)

#define TC397_DSPR_BASE       0x70000000
#define TC397_PSPR_BASE       0x70100000
#define TC397_FLASH_BASE      0x80000000
#define TC397_FLASH_ALIAS     0xa0000000
#define TC397_PSPR_ALIAS      0xc0000000
#define TC397_DSPR_ALIAS      0xd0000000
#define TC397_ASCLIN_BASE     0xf0000600
#define TC397_ASCLIN_STRIDE   0x100
#define TC397_STM_BASE        0xf0001000
#define TC397_INT_BASE        0xf0037000
#define TC397_SRC_BASE        0xf0038000
#define TC397_PORT0_BASE      0xf003a000

#define TC397_SRC_ASCLIN_TX   0x14
#define TC397_SRC_ASCLIN_RX   0x15
#define TC397_SRC_ASCLIN_ERR  0x16
#define TC397_SRC_ASCLIN_STRIDE 3
#define TC397_SRC_STM_SR0     0xc0
#define TC397_SRC_STM_SR1     0xc1

struct TC397MachineState {
    MachineState parent_obj;
    TriCoreCPU *cpu;
    MemoryRegion dspr;
    MemoryRegion pspr;
    MemoryRegion flash;
    MemoryRegion dspr_alias;
    MemoryRegion pspr_alias;
    MemoryRegion flash_alias;
    uint32_t entry;
};

static void tc397_reset(void *opaque)
{
    TC397MachineState *s = opaque;

    cpu_reset(CPU(s->cpu));
    cpu_set_pc(CPU(s->cpu), s->entry);
}

static void tc397_map_alias(MemoryRegion *alias, Object *owner,
                            const char *name, MemoryRegion *original,
                            hwaddr address)
{
    memory_region_init_alias(alias, owner, name, original, 0,
                             memory_region_size(original));
    memory_region_add_subregion(get_system_memory(), address, alias);
}

static DeviceState *tc397_new_device(MachineState *machine, const char *name,
                                    const char *type)
{
    DeviceState *dev = qdev_new(type);

    object_property_add_child(OBJECT(machine), name, OBJECT(dev));
    return dev;
}

static void tc397_init(MachineState *machine)
{
    TC397MachineState *s = TC397_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *ir, *uart, *stm, *port, *can;
    Clock *fstm;

    s->cpu = TRICORE_CPU(cpu_create(machine->cpu_type));
    s->entry = TC397_FLASH_ALIAS;

    memory_region_init_ram(&s->dspr, NULL, "tc397.dspr0",
                           240 * KiB, &error_fatal);
    memory_region_init_ram(&s->pspr, NULL, "tc397.pspr0",
                           64 * KiB, &error_fatal);
    /* ELF loading is supported; flash programming and UCB/BMHD are not. */
    memory_region_init_rom(&s->flash, NULL, "tc397.pflash0",
                           3 * MiB, &error_fatal);
    memory_region_add_subregion(sysmem, TC397_DSPR_BASE, &s->dspr);
    memory_region_add_subregion(sysmem, TC397_PSPR_BASE, &s->pspr);
    memory_region_add_subregion(sysmem, TC397_FLASH_BASE, &s->flash);
    tc397_map_alias(&s->dspr_alias, OBJECT(machine), "tc397.local-dspr",
                    &s->dspr, TC397_DSPR_ALIAS);
    tc397_map_alias(&s->pspr_alias, OBJECT(machine), "tc397.local-pspr",
                    &s->pspr, TC397_PSPR_ALIAS);
    tc397_map_alias(&s->flash_alias, OBJECT(machine), "tc397.uncached-flash",
                    &s->flash, TC397_FLASH_ALIAS);

    ir = tc397_new_device(machine, "ir", TYPE_TRICORE_IR);
    qdev_prop_set_uint16(ir, "num-irqs", 512);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ir), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ir), 0, TC397_INT_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(ir), 1, TC397_SRC_BASE);
    qdev_connect_gpio_out_named(ir, "priority", 0,
        qdev_get_gpio_in_named(DEVICE(s->cpu), "irq-priority", 0));
    qdev_connect_gpio_out_named(DEVICE(s->cpu), "irq-ack", 0,
        qdev_get_gpio_in_named(ir, "ack", 0));

    /* TC39x-B ASCLINn: base + n * 0x100; three consecutive SRCs each. */
    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("asclin%u", i);
        unsigned src_offset = i * TC397_SRC_ASCLIN_STRIDE;

        uart = tc397_new_device(machine, name, TYPE_TRICORE_ASCLIN);
        qdev_prop_set_chr(uart, "chardev", serial_hd(i));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0,
                       TC397_ASCLIN_BASE + i * TC397_ASCLIN_STRIDE);
        sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
            qdev_get_gpio_in_named(ir, "irq", TC397_SRC_ASCLIN_RX + src_offset));
        sysbus_connect_irq(SYS_BUS_DEVICE(uart), 1,
            qdev_get_gpio_in_named(ir, "irq", TC397_SRC_ASCLIN_TX + src_offset));
        sysbus_connect_irq(SYS_BUS_DEVICE(uart), 2,
            qdev_get_gpio_in_named(ir, "irq", TC397_SRC_ASCLIN_ERR + src_offset));
    }

    port = tc397_new_device(machine, "port0", TYPE_TRICORE_PORT);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(port), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(port), 0, TC397_PORT0_BASE);

    can = tc397_new_device(machine, "can0", TYPE_TC397_CAN);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(can), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(can), 0, 0xf0208000);
    sysbus_mmio_map(SYS_BUS_DEVICE(can), 1, 0xf0200000);
    for (unsigned i = 0; i < 16; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(can), i,
            qdev_get_gpio_in_named(ir, "irq", 0x16c + i));
    }

    stm = tc397_new_device(machine, "stm0", TYPE_TRICORE_STM);
    fstm = clock_new(OBJECT(machine), "fstm");
    clock_set_hz(fstm, 50000000);
    qdev_connect_clock_in(stm, "fstm", fstm);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(stm), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(stm), 0, TC397_STM_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(stm), 0,
        qdev_get_gpio_in_named(ir, "irq", TC397_SRC_STM_SR0));
    sysbus_connect_irq(SYS_BUS_DEVICE(stm), 1,
        qdev_get_gpio_in_named(ir, "irq", TC397_SRC_STM_SR1));

    if (machine->kernel_filename) {
        uint64_t entry;
        ssize_t loaded = load_elf(machine->kernel_filename, NULL, NULL, NULL,
                                   &entry, NULL, NULL, NULL, ELFDATA2LSB,
                                   EM_TRICORE, 1, 0);

        if (loaded <= 0 || entry > UINT32_MAX || (entry & 1)) {
            error_report("TC397 requires a loadable TriCore ELF with an "
                         "aligned 32-bit entry point");
            exit(EXIT_FAILURE);
        }
        s->entry = entry;
    }
    /* Direct ELF boot, including subsequent QMP system_reset requests. */
    qemu_register_reset(tc397_reset, s);
}

static void tc397_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        TRICORE_CPU_TYPE_NAME("tc397"), NULL,
    };

    mc->desc = "Infineon AURIX TC397B (minimal single-core model)";
    mc->init = tc397_init;
    mc->default_cpu_type = TRICORE_CPU_TYPE_NAME("tc397");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->default_ram_size = 0;
}

static const TypeInfo tc397_type = {
    .name = TYPE_TC397_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(TC397MachineState),
    .class_init = tc397_class_init,
};

static void tc397_register_types(void)
{
    type_register_static(&tc397_type);
}

type_init(tc397_register_types)
