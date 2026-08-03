/*
 * QTest baseline coverage for the Arm GICv3 distributor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define GICD_BASE              0x08000000
#define GICD_IIDR              0x0008
#define GICD_ISENABLER1        0x0104
#define GICD_ISPENDR1          0x0204
#define GICD_ICPENDR1          0x0284
#define GICV3_QEMU_IIDR        0x0000043b
#define TEST_SPI_MASK          BIT(0)

static QTestState *gicv3_qtest_start(void)
{
    return qtest_init("-machine virt,gic-version=3 -nodefaults");
}

static void test_iidr(void)
{
    /* Given: QEMU's local Arm virt machine with a GICv3 distributor. */
    QTestState *qts = gicv3_qtest_start();

    /* When: the distributor implementer register is read. */
    uint32_t iidr = qtest_readl(qts, GICD_BASE + GICD_IIDR);

    /* Then: it reports QEMU's documented Arm r0p0 identity. */
    g_assert_cmphex(iidr, ==, GICV3_QEMU_IIDR);
    qtest_quit(qts);
}

static void test_irq_pending(void)
{
    /* Given: an enabled distributor SPI in the local GICv3 model. */
    QTestState *qts = gicv3_qtest_start();

    qtest_writel(qts, GICD_BASE + GICD_ISENABLER1, TEST_SPI_MASK);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISENABLER1) &
                    TEST_SPI_MASK, ==, TEST_SPI_MASK);

    /* When: software sets and then clears its pending state. */
    qtest_writel(qts, GICD_BASE + GICD_ISPENDR1, TEST_SPI_MASK);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISPENDR1) &
                    TEST_SPI_MASK, ==, TEST_SPI_MASK);
    qtest_writel(qts, GICD_BASE + GICD_ICPENDR1, TEST_SPI_MASK);

    /* Then: the pending state is deasserted. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISPENDR1) &
                    TEST_SPI_MASK, ==, 0);
    qtest_quit(qts);
}

static void test_reset_clears_irq_state(void)
{
    /* Given: an enabled and pending distributor SPI. */
    QTestState *qts = gicv3_qtest_start();

    qtest_writel(qts, GICD_BASE + GICD_ISENABLER1, TEST_SPI_MASK);
    qtest_writel(qts, GICD_BASE + GICD_ISPENDR1, TEST_SPI_MASK);

    /* When: the machine completes a system reset. */
    qtest_system_reset(qts);

    /* Then: enable and pending state return to reset values. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISENABLER1) &
                    TEST_SPI_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISPENDR1) &
                    TEST_SPI_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_IIDR),
                    ==, GICV3_QEMU_IIDR);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv3-baseline/iidr", test_iidr);
    qtest_add_func("/arm-gicv3-baseline/irq-pending", test_irq_pending);
    qtest_add_func("/arm-gicv3-baseline/reset", test_reset_clears_irq_state);
    return g_test_run();
}
