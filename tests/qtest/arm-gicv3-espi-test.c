/*
 * QTest coverage for the Arm GICv3 extended SPI distributor registers.
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

#define GICD_BASE 0x08000000
#define GIC_PATH "/machine/unattached/device[1]"

typedef struct ESPIRegisterCase {
    const char *name;
    uint32_t intid;
    uint32_t mask;
    uint32_t group;
    uint32_t enable_set;
    uint32_t enable_clear;
    uint32_t pending_set;
    uint32_t pending_clear;
    uint32_t active_set;
    uint32_t active_clear;
    uint32_t priority;
    uint32_t config;
    uint32_t config_mask;
    uint32_t router;
    uint64_t route;
} ESPIRegisterCase;

static const ESPIRegisterCase espi_cases[] = {
    {
        .name = "first",
        .intid = 4096,
        .mask = BIT(0),
        .group = 0x1000,
        .enable_set = 0x1200,
        .enable_clear = 0x1400,
        .pending_set = 0x1600,
        .pending_clear = 0x1800,
        .active_set = 0x1a00,
        .active_clear = 0x1c00,
        .priority = 0x2000,
        .config = 0x3000,
        .config_mask = BIT(1),
        .router = 0x8000,
        .route = 0,
    },
    {
        .name = "middle",
        .intid = 4608,
        .mask = BIT(0),
        .group = 0x1040,
        .enable_set = 0x1240,
        .enable_clear = 0x1440,
        .pending_set = 0x1640,
        .pending_clear = 0x1840,
        .active_set = 0x1a40,
        .active_clear = 0x1c40,
        .priority = 0x2200,
        .config = 0x3080,
        .config_mask = BIT(1),
        .router = 0x9000,
        .route = 1,
    },
    {
        .name = "last",
        .intid = 5119,
        .mask = BIT(31),
        .group = 0x107c,
        .enable_set = 0x127c,
        .enable_clear = 0x147c,
        .pending_set = 0x167c,
        .pending_clear = 0x187c,
        .active_set = 0x1a7c,
        .active_clear = 0x1c7c,
        .priority = 0x23ff,
        .config = 0x30fc,
        .config_mask = BIT(31),
        .router = 0x9ff8,
        .route = 0x80000000,
    },
};

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

void gicv3_set_irq(void *opaque, int irq, int level)
{
}

static uint64_t direct_dist_read(GICv3State *s, hwaddr offset,
                                 unsigned size, bool secure)
{
    MemTxAttrs attrs = { .secure = secure };
    uint64_t value = UINT64_MAX;

    g_assert_cmpint(gicv3_dist_read(s, offset, &value, size, attrs), ==,
                    MEMTX_OK);
    return value;
}

static void direct_dist_write(GICv3State *s, hwaddr offset, uint64_t value,
                              unsigned size, bool secure)
{
    MemTxAttrs attrs = { .secure = secure };

    g_assert_cmpint(gicv3_dist_write(s, offset, value, size, attrs), ==,
                    MEMTX_OK);
}

static QTestState *gicv3_qtest_start(const char *extra_args)
{
    return qtest_initf("-machine virt,gic-version=3 -m 64M "
                       "-nodefaults %s", extra_args ? extra_args : "");
}

static QTestState *gicv3_espi_qtest_start(void)
{
    return gicv3_qtest_start("-global arm-gicv3.num-espi=1024");
}

static void test_pin_normal_distributor(void)
{
    /* Given: the unchanged software GIC with no extended SPI range. */
    QTestState *qts = gicv3_qtest_start(NULL);

    /* When: normal discovery, SPI32 state, and routing are programmed. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x0004), ==, 0x037a0008);
    qtest_writel(qts, GICD_BASE + 0x0104, BIT(0));
    qtest_writeq(qts, GICD_BASE + 0x6100, 1);

    /* Then: exact normal-bank values survive and reset independently. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x0104), ==, BIT(0));
    g_assert_cmphex(qtest_readq(qts, GICD_BASE + 0x6100), ==, 1);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x0104), ==, 0);
    g_assert_cmphex(qtest_readq(qts, GICD_BASE + 0x6100), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x0004), ==, 0x037a0008);
    qtest_quit(qts);
}

static void test_espi_discovery(void)
{
    /* Given: devices with minimum, middle, and maximum ESPI counts. */
    QTestState *minimum = gicv3_qtest_start(
        "-global arm-gicv3.num-espi=32");
    QTestState *middle = gicv3_qtest_start(
        "-global arm-gicv3.num-espi=512");
    QTestState *maximum = gicv3_espi_qtest_start();

    /* When: software discovers the extended range through GICD_TYPER. */
    uint32_t minimum_typer = qtest_readl(minimum, GICD_BASE + 0x0004);
    uint32_t middle_typer = qtest_readl(middle, GICD_BASE + 0x0004);
    uint32_t maximum_typer = qtest_readl(maximum, GICD_BASE + 0x0004);

    /* Then: ESPI and ESPI_Range are exact literal architectural values. */
    g_assert_cmphex(minimum_typer, ==, 0x037a011e);
    g_assert_cmphex(middle_typer, ==, 0x7b7a011e);
    g_assert_cmphex(maximum_typer, ==, 0xfb7a011e);
    qtest_quit(minimum);
    qtest_quit(middle);
    qtest_quit(maximum);
}

static void test_espi_group_priority_config_route(void)
{
    /* Given: first, middle, and last ESPIs in a 1024-ESPI distributor. */
    QTestState *qts = gicv3_espi_qtest_start();

    /* When: each independent group/config/priority/route address is written. */
    for (size_t i = 0; i < G_N_ELEMENTS(espi_cases); i++) {
        const ESPIRegisterCase *test = &espi_cases[i];
        uint8_t priority = 0x20 + i * 0x20;

        g_test_message("ESPI %s INTID %u", test->name, test->intid);
        qtest_writel(qts, GICD_BASE + test->group, test->mask);
        qtest_writel(qts, GICD_BASE + test->config, test->config_mask);
        qtest_writeb(qts, GICD_BASE + test->priority, priority);
        qtest_writeq(qts, GICD_BASE + test->router, test->route);

        /* Then: the same family-local address returns the programmed value. */
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->group), ==,
                        test->mask);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->config), ==,
                        test->config_mask);
        g_assert_cmphex(qtest_readb(qts, GICD_BASE + test->priority), ==,
                        priority);
        g_assert_cmphex(qtest_readq(qts, GICD_BASE + test->router), ==,
                        test->route);
    }
    qtest_quit(qts);
}

static void test_espi_enable_pending_active(void)
{
    /* Given: reset first, middle, and last ESPI bitmap state. */
    QTestState *qts = gicv3_espi_qtest_start();

    /* When: each set register is written at its literal MMIO address. */
    for (size_t i = 0; i < G_N_ELEMENTS(espi_cases); i++) {
        const ESPIRegisterCase *test = &espi_cases[i];

        g_test_message("ESPI %s INTID %u", test->name, test->intid);
        qtest_writel(qts, GICD_BASE + test->enable_set, test->mask);
        qtest_writel(qts, GICD_BASE + test->pending_set, test->mask);
        qtest_writel(qts, GICD_BASE + test->active_set, test->mask);

        /* Then: enable, pending, and active become visible and clearable. */
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->enable_set), ==,
                        test->mask);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->pending_set), ==,
                        test->mask);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->active_set), ==,
                        test->mask);
        qtest_writel(qts, GICD_BASE + test->enable_set, 0);
        qtest_writel(qts, GICD_BASE + test->pending_set, 0);
        qtest_writel(qts, GICD_BASE + test->active_set, 0);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->enable_clear), ==,
                        test->mask);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->pending_clear), ==,
                        test->mask);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->active_clear), ==,
                        test->mask);
        qtest_writel(qts, GICD_BASE + test->enable_clear, test->mask);
        qtest_writel(qts, GICD_BASE + test->pending_clear, test->mask);
        qtest_writel(qts, GICD_BASE + test->active_clear, test->mask);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->enable_set), ==, 0);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->pending_set), ==, 0);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->active_set), ==, 0);
    }
    qtest_quit(qts);
}

static void test_espi_pending_line_semantics(void)
{
    /* Given: ESPI4096 first as level-sensitive, then as edge-triggered. */
    QTestState *qts = gicv3_espi_qtest_start();

    /* When: its family-local GPIO is asserted and the pending latch cleared. */
    qtest_set_irq_in(qts, GIC_PATH, NULL, 256, 1);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1600), ==, BIT(0));
    qtest_writel(qts, GICD_BASE + 0x1800, BIT(0));

    /* Then: a high level remains pending, while an edge latches after low. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1600), ==, BIT(0));
    qtest_set_irq_in(qts, GIC_PATH, NULL, 256, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1600), ==, 0);
    qtest_writel(qts, GICD_BASE + 0x3000, BIT(1));
    qtest_set_irq_in(qts, GIC_PATH, NULL, 256, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL, 256, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1600), ==, BIT(0));
    qtest_writel(qts, GICD_BASE + 0x1800, BIT(0));
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1600), ==, 0);
    qtest_quit(qts);
}

static void test_espi_reset_and_normal_nonalias(void)
{
    /* Given: normal SPI32 and first/middle/last ESPIs are programmed. */
    QTestState *qts = gicv3_espi_qtest_start();

    qtest_writel(qts, GICD_BASE + 0x0104, BIT(0));
    for (size_t i = 0; i < G_N_ELEMENTS(espi_cases); i++) {
        const ESPIRegisterCase *test = &espi_cases[i];

        qtest_writel(qts, GICD_BASE + test->enable_set, test->mask);
        qtest_writel(qts, GICD_BASE + test->pending_set, test->mask);
        qtest_writel(qts, GICD_BASE + test->active_set, test->mask);
        qtest_writel(qts, GICD_BASE + test->group, test->mask);
        qtest_writel(qts, GICD_BASE + test->config, test->config_mask);
        qtest_writeb(qts, GICD_BASE + test->priority, 0x60 + i);
        qtest_writeq(qts, GICD_BASE + test->router, 0x100 + i);
    }

    /* When: the whole machine is reset. */
    qtest_system_reset(qts);

    /* Then: every endpoint and the normal bank reset without aliasing. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x0104), ==, 0);
    for (size_t i = 0; i < G_N_ELEMENTS(espi_cases); i++) {
        const ESPIRegisterCase *test = &espi_cases[i];

        g_assert_cmphex(qtest_readl(qts,
                                   GICD_BASE + test->enable_set), ==, 0);
        g_assert_cmphex(qtest_readl(qts,
                                   GICD_BASE + test->pending_set), ==, 0);
        g_assert_cmphex(qtest_readl(qts,
                                   GICD_BASE + test->active_set), ==, 0);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->group), ==, 0);
        g_assert_cmphex(qtest_readl(qts, GICD_BASE + test->config), ==, 0);
        g_assert_cmphex(qtest_readb(qts, GICD_BASE + test->priority), ==, 0);
        g_assert_cmphex(qtest_readq(qts, GICD_BASE + test->router), ==, 0);
    }
    qtest_quit(qts);
}

static void test_espi_opposite_normal_nonalias(void)
{
    /* Given: three normal SPIs and three ESPIs with opposing family state. */
    GICv3State state = {
        .num_irq = 992,
        .num_espi = GICV3_MAX_ESPI,
    };
    const uint32_t normal_group[] = { 0x0084, 0x00c0, 0x00f8 };
    const uint32_t normal_enable[] = { 0x0104, 0x0140, 0x0178 };
    const uint32_t normal_pending[] = { 0x0204, 0x0240, 0x0278 };
    const uint32_t normal_active[] = { 0x0304, 0x0340, 0x0378 };
    const uint32_t normal_priority[] = { 0x0420, 0x0600, 0x07df };
    const uint32_t normal_config[] = { 0x0c08, 0x0c80, 0x0cf4 };
    const uint32_t normal_router[] = { 0x6100, 0x7000, 0x7ef8 };
    const uint32_t masks[] = { BIT(0), BIT(0), BIT(31) };
    const uint32_t config_masks[] = { BIT(1), BIT(1), BIT(31) };

    for (size_t i = 0; i < G_N_ELEMENTS(espi_cases); i++) {
        direct_dist_write(&state, normal_group[i], masks[i], 4, true);
        direct_dist_write(&state, normal_enable[i], masks[i], 4, true);
        direct_dist_write(&state, normal_pending[i], masks[i], 4, true);
        direct_dist_write(&state, normal_active[i], masks[i], 4, true);
        direct_dist_write(&state, normal_priority[i], 0x11 + i, 1, true);
        direct_dist_write(&state, normal_config[i], config_masks[i], 4, true);
        direct_dist_write(&state, normal_router[i], 0x100 + i, 8, true);
    }

    /* When: only the ESPI family is programmed with opposite canaries. */
    for (size_t i = 0; i < G_N_ELEMENTS(espi_cases); i++) {
        const ESPIRegisterCase *test = &espi_cases[i];

        g_assert_cmphex(direct_dist_read(&state, test->enable_set, 4, true),
                        ==, 0);
        g_assert_cmphex(direct_dist_read(&state, test->pending_set, 4, true),
                        ==, 0);
        g_assert_cmphex(direct_dist_read(&state, test->active_set, 4, true),
                        ==, 0);
        direct_dist_write(&state, test->enable_set, test->mask, 4, true);
        direct_dist_write(&state, test->pending_set, test->mask, 4, true);
        direct_dist_write(&state, test->active_set, test->mask, 4, true);
        direct_dist_write(&state, test->priority, 0xee - i, 1, true);
        direct_dist_write(&state, test->config, test->config_mask, 4, true);
        direct_dist_write(&state, test->router, 0xf00 + i, 8, true);
    }

    /* Then: either family can mutate without changing the other family. */
    for (size_t i = 0; i < G_N_ELEMENTS(espi_cases); i++) {
        const ESPIRegisterCase *test = &espi_cases[i];

        g_assert_cmphex(direct_dist_read(&state, normal_group[i], 4, true),
                        ==, masks[i]);
        g_assert_cmphex(direct_dist_read(&state, normal_enable[i], 4, true),
                        ==, masks[i]);
        g_assert_cmphex(direct_dist_read(&state, normal_pending[i], 4, true),
                        ==, masks[i]);
        g_assert_cmphex(direct_dist_read(&state, normal_active[i], 4, true),
                        ==, masks[i]);
        g_assert_cmphex(direct_dist_read(&state, normal_priority[i], 1, true),
                        ==, 0x11 + i);
        g_assert_cmphex(direct_dist_read(&state, normal_config[i], 4, true),
                        ==, config_masks[i]);
        g_assert_cmphex(direct_dist_read(&state, normal_router[i], 8, true),
                        ==, 0x100 + i);
        direct_dist_write(&state, normal_group[i], 0, 4, true);
        direct_dist_write(&state, normal_enable[i] + 0x80, masks[i], 4, true);
        direct_dist_write(&state, normal_pending[i] + 0x80, masks[i], 4,
                          true);
        direct_dist_write(&state, normal_active[i] + 0x80, masks[i], 4, true);
        direct_dist_write(&state, normal_priority[i], 0, 1, true);
        direct_dist_write(&state, normal_config[i], 0, 4, true);
        direct_dist_write(&state, normal_router[i], 0, 8, true);
        g_assert_cmphex(direct_dist_read(&state, test->enable_set, 4, true),
                        ==, test->mask);
        g_assert_cmphex(direct_dist_read(&state, test->pending_set, 4, true),
                        ==, test->mask);
        g_assert_cmphex(direct_dist_read(&state, test->active_set, 4, true),
                        ==, test->mask);
        g_assert_cmphex(direct_dist_read(&state, test->priority, 1, true),
                        ==, 0xee - i);
        g_assert_cmphex(direct_dist_read(&state, test->config, 4, true),
                        ==, test->config_mask);
        g_assert_cmphex(direct_dist_read(&state, test->router, 8, true),
                        ==, 0xf00 + i);
    }

    for (size_t i = 0; i < G_N_ELEMENTS(espi_cases); i++) {
        g_assert_cmphex(direct_dist_read(&state, normal_group[i], 4, true),
                        ==, 0);
        g_assert_cmphex(direct_dist_read(&state, normal_enable[i], 4, true),
                        ==, 0);
        g_assert_cmphex(direct_dist_read(&state, espi_cases[i].enable_set,
                                        4, true), ==, espi_cases[i].mask);
    }
}

static void test_espi_explicit_security_attrs(void)
{
    /* Given: a DS=0 Distributor and explicit Secure/Non-secure requesters. */
    GICv3State state = { .num_espi = 32 };

    direct_dist_write(&state, GICD_ISENABLERnE, BIT(0), 4, true);
    direct_dist_write(&state, GICD_ISPENDRnE, BIT(0), 4, true);
    direct_dist_write(&state, GICD_ISACTIVERnE, BIT(0), 4, true);
    direct_dist_write(&state, GICD_IPRIORITYRnE, 0x20, 1, true);
    direct_dist_write(&state, GICD_ICFGRnE, BIT(1), 4, true);
    direct_dist_write(&state, GICD_IROUTERnE, 0x1234, 8, true);
    g_assert_cmphex(direct_dist_read(&state, GICD_ISENABLERnE, 4, true), ==,
                    BIT(0));
    g_assert_cmphex(direct_dist_read(&state, GICD_ISPENDRnE, 4, true), ==,
                    BIT(0));
    g_assert_cmphex(direct_dist_read(&state, GICD_ISACTIVERnE, 4, true), ==,
                    BIT(0));
    g_assert_cmphex(direct_dist_read(&state, GICD_IPRIORITYRnE, 1, true), ==,
                    0x20);
    g_assert_cmphex(direct_dist_read(&state, GICD_ICFGRnE, 4, true), ==,
                    BIT(1));
    g_assert_cmphex(direct_dist_read(&state, GICD_IROUTERnE, 8, true), ==,
                    0x1234);

    /* When: Secure assigns Group1NS, the Non-secure view is writable. */
    direct_dist_write(&state, GICD_IGROUPRnE, BIT(0), 4, true);
    direct_dist_write(&state, GICD_IPRIORITYRnE, 0x40, 1, false);
    direct_dist_write(&state, GICD_IROUTERnE, 0x5678, 8, false);
    g_assert_cmphex(direct_dist_read(&state, GICD_IGROUPRnE, 4, true), ==,
                    BIT(0));
    g_assert_cmphex(direct_dist_read(&state, GICD_IPRIORITYRnE, 1, false), ==,
                    0x40);
    g_assert_cmphex(direct_dist_read(&state, GICD_IPRIORITYRnE, 1, true), ==,
                    0xa0);
    g_assert_cmphex(direct_dist_read(&state, GICD_IROUTERnE, 8, false), ==,
                    0x5678);

    /* Then: NSACR 0/1/2/3 gates Group0 pending, active, and route access. */
    direct_dist_write(&state, GICD_IGROUPRnE, 0, 4, true);
    direct_dist_write(&state, GICD_ICPENDRnE, BIT(0), 4, true);
    direct_dist_write(&state, GICD_ICACTIVERnE, BIT(0), 4, true);
    state.espi_nsacr[0] = 0;
    direct_dist_write(&state, GICD_ISPENDRnE, BIT(0), 4, false);
    g_assert_cmphex(direct_dist_read(&state, GICD_ISPENDRnE, 4, true), ==, 0);
    state.espi_nsacr[0] = 1;
    direct_dist_write(&state, GICD_ISPENDRnE, BIT(0), 4, false);
    direct_dist_write(&state, GICD_ICPENDRnE, BIT(0), 4, false);
    g_assert_cmphex(direct_dist_read(&state, GICD_ISPENDRnE, 4, true), ==,
                    BIT(0));
    state.espi_nsacr[0] = 2;
    direct_dist_write(&state, GICD_ICPENDRnE, BIT(0), 4, false);
    direct_dist_write(&state, GICD_ISACTIVERnE, BIT(0), 4, true);
    direct_dist_write(&state, GICD_ICACTIVERnE, BIT(0), 4, false);
    g_assert_cmphex(direct_dist_read(&state, GICD_ISPENDRnE, 4, true), ==, 0);
    g_assert_cmphex(direct_dist_read(&state, GICD_ISACTIVERnE, 4, false), ==,
                    BIT(0));
    g_assert_cmphex(direct_dist_read(&state, GICD_ISACTIVERnE, 4, true), ==,
                    BIT(0));
    state.espi_nsacr[0] = 3;
    direct_dist_write(&state, GICD_IROUTERnE, 0x9abc, 8, false);
    g_assert_cmphex(direct_dist_read(&state, GICD_IROUTERnE, 8, false), ==,
                    0x9abc);

    state.gicd_ctlr = GICD_CTLR_DS;
    direct_dist_write(&state, GICD_IGROUPRnE, BIT(0), 4, false);
    direct_dist_write(&state, GICD_ICACTIVERnE, BIT(0), 4, false);
    g_assert_cmphex(direct_dist_read(&state, GICD_IGROUPRnE, 4, false), ==,
                    BIT(0));
    g_assert_cmphex(direct_dist_read(&state, GICD_ISACTIVERnE, 4, false), ==,
                    0);
}

static void test_espi_post_load_route_cache(void)
{
    /* Given: restored ESPI routes and deliberately stale target pointers. */
    GICv3State state = {
        .num_cpu = 2,
        .num_irq = GIC_INTERNAL + 32,
        .num_espi = GICV3_MAX_ESPI,
        .cpu = g_new0(GICv3CPUState, 2),
    };

    state.cpu[0].gic = &state;
    state.cpu[1].gic = &state;
    state.cpu[0].hppi.prio = 0xff;
    state.cpu[1].hppi.prio = 0xff;
    state.cpu[1].gicr_typer = 1ULL << 32;
    state.espi_irouter[0] = 1;
    state.espi_irouter[512] = 0;
    state.espi_irouter[GICV3_MAX_ESPI - 1] = 1;
    state.espi_irouter_target[0] = &state.cpu[0];
    state.espi_irouter_target[512] = &state.cpu[1];
    state.espi_irouter_target[GICV3_MAX_ESPI - 1] = &state.cpu[0];

    /* When: the production software-GIC post-load callback is invoked. */
    arm_gicv3_post_load(&state);

    /* Then: all restored route targets are rebuilt from affinity values. */
    g_assert_true(state.espi_irouter_target[0] == &state.cpu[1]);
    g_assert_true(state.espi_irouter_target[512] == &state.cpu[0]);
    g_assert_true(state.espi_irouter_target[GICV3_MAX_ESPI - 1] ==
                  &state.cpu[1]);
    g_free(state.cpu);
}

static void test_espi_holes_and_range_limit(void)
{
    /* Given: disabled, minimum, and maximum ESPI distributors. */
    QTestState *disabled = gicv3_qtest_start(NULL);
    QTestState *maximum = gicv3_espi_qtest_start();
    QTestState *minimum = gicv3_qtest_start(
        "-global arm-gicv3.num-espi=32");
    const uint32_t maximum_holes[] = {
        0x1080, 0x1280, 0x1480, 0x1680, 0x1880, 0x1a80, 0x1c80,
        0x2400, 0x3100, 0xa000,
    };
    const uint32_t minimum_unimplemented[] = {
        0x1004, 0x1204, 0x1404, 0x1604, 0x1804, 0x1a04, 0x1c04,
        0x2020, 0x3008, 0x8100,
    };
    const uint32_t disabled_banks[] = {
        0x1000, 0x1200, 0x1400, 0x1600, 0x1800, 0x1a00, 0x1c00,
        0x2000, 0x3000, 0x8000,
    };

    /* When: ESPI5120 holes and unimplemented range addresses are written. */
    for (size_t i = 0; i < G_N_ELEMENTS(disabled_banks); i++) {
        qtest_writel(disabled, GICD_BASE + disabled_banks[i], UINT32_MAX);
        /* Then: disabled extended banks are RAZ/WI. */
        g_assert_cmphex(qtest_readl(disabled,
                                    GICD_BASE + disabled_banks[i]), ==, 0);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(maximum_holes); i++) {
        qtest_writel(maximum, GICD_BASE + maximum_holes[i], UINT32_MAX);
        /* Then: architecturally reserved addresses are RAZ/WI. */
        g_assert_cmphex(qtest_readl(maximum,
                                    GICD_BASE + maximum_holes[i]), ==, 0);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(minimum_unimplemented); i++) {
        qtest_writel(minimum, GICD_BASE + minimum_unimplemented[i],
                     UINT32_MAX);
        /* Then: count-limited addresses are also RAZ/WI. */
        g_assert_cmphex(qtest_readl(minimum,
                                    GICD_BASE + minimum_unimplemented[i]),
                        ==, 0);
    }
    qtest_quit(disabled);
    qtest_quit(maximum);
    qtest_quit(minimum);
}

static void test_espi_access_width_alignment_and_endian(void)
{
    /* Given: reset ESPI4096 registers on the little-endian distributor. */
    QTestState *qts = gicv3_espi_qtest_start();

    /* When: a priority word carries four independent byte values. */
    qtest_writel(qts, GICD_BASE + 0x2000, 0x10203040);

    /* Then: byte lanes are little-endian and round-trip exactly. */
    g_assert_cmphex(qtest_readb(qts, GICD_BASE + 0x2000), ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, GICD_BASE + 0x2001), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, GICD_BASE + 0x2002), ==, 0x20);
    g_assert_cmphex(qtest_readb(qts, GICD_BASE + 0x2003), ==, 0x10);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x2000), ==, 0x10203040);

    qtest_writel(qts, GICD_BASE + 0x8000, 0x89abcdef);
    qtest_writel(qts, GICD_BASE + 0x8004, 0x00000012);
    g_assert_cmphex(qtest_readq(qts, GICD_BASE + 0x8000), ==,
                    0x0000001289abcdefULL);

    /* When: unsupported sizes and unaligned accesses target other ESPI banks. */
    qtest_writeq(qts, GICD_BASE + 0x1200, UINT64_MAX);
    qtest_writel(qts, GICD_BASE + 0x1201, UINT32_MAX);
    qtest_writeb(qts, GICD_BASE + 0x8000, UINT8_MAX);
    qtest_writew(qts, GICD_BASE + 0x8002, UINT16_MAX);

    /* Then: malformed accesses are RAZ/WI and do not mutate aligned state. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1200), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1201), ==, 0);
    g_assert_cmphex(qtest_readq(qts, GICD_BASE + 0x8000), ==,
                    0x0000001289abcdefULL);
    qtest_quit(qts);
}

static void test_espi_nonsecure_group0_access(void)
{
    /* Given: security extensions are active and ESPI4096 remains Group0. */
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3,secure=on -m 64M -nodefaults "
        "-global arm-gicv3.num-espi=1024");

    /* When: the qtest Non-secure transaction attempts every Task 21 bank. */
    qtest_writel(qts, GICD_BASE + 0x1000, BIT(0));
    qtest_writel(qts, GICD_BASE + 0x1200, BIT(0));
    qtest_writel(qts, GICD_BASE + 0x1600, BIT(0));
    qtest_writel(qts, GICD_BASE + 0x1a00, BIT(0));
    qtest_writeb(qts, GICD_BASE + 0x2000, 0x40);
    qtest_writel(qts, GICD_BASE + 0x3000, BIT(1));
    qtest_writeq(qts, GICD_BASE + 0x8000, 1);

    /* Then: Secure Group0 state is RAZ/WI to the Non-secure requester. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1000), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1200), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1600), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x1a00), ==, 0);
    g_assert_cmphex(qtest_readb(qts, GICD_BASE + 0x2000), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + 0x3000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, GICD_BASE + 0x8000), ==, 0);
    qtest_quit(qts);
}

static void test_espi_invalid_property(void)
{
    /* Given: an ESPI count that is not an architected 32-interrupt block. */
    const char *qemu = qtest_qemu_binary(NULL);
    const char *argv[] = {
        qemu,
        "-machine", "virt,gic-version=3",
        "-global", "arm-gicv3.num-espi=33",
        "-display", "none",
        "-nodefaults",
        NULL,
    };
    g_autofree char *stderr_text = NULL;
    int wait_status;

    /* When: the production binary realizes the malformed property. */
    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL,
                               NULL, &stderr_text, &wait_status, NULL));

    /* Then: realization fails closed with the exact range diagnostic. */
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), !=, 0);
    g_assert_nonnull(strstr(stderr_text,
                            "num-espi must be zero or a multiple of 32"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv3-espi/pin/normal-distributor",
                   test_pin_normal_distributor);
    qtest_add_func("/arm-gicv3-espi/discovery",
                   test_espi_discovery);
    qtest_add_func("/arm-gicv3-espi/group-priority-config-route",
                   test_espi_group_priority_config_route);
    qtest_add_func("/arm-gicv3-espi/enable-pending-active",
                   test_espi_enable_pending_active);
    qtest_add_func("/arm-gicv3-espi/pending-line-semantics",
                   test_espi_pending_line_semantics);
    qtest_add_func("/arm-gicv3-espi/reset-nonalias",
                   test_espi_reset_and_normal_nonalias);
    qtest_add_func("/arm-gicv3-espi/opposite-normal-nonalias",
                   test_espi_opposite_normal_nonalias);
    qtest_add_func("/arm-gicv3-espi/explicit-security-attrs",
                   test_espi_explicit_security_attrs);
    qtest_add_func("/arm-gicv3-espi/post-load-route-cache",
                   test_espi_post_load_route_cache);
    qtest_add_func("/arm-gicv3-espi/holes-range-limit",
                   test_espi_holes_and_range_limit);
    qtest_add_func("/arm-gicv3-espi/access-width-alignment-endian",
                   test_espi_access_width_alignment_and_endian);
    qtest_add_func("/arm-gicv3-espi/nonsecure-group0-access",
                   test_espi_nonsecure_group0_access);
    qtest_add_func("/arm-gicv3-espi/invalid-property",
                   test_espi_invalid_property);
    return g_test_run();
}
