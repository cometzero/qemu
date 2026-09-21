/*
 * Apollo application-processor Linux boot platform.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is the Linux-facing subset of the Apollo QBox physical map. It
 * intentionally does not instantiate the RSE or Safety Island firmware.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "system/system.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/char/pl011.h"
#include "hw/intc/arm_gicv3_common.h"
#include "qobject/qlist.h"
#include "target/arm/cpu.h"
#include "target/arm/gtimer.h"

#define TYPE_APOLLO_MACHINE MACHINE_TYPE_NAME("apollo-qvp")
OBJECT_DECLARE_SIMPLE_TYPE(ApolloMachineState, APOLLO_MACHINE)

#define APOLLO_RAM_BASE 0x80000000ULL
#define APOLLO_RAM_LOW 0x7f000000ULL
#define APOLLO_RAM_HIGH_BASE 0x20000000000ULL
#define APOLLO_RAM_HIGH (2 * GiB)
#define APOLLO_GICD 0x20800000
#define APOLLO_GICR 0x20880000
#define APOLLO_GICR_STRIDE 0x40000
#define APOLLO_SPI_COUNT 480
#define APOLLO_CLOCK 125000000

struct ApolloMachineState {
    MachineState parent_obj;
    struct arm_boot_info bootinfo;
    MemoryRegion low_ram;
    MemoryRegion high_ram;
    DeviceState *gic;
    void *fdt;
    int fdt_size;
};

static uint64_t apollo_mpidr(unsigned int cpu)
{
    return ((cpu / 4) << 16) | ((cpu % 4) << 8);
}

static void apollo_fdt_device(ApolloMachineState *s, const char *node,
                              const char *compatible, hwaddr base,
                              hwaddr size, unsigned int spi)
{
    qemu_fdt_add_subnode(s->fdt, node);
    qemu_fdt_setprop_string(s->fdt, node, "compatible", compatible);
    qemu_fdt_setprop_sized_cells(s->fdt, node, "reg", 2, base, 2, size);
    qemu_fdt_setprop_cells(s->fdt, node, "interrupts", 0, spi, 4);
}

static void apollo_create_fdt(ApolloMachineState *s)
{
    MachineState *ms = MACHINE(s);
    const char *gic = "/interrupt-controller@20800000";
    g_autofree uint32_t *reg = g_new0(uint32_t, (ms->smp.cpus + 1) * 4);

    s->fdt = create_device_tree(&s->fdt_size);
    qemu_fdt_setprop_string(s->fdt, "/", "model", "Apollo QEMU Linux platform");
    qemu_fdt_setprop_string(s->fdt, "/", "compatible", "arm,apollo-qvp");
    qemu_fdt_setprop_cell(s->fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(s->fdt, "/", "#size-cells", 2);
    qemu_fdt_setprop_cell(s->fdt, "/", "interrupt-parent", 1);
    qemu_fdt_add_subnode(s->fdt, "/chosen");
    qemu_fdt_setprop_string(s->fdt, "/chosen", "stdout-path",
                            "/serial@1a400000");
    qemu_fdt_add_subnode(s->fdt, "/cpus");
    qemu_fdt_setprop_cell(s->fdt, "/cpus", "#address-cells", 2);
    qemu_fdt_setprop_cell(s->fdt, "/cpus", "#size-cells", 0);
    for (int i = 0; i < ms->smp.cpus; i++) {
        ARMCPU *cpu = ARM_CPU(qemu_get_cpu(i));
        g_autofree char *node = g_strdup_printf("/cpus/cpu@%" PRIx64,
                                               apollo_mpidr(i));
        qemu_fdt_add_subnode(s->fdt, node);
        qemu_fdt_setprop_string(s->fdt, node, "device_type", "cpu");
        qemu_fdt_setprop_string(s->fdt, node, "compatible",
                                cpu->dtb_compatible);
        qemu_fdt_setprop_sized_cells(s->fdt, node, "reg", 2, apollo_mpidr(i));
        qemu_fdt_setprop_string(s->fdt, node, "enable-method", "psci");
    }
    qemu_fdt_add_subnode(s->fdt, "/psci");
    qemu_fdt_setprop_string(s->fdt, "/psci", "compatible", "arm,psci-1.0");
    qemu_fdt_setprop_string(s->fdt, "/psci", "method", "smc");
    qemu_fdt_add_subnode(s->fdt, "/timer");
    qemu_fdt_setprop_string(s->fdt, "/timer", "compatible", "arm,armv8-timer");
    qemu_fdt_setprop_cells(s->fdt, "/timer", "interrupts",
                           1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4);
    qemu_fdt_setprop(s->fdt, "/timer", "always-on", NULL, 0);
    qemu_fdt_add_subnode(s->fdt, gic);
    qemu_fdt_setprop_string(s->fdt, gic, "compatible", "arm,gic-v3");
    qemu_fdt_setprop_cell(s->fdt, gic, "#interrupt-cells", 3);
    qemu_fdt_setprop_cell(s->fdt, gic, "phandle", 1);
    qemu_fdt_setprop(s->fdt, gic, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(s->fdt, gic, "#redistributor-regions", ms->smp.cpus);
    qemu_fdt_setprop_cells(s->fdt, gic, "interrupts", 1, 9, 4);
    reg[1] = cpu_to_be32(APOLLO_GICD);
    reg[3] = cpu_to_be32(0x10000);
    for (int i = 0; i < ms->smp.cpus; i++) {
        reg[4 * (i + 1) + 1] =
            cpu_to_be32(APOLLO_GICR + i * APOLLO_GICR_STRIDE);
        reg[4 * (i + 1) + 3] = cpu_to_be32(0x20000);
    }
    qemu_fdt_setprop(s->fdt, gic, "reg", reg, (ms->smp.cpus + 1) * 16);
    qemu_fdt_add_subnode(s->fdt, "/clock-24mhz");
    qemu_fdt_setprop_string(s->fdt, "/clock-24mhz", "compatible",
                            "fixed-clock");
    qemu_fdt_setprop_cell(s->fdt, "/clock-24mhz", "#clock-cells", 0);
    qemu_fdt_setprop_cell(s->fdt, "/clock-24mhz", "clock-frequency", 24000000);
    qemu_fdt_setprop_cell(s->fdt, "/clock-24mhz", "phandle", 2);
    apollo_fdt_device(s, "/serial@1a400000", "arm,pl011",
                      0x1a400000, 0x1000, 52);
    qemu_fdt_setprop(s->fdt, "/serial@1a400000", "compatible",
                     "arm,pl011\0arm,primecell",
                     sizeof("arm,pl011\0arm,primecell"));
    qemu_fdt_setprop_cells(s->fdt, "/serial@1a400000", "clocks", 2, 2);
    qemu_fdt_setprop(s->fdt, "/serial@1a400000", "clock-names",
                     "uartclk\0apb_pclk", sizeof("uartclk\0apb_pclk"));
    apollo_fdt_device(s, "/rtc@300d0000", "arm,pl031", 0x300d0000, 0x1000, 268);
    qemu_fdt_setprop(s->fdt, "/rtc@300d0000", "compatible",
                     "arm,pl031\0arm,primecell",
                     sizeof("arm,pl031\0arm,primecell"));
    qemu_fdt_setprop_cell(s->fdt, "/rtc@300d0000", "clocks", 2);
    qemu_fdt_setprop_string(s->fdt, "/rtc@300d0000", "clock-names", "apb_pclk");
}

static void *apollo_get_dtb(const struct arm_boot_info *info, int *size)
{
    ApolloMachineState *s = container_of(info, ApolloMachineState, bootinfo);
    *size = s->fdt_size;
    return g_memdup2(s->fdt, s->fdt_size);
}

static void apollo_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    ApolloMachineState *s = container_of(info, ApolloMachineState, bootinfo);
    uint64_t high_size = MACHINE(s)->ram_size - info->ram_size;
    /* The loader only uses the contiguous low bank for boot payloads. */
    if (high_size) {
        qemu_fdt_add_subnode(fdt, "/memory@20000000000");
        qemu_fdt_setprop_string(fdt, "/memory@20000000000", "device_type",
                                "memory");
        qemu_fdt_setprop_sized_cells(fdt, "/memory@20000000000", "reg",
                                     2, APOLLO_RAM_HIGH_BASE, 2, high_size);
    }
}

static void apollo_init(MachineState *ms)
{
    ApolloMachineState *s = APOLLO_MACHINE(ms);
    MemoryRegion *sysmem = get_system_memory();
    uint64_t low_size = MIN(ms->ram_size, APOLLO_RAM_LOW);
    QList *regions = qlist_new();
    const hwaddr virtio_base[] = { 0x30020000, 0x30060000, 0x30080000 };
    const unsigned int virtio_spi[] = { 257, 261, 263 };

    if (ms->ram_size < 256 * MiB ||
        ms->ram_size > APOLLO_RAM_LOW + APOLLO_RAM_HIGH) {
        error_report("apollo-qvp requires 256 to 4080 MiB of RAM");
        exit(1);
    }
    memory_region_init_alias(&s->low_ram, OBJECT(s), "apollo.low-ram",
                              ms->ram, 0, low_size);
    memory_region_add_subregion(sysmem, APOLLO_RAM_BASE, &s->low_ram);
    if (ms->ram_size > low_size) {
        memory_region_init_alias(&s->high_ram, OBJECT(s), "apollo.high-ram",
                                  ms->ram, low_size, ms->ram_size - low_size);
        memory_region_add_subregion(sysmem, APOLLO_RAM_HIGH_BASE, &s->high_ram);
    }
    for (int i = 0; i < ms->smp.cpus; i++) {
        Object *cpu = object_new(ms->cpu_type);
        object_property_set_int(cpu, "mp-affinity", apollo_mpidr(i),
                                &error_fatal);
        object_property_set_int(cpu, "cntfrq", APOLLO_CLOCK, &error_fatal);
        object_property_set_bool(cpu, "has_el3", false, &error_fatal);
        object_property_set_bool(cpu, "has_el2", true, &error_fatal);
        object_property_set_link(cpu, "memory", OBJECT(sysmem), &error_fatal);
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
        object_unref(cpu);
        qlist_append_int(regions, 1);
    }
    s->gic = qdev_new("arm-gicv3");
    qdev_prop_set_uint32(s->gic, "revision", 3);
    qdev_prop_set_uint32(s->gic, "num-cpu", ms->smp.cpus);
    qdev_prop_set_uint32(s->gic, "num-irq", APOLLO_SPI_COUNT + 32);
    qdev_prop_set_array(s->gic, "redist-region-count", regions);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 0, APOLLO_GICD);
    for (int i = 0; i < ms->smp.cpus; i++) {
        DeviceState *cpu = DEVICE(qemu_get_cpu(i));
        int base = APOLLO_SPI_COUNT + i * GIC_INTERNAL;
        const int timer_irq[] = { ARCH_TIMER_NS_EL1_IRQ, ARCH_TIMER_VIRT_IRQ,
            ARCH_TIMER_NS_EL2_IRQ, ARCH_TIMER_S_EL1_IRQ,
            ARCH_TIMER_NS_EL2_VIRT_IRQ, ARCH_TIMER_S_EL2_IRQ,
            ARCH_TIMER_S_EL2_VIRT_IRQ };
        sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), i + 1,
                        APOLLO_GICR + i * APOLLO_GICR_STRIDE);
        for (int irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpu, irq,
                                  qdev_get_gpio_in(s->gic,
                                                   base + timer_irq[irq]));
        }
        qdev_connect_gpio_out_named(cpu, "gicv3-maintenance-interrupt", 0,
                                   qdev_get_gpio_in(s->gic,
                                                    base + ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpu, "pmu-interrupt", 0,
                                   qdev_get_gpio_in(s->gic,
                                                    base + VIRTUAL_PMU_IRQ));
        for (int irq = 0; irq < 4; irq++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(s->gic), i + irq * ms->smp.cpus,
                               qdev_get_gpio_in(cpu, irq));
        }
    }
    pl011_create(0x1a400000, qdev_get_gpio_in(s->gic, 52), serial_hd(0));
    sysbus_create_simple("pl031", 0x300d0000, qdev_get_gpio_in(s->gic, 268));
    apollo_create_fdt(s);
    for (int i = 0; i < ARRAY_SIZE(virtio_base); i++) {
        DeviceState *dev = qdev_new("virtio-mmio");
        g_autofree char *node = g_strdup_printf("/virtio@%" PRIx64,
                                               virtio_base[i]);
        qdev_prop_set_bit(dev, "force-legacy", false);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, virtio_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qdev_get_gpio_in(s->gic, virtio_spi[i]));
        apollo_fdt_device(s, node, "virtio,mmio", virtio_base[i],
                          0x200, virtio_spi[i]);
        qemu_fdt_setprop(s->fdt, node, "dma-coherent", NULL, 0);
    }
    s->bootinfo.ram_size = low_size;
    s->bootinfo.loader_start = APOLLO_RAM_BASE;
    s->bootinfo.board_id = -1;
    s->bootinfo.get_dtb = apollo_get_dtb;
    s->bootinfo.modify_dtb = apollo_modify_dtb;
    s->bootinfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    arm_load_kernel(ARM_CPU(first_cpu), ms, &s->bootinfo);
}

static void apollo_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Apollo application-processor Linux platform";
    mc->init = apollo_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a720ae");
    mc->default_ram_size = APOLLO_RAM_LOW + APOLLO_RAM_HIGH;
    mc->default_ram_id = "apollo.ram";
    mc->default_cpus = 4;
    mc->max_cpus = 16;
    mc->no_cdrom = true;
    mc->no_floppy = true;
}

static const TypeInfo apollo_machine_type = {
    .name = TYPE_APOLLO_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(ApolloMachineState),
    .class_init = apollo_class_init,
    .interfaces = aarch64_machine_interfaces,
};

static void apollo_register_types(void)
{
    type_register_static(&apollo_machine_type);
}
type_init(apollo_register_types)
