/*
 * QTest coverage for the GICv4.1 physical DirectLPI interface.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define GICR_BASE                   0x080a0000
#define GICR_STRIDE                 0x00040000
#define GICR_CTLR                   0x0000
#define GICR_TYPER                  0x0008
#define GICR_SETLPIR                0x0040
#define GICR_CLRLPIR                0x0048
#define GICR_PROPBASER              0x0070
#define GICR_PENDBASER              0x0078

#define PROP_TABLE_ADDR             0x40100000
#define PEND_TABLE_0_ADDR           0x40200000
#define PEND_TABLE_1_ADDR           0x40300000
#define TEST_LPI                    8192
#define INVALID_LOW_INTID           8191
#define INVALID_HIGH_INTID          16384
#define LPI_TABLE_IDBITS            13
#define PENDING_TABLE_SIZE          ((1U << (LPI_TABLE_IDBITS + 1)) / 8)

#define GICR_CTLR_ENABLE_LPIS       BIT(0)
#define GICR_TYPER_DIRECTLPI        BIT(3)
#define GICR_TYPER_PROCNUM_MASK     (0xffffULL << 8)
#define GICR_TYPER_PROCNUM_SHIFT    8

static uint64_t gicr_base(unsigned int cpu)
{
    return GICR_BASE + cpu * GICR_STRIDE;
}

static uint64_t pending_table(unsigned int cpu)
{
    return cpu ? PEND_TABLE_1_ADDR : PEND_TABLE_0_ADDR;
}

static QTestState *direct_lpi_qtest_start(bool direct_lpi)
{
    g_autofree char *args = g_strdup_printf(
        "-machine virt,gic-version=4,virtualization=on,its=on "
        "-global arm-gicv3.has-gicv4-1=on "
        "-global arm-gicv3.has-direct-lpi=%s "
        "-smp 2 -m 64M -nodefaults",
        direct_lpi ? "on" : "off");

    return qtest_init(args);
}

static void configure_physical_lpis(QTestState *qts)
{
    qtest_writeb(qts, PROP_TABLE_ADDR + TEST_LPI, 0x21);

    for (unsigned int cpu = 0; cpu < 2; cpu++) {
        qtest_writeq(qts, gicr_base(cpu) + GICR_PROPBASER,
                     PROP_TABLE_ADDR | LPI_TABLE_IDBITS);
        qtest_writeq(qts, gicr_base(cpu) + GICR_PENDBASER,
                     pending_table(cpu));
        qtest_writel(qts, gicr_base(cpu) + GICR_CTLR,
                     GICR_CTLR_ENABLE_LPIS);
    }
}

static uint8_t pending_byte(QTestState *qts, unsigned int cpu, int intid)
{
    return qtest_readb(qts, pending_table(cpu) + intid / 8);
}

static void test_set_targets_physical_pe(void)
{
    /* Given: two PEs with separate physical LPI pending tables. */
    QTestState *qts = direct_lpi_qtest_start(true);
    uint64_t typer = qtest_readq(qts, gicr_base(1) + GICR_TYPER);

    configure_physical_lpis(qts);
    g_assert_cmpuint((typer & GICR_TYPER_PROCNUM_MASK) >>
                     GICR_TYPER_PROCNUM_SHIFT, ==, 1);

    /* When: the LPI is injected through PE1's Redistributor. */
    qtest_writeq(qts, gicr_base(1) + GICR_SETLPIR, TEST_LPI);

    /* Then: only PE1's physical pending table records delivery. */
    g_assert_cmphex(pending_byte(qts, 0, TEST_LPI) & BIT(0), ==, 0);
    g_assert_cmphex(pending_byte(qts, 1, TEST_LPI) & BIT(0), ==, BIT(0));
    qtest_quit(qts);
}

static void test_clear_removes_delivery(void)
{
    /* Given: a physical LPI delivered to PE1 through DirectLPI. */
    QTestState *qts = direct_lpi_qtest_start(true);

    configure_physical_lpis(qts);
    qtest_writel(qts, gicr_base(1) + GICR_SETLPIR, TEST_LPI);
    g_assert_cmphex(pending_byte(qts, 1, TEST_LPI) & BIT(0), ==, BIT(0));

    /* When: the same Redistributor receives CLRLPIR. */
    qtest_writel(qts, gicr_base(1) + GICR_CLRLPIR, TEST_LPI);

    /* Then: the physical pending state is removed. */
    g_assert_cmphex(pending_byte(qts, 1, TEST_LPI) & BIT(0), ==, 0);
    qtest_quit(qts);
}

static void test_invalid_redistributor_has_no_effect(void)
{
    /* Given: two configured Redistributors and clean pending tables. */
    QTestState *qts = direct_lpi_qtest_start(true);

    configure_physical_lpis(qts);

    /* When: SETLPIR is written beyond the implemented Redistributors. */
    qtest_writeq(qts, gicr_base(2) + GICR_SETLPIR, TEST_LPI);

    /* Then: neither physical PE receives the LPI. */
    g_assert_cmphex(pending_byte(qts, 0, TEST_LPI) & BIT(0), ==, 0);
    g_assert_cmphex(pending_byte(qts, 1, TEST_LPI) & BIT(0), ==, 0);
    qtest_quit(qts);
}

static void test_invalid_intids_have_no_effect(void)
{
    /* Given: a configured PE and a snapshot of both pending tables. */
    QTestState *qts = direct_lpi_qtest_start(true);
    g_autofree uint8_t *before0 = g_malloc(PENDING_TABLE_SIZE);
    g_autofree uint8_t *before1 = g_malloc(PENDING_TABLE_SIZE);
    g_autofree uint8_t *after0 = g_malloc(PENDING_TABLE_SIZE);
    g_autofree uint8_t *after1 = g_malloc(PENDING_TABLE_SIZE);

    configure_physical_lpis(qts);
    qtest_memread(qts, PEND_TABLE_0_ADDR, before0, PENDING_TABLE_SIZE);
    qtest_memread(qts, PEND_TABLE_1_ADDR, before1, PENDING_TABLE_SIZE);

    /* When: non-LPI and out-of-range physical INTIDs are requested. */
    qtest_writeq(qts, gicr_base(1) + GICR_SETLPIR, INVALID_LOW_INTID);
    qtest_writeq(qts, gicr_base(1) + GICR_SETLPIR, INVALID_HIGH_INTID);
    qtest_writeq(qts, gicr_base(1) + GICR_CLRLPIR, INVALID_LOW_INTID);
    qtest_writeq(qts, gicr_base(1) + GICR_CLRLPIR, INVALID_HIGH_INTID);

    /* Then: no physical pending-table byte is mutated. */
    qtest_memread(qts, PEND_TABLE_0_ADDR, after0, PENDING_TABLE_SIZE);
    qtest_memread(qts, PEND_TABLE_1_ADDR, after1, PENDING_TABLE_SIZE);
    g_assert_cmpmem(after0, PENDING_TABLE_SIZE,
                    before0, PENDING_TABLE_SIZE);
    g_assert_cmpmem(after1, PENDING_TABLE_SIZE,
                    before1, PENDING_TABLE_SIZE);
    qtest_quit(qts);
}

static void test_feature_bit_and_behavior_are_coupled(void)
{
    /* Given: DirectLPI is disabled through the production property. */
    QTestState *qts = direct_lpi_qtest_start(false);
    uint64_t typer = qtest_readq(qts, gicr_base(0) + GICR_TYPER);

    configure_physical_lpis(qts);
    g_assert_cmphex(typer & GICR_TYPER_DIRECTLPI, ==, 0);

    /* When: software writes the architecturally absent SETLPIR. */
    qtest_writeq(qts, gicr_base(0) + GICR_SETLPIR, TEST_LPI);

    /* Then: reporting stays off and no physical delivery occurs. */
    g_assert_cmphex(pending_byte(qts, 0, TEST_LPI) & BIT(0), ==, 0);
    qtest_quit(qts);

    /*
     * Given: DirectLPI is enabled through that same property.
     * When: the bit and SETLPIR behavior are audited together.
     * Then: both are present, so a reporting-only implementation fails.
     */
    qts = direct_lpi_qtest_start(true);
    configure_physical_lpis(qts);
    typer = qtest_readq(qts, gicr_base(0) + GICR_TYPER);
    g_assert_cmphex(typer & GICR_TYPER_DIRECTLPI, ==,
                    GICR_TYPER_DIRECTLPI);
    qtest_writeq(qts, gicr_base(0) + GICR_SETLPIR, TEST_LPI);
    g_assert_cmphex(pending_byte(qts, 0, TEST_LPI) & BIT(0), ==, BIT(0));
    qtest_quit(qts);
}

static void test_subfeature_without_gicv4_1_fails_realize(void)
{
    /* Given: a command line requesting DirectLPI without GICv4.1. */
    const char *qemu = qtest_qemu_binary(NULL);
    const char *argv[] = {
        qemu,
        "-machine", "virt,gic-version=4,virtualization=on,its=on",
        "-global", "arm-gicv3.has-direct-lpi=on",
        "-display", "none",
        "-nodefaults",
        NULL,
    };
    g_autofree char *stderr_text = NULL;
    int wait_status;
    bool spawned;

    /* When: the production QEMU binary realizes the machine. */
    spawned = g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL, NULL,
                           &stderr_text, &wait_status, NULL);

    /* Then: realization rejects uncoupled subfeature enablement. */
    g_assert_true(spawned);
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), !=, 0);
    g_assert_nonnull(strstr(stderr_text,
                           "sub-features require has-gicv4-1"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv4-1-direct-lpi/set-physical-pe",
                   test_set_targets_physical_pe);
    qtest_add_func("/arm-gicv4-1-direct-lpi/clear",
                   test_clear_removes_delivery);
    qtest_add_func("/arm-gicv4-1-direct-lpi/invalid-redistributor",
                   test_invalid_redistributor_has_no_effect);
    qtest_add_func("/arm-gicv4-1-direct-lpi/invalid-intids",
                   test_invalid_intids_have_no_effect);
    qtest_add_func("/arm-gicv4-1-direct-lpi/feature-coupling",
                   test_feature_bit_and_behavior_are_coupled);
    qtest_add_func("/arm-gicv4-1-direct-lpi/realize-coupling",
                   test_subfeature_without_gicv4_1_fails_realize);
    return g_test_run();
}
