/*
 * QTest coverage for GICv3 extended-range GPIO delivery.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/intc/gicv3_internal.h"

void device_class_set_parent_realize(DeviceClass *dc,
                                     DeviceRealize dev_realize,
                                     DeviceRealize *parent_realize)
{
    *parent_realize = NULL;
}

#include "../../hw/intc/arm_gicv3.c"

#define NORMAL_SPI_COUNT (992 - GIC_INTERNAL)
#define ESPI_GPIO_BASE NORMAL_SPI_COUNT
#define PPI_GPIO_BASE (ESPI_GPIO_BASE + GICV3_MAX_ESPI)
#define EPPI_GPIO_BASE (PPI_GPIO_BASE + 2 * GIC_INTERNAL)
#define REAL_GIC_PATH "/machine/unattached/device[1]"
#define REAL_SMP2_GIC_PATH "/machine/unattached/device[2]"
#define REAL_GICD_BASE 0x08000000
#define REAL_GICR_SGI_BASE 0x080b0000
#define REAL_GICR_CPU1_SGI_BASE 0x080d0000

typedef struct GPIODeliveryFixture {
    GICv3State gic;
    GICv3CPUState cpu[2];
} GPIODeliveryFixture;

void gicv3_cpuif_update(GICv3CPUState *cs)
{
}

void gicv3_init_cpuif(GICv3State *s)
{
}

void gicv3_init_irqs_and_mmio(GICv3State *s, qemu_irq_handler handler,
                              const MemoryRegionOps *ops)
{
}

MemTxResult gicv3_redist_read(void *opaque, hwaddr offset, uint64_t *data,
                              unsigned size, MemTxAttrs attrs)
{
    return MEMTX_OK;
}

MemTxResult gicv3_redist_write(void *opaque, hwaddr offset, uint64_t data,
                               unsigned size, MemTxAttrs attrs)
{
    return MEMTX_OK;
}

void gicv3_redist_update_lpi_only(GICv3CPUState *cs)
{
}

void gicv3_dist_set_irq(GICv3State *s, int irq, int level)
{
    g_assert_not_reached();
}

void gicv3_redist_set_irq(GICv3CPUState *cs, int irq, int level)
{
    g_assert_not_reached();
}

static void fixture_init(GPIODeliveryFixture *f)
{
    f->gic.num_cpu = G_N_ELEMENTS(f->cpu);
    f->gic.num_irq = 992;
    f->gic.num_espi = GICV3_MAX_ESPI;
    f->gic.num_eppi = GICV3_MAX_EPPI;
    f->gic.gicd_ctlr = GICD_CTLR_DS | GICD_CTLR_EN_GRP0;
    f->gic.cpu = f->cpu;
    for (size_t i = 0; i < G_N_ELEMENTS(f->cpu); i++) {
        f->cpu[i].gic = &f->gic;
        f->cpu[i].hppi.prio = 0xff;
    }
}

static void test_normal_spi_gpio_pin(void)
{
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3 -m 64M -nodefaults");

    qtest_set_irq_in(qts, REAL_GIC_PATH, NULL, 0, 1);
    g_assert_cmphex(qtest_readl(qts, REAL_GICD_BASE + 0x204) & BIT(0),
                    ==, BIT(0));
    qtest_quit(qts);
}

static void test_normal_ppi_gpio_pin(void)
{
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3 -m 64M -nodefaults");

    qtest_set_irq_in(qts, REAL_GIC_PATH, NULL, 256 + 31, 1);
    g_assert_cmphex(qtest_readl(qts, REAL_GICR_SGI_BASE + 0x200) & BIT(31),
                    ==, BIT(31));
    qtest_quit(qts);
}

static void test_espi_gpio_delivery(void)
{
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3 -m 64M -nodefaults "
        "-global arm-gicv3.num-espi=1024");

    qtest_set_irq_in(qts, REAL_GIC_PATH, NULL, 256, 1);
    g_assert_cmphex(qtest_readl(qts, REAL_GICD_BASE + GICD_ISPENDRnE),
                    ==, BIT(0));
    qtest_set_irq_in(qts, REAL_GIC_PATH, NULL,
                     256 + GICV3_MAX_ESPI - 1, 1);
    g_assert_cmphex(qtest_readl(qts, REAL_GICD_BASE + GICD_ISPENDRnE +
                               (GICV3_MAX_ESPI / 32 - 1) * 4), ==,
                    BIT(31));
    qtest_set_irq_in(qts, REAL_GIC_PATH, NULL, 256, 0);
    qtest_set_irq_in(qts, REAL_GIC_PATH, NULL,
                     256 + GICV3_MAX_ESPI - 1, 0);
    qtest_quit(qts);
}

static void test_eppi_gpio_delivery(void)
{
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3 -m 64M -nodefaults -smp 2 "
        "-global arm-gicv3.num-eppi=64");
    unsigned int eppi_base = 256 + 2 * GIC_INTERNAL;

    qtest_set_irq_in(qts, REAL_SMP2_GIC_PATH, NULL,
                     eppi_base + GICV3_MAX_EPPI, 1);
    g_assert_cmphex(qtest_readl(qts, REAL_GICR_CPU1_SGI_BASE + 0x204),
                    ==, BIT(0));
    qtest_set_irq_in(qts, REAL_SMP2_GIC_PATH, NULL,
                     eppi_base + 2 * GICV3_MAX_EPPI - 1, 1);
    g_assert_cmphex(qtest_readl(qts, REAL_GICR_CPU1_SGI_BASE + 0x208),
                    ==, BIT(31));
    qtest_set_irq_in(qts, REAL_SMP2_GIC_PATH, NULL,
                     eppi_base + GICV3_MAX_EPPI, 0);
    qtest_set_irq_in(qts, REAL_SMP2_GIC_PATH, NULL,
                     eppi_base + 2 * GICV3_MAX_EPPI - 1, 0);
    qtest_quit(qts);
}

static void test_sgi_gpio_rejected(void)
{
    if (g_test_subprocess()) {
        GPIODeliveryFixture f = { 0 };

        fixture_init(&f);
        gicv3_set_irq(&f.gic, PPI_GPIO_BASE, 1);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
}

static void test_invalid_gpio_rejected(void)
{
    if (g_test_subprocess()) {
        GPIODeliveryFixture f = { 0 };
        int count;

        fixture_init(&f);
        g_assert_true(gicv3_gpio_count(&f.gic, &count));
        gicv3_set_irq(&f.gic, count, 1);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
}

static void test_reserved_intid_holes_rejected(void)
{
    GPIODeliveryFixture f = { 0 };
    static const uint32_t holes[] = { 992, 1120, 4095, 5120 };

    fixture_init(&f);
    for (size_t i = 0; i < G_N_ELEMENTS(holes); i++) {
        g_assert_false(gicv3_intid_to_irq(&f.gic, holes[i], 0, NULL));
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv3-ext-range-gpio-delivery/pin/spi",
                   test_normal_spi_gpio_pin);
    qtest_add_func("/arm-gicv3-ext-range-gpio-delivery/pin/ppi",
                   test_normal_ppi_gpio_pin);
    qtest_add_func("/arm-gicv3-ext-range-gpio-delivery/espi/last-owner",
                   test_espi_gpio_delivery);
    qtest_add_func("/arm-gicv3-ext-range-gpio-delivery/eppi/last-owner",
                   test_eppi_gpio_delivery);
    qtest_add_func("/arm-gicv3-ext-range-gpio-delivery/reject/sgi",
                   test_sgi_gpio_rejected);
    qtest_add_func("/arm-gicv3-ext-range-gpio-delivery/reject/gpio-end",
                   test_invalid_gpio_rejected);
    qtest_add_func("/arm-gicv3-ext-range-gpio-delivery/reject/intid-holes",
                   test_reserved_intid_holes_rejected);
    return g_test_run();
}
