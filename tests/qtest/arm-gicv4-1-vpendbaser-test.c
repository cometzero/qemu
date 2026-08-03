/*
 * QTest coverage for GICv4.1 GICR_VPENDBASER state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define GICR_BASE                   0x080a0000
#define GICR_VLPI_BASE              (GICR_BASE + 0x20000)
#define GICR_TYPER                  0x0008
#define GICR_VPROPBASER             0x0070
#define GICR_VPENDBASER             0x0078
#define GITS_BASE                   0x08080000
#define GITS_CTLR                   0x0000
#define GITS_BASER                  0x0100
#define GITS_TRANSLATER             0x10040

#define DEVICE_TABLE_ADDR           0x40100000
#define VPE_TABLE_ADDR              0x40200000
#define ITT_ADDR                    0x40300000
#define VPT_ADDR                    0x40400000
#define VPROP_ADDR                  0x40500000
#define TEST_VPEID                  0x42
#define TEST_VINTID                 8192

#define GICR_TYPER_DIRTY            BIT(2)
#define GICR_TYPER_RVPEID           BIT(7)
#define VPENDBASER_VPEID_MASK       0xffffULL
#define VPENDBASER_DIRTY             BIT_ULL(60)
#define VPENDBASER_PENDING_LAST      BIT_ULL(61)
#define VPENDBASER_VALID             BIT_ULL(63)
#define GITS_BASER_VALID             BIT_ULL(63)

static QTestState *vpendbaser_qtest_start_full(unsigned int vpeid_bits,
                                               const char *extra_args)
{
    g_autofree char *args = g_strdup_printf(
        "-machine virt,gic-version=4,virtualization=on,its=on "
        "-global arm-gicv3.has-gicv4-1=on "
        "-global arm-gicv3.has-rvpeid=on "
        "-global arm-gicv3.has-vpend-valid-dirty=on "
        "-global arm-gicv3.vpeid-bits=%u "
        "-m 64M -nodefaults %s", vpeid_bits,
        extra_args ? extra_args : "");

    return qtest_init(args);
}

static QTestState *vpendbaser_qtest_start(unsigned int vpeid_bits)
{
    return vpendbaser_qtest_start_full(vpeid_bits, NULL);
}

static uint64_t read_vpendbaser(QTestState *qts)
{
    return qtest_readq(qts, GICR_VLPI_BASE + GICR_VPENDBASER);
}

static void write_vpendbaser(QTestState *qts, uint64_t value)
{
    qtest_writeq(qts, GICR_VLPI_BASE + GICR_VPENDBASER, value);
}

static uint64_t vte_value(unsigned int vptsize)
{
    return BIT_ULL(0) | ((uint64_t)vptsize << 1) |
        ((VPT_ADDR >> 16) << 6);
}

static void configure_virtual_its(QTestState *qts, unsigned int vptsize)
{
    uint64_t baser;
    uint64_t dte = BIT_ULL(0) | ((ITT_ADDR >> 8) << 6);
    uint64_t ite = BIT_ULL(0) | ((uint64_t)TEST_VINTID << 2) |
        ((uint64_t)TEST_VPEID << 48);

    baser = qtest_readq(qts, GITS_BASE + GITS_BASER);
    qtest_writeq(qts, GITS_BASE + GITS_BASER,
                 baser | DEVICE_TABLE_ADDR | GITS_BASER_VALID);
    baser = qtest_readq(qts, GITS_BASE + GITS_BASER + 16);
    qtest_writeq(qts, GITS_BASE + GITS_BASER + 16,
                 baser | VPE_TABLE_ADDR | GITS_BASER_VALID);

    qtest_writeq(qts, DEVICE_TABLE_ADDR, dte);
    qtest_writeq(qts, ITT_ADDR, ite);
    qtest_writel(qts, ITT_ADDR + 8, 1023);
    qtest_writeq(qts, VPE_TABLE_ADDR + TEST_VPEID * 8,
                 vte_value(vptsize));
    qtest_writeb(qts, VPROP_ADDR, 0x21);
    qtest_writeq(qts, GICR_VLPI_BASE + GICR_VPROPBASER,
                 VPROP_ADDR | 13);
    qtest_writel(qts, GITS_BASE + GITS_CTLR, 1);
}

static void trigger_virtual_lpi(QTestState *qts)
{
    qtest_writel(qts, GITS_BASE + GITS_TRANSLATER, 0);
}

static void test_valid_and_rvpeid(void)
{
    QTestState *qts = vpendbaser_qtest_start(16);
    uint64_t value = VPENDBASER_VALID | VPENDBASER_PENDING_LAST | 0x1234;
    uint64_t typer = qtest_readq(qts, GICR_BASE + GICR_TYPER);

    g_assert_cmphex(typer & (GICR_TYPER_DIRTY | GICR_TYPER_RVPEID), ==,
                    GICR_TYPER_DIRTY | GICR_TYPER_RVPEID);

    write_vpendbaser(qts, value);
    g_assert_cmphex(read_vpendbaser(qts) &
                    (VPENDBASER_VALID | VPENDBASER_VPEID_MASK), ==,
                    VPENDBASER_VALID | 0x1234);

    write_vpendbaser(qts, VPENDBASER_PENDING_LAST | 0x1234);
    g_assert_cmphex(read_vpendbaser(qts) &
                    (VPENDBASER_VALID | VPENDBASER_DIRTY |
                     VPENDBASER_VPEID_MASK), ==, 0x1234);
    qtest_quit(qts);
}

static void test_invalid_vpeid_preserves_state(void)
{
    QTestState *qts = vpendbaser_qtest_start(8);
    uint64_t before;

    write_vpendbaser(qts, 0x5a);
    before = read_vpendbaser(qts);
    write_vpendbaser(qts, VPENDBASER_VALID | 0x100);

    g_assert_cmphex(read_vpendbaser(qts), ==, before);
    qtest_quit(qts);
}

static void test_reset_clears_state(void)
{
    QTestState *qts = vpendbaser_qtest_start(16);

    write_vpendbaser(qts, VPENDBASER_VALID | 0x42);
    qtest_system_reset(qts);

    g_assert_cmphex(read_vpendbaser(qts), ==, 0);
    qtest_quit(qts);
}

static void test_dirty_and_rvpeid_routing(void)
{
    QTestState *qts = vpendbaser_qtest_start(16);
    uint64_t pending_addr = VPT_ADDR + TEST_VINTID / 8;

    configure_virtual_its(qts, 13);
    write_vpendbaser(qts, VPENDBASER_VALID | TEST_VPEID);
    trigger_virtual_lpi(qts);

    g_assert_cmphex(qtest_readb(qts, pending_addr) & BIT(0), ==, BIT(0));
    g_assert_cmphex(read_vpendbaser(qts) & VPENDBASER_DIRTY, ==,
                    VPENDBASER_DIRTY);

    write_vpendbaser(qts, TEST_VPEID);
    g_assert_cmphex(read_vpendbaser(qts) & VPENDBASER_DIRTY, ==, 0);

    qtest_writeb(qts, pending_addr, 0);
    write_vpendbaser(qts, VPENDBASER_VALID | (TEST_VPEID + 1));
    trigger_virtual_lpi(qts);
    g_assert_cmphex(qtest_readb(qts, pending_addr) & BIT(0), ==, BIT(0));
    g_assert_cmphex(read_vpendbaser(qts) & VPENDBASER_DIRTY, ==, 0);
    qtest_quit(qts);
}

static void test_invalid_vpt_size_preserves_state(void)
{
    QTestState *qts = vpendbaser_qtest_start(16);
    uint64_t pending_addr = VPT_ADDR + TEST_VINTID / 8;
    uint64_t before;

    configure_virtual_its(qts, 12);
    write_vpendbaser(qts, VPENDBASER_VALID | TEST_VPEID);
    before = read_vpendbaser(qts);
    trigger_virtual_lpi(qts);

    g_assert_cmphex(qtest_readb(qts, pending_addr), ==, 0);
    g_assert_cmphex(read_vpendbaser(qts), ==, before);
    qtest_quit(qts);
}

static void test_invalid_legacy_alignment_preserves_state(void)
{
    QTestState *qts = vpendbaser_qtest_start_full(
        16, "-global arm-gicv3.has-rvpeid=off");

    write_vpendbaser(qts, VPENDBASER_VALID | BIT_ULL(12));
    g_assert_cmphex(read_vpendbaser(qts), ==, 0);
    qtest_quit(qts);
}

static void test_migration_preserves_state(void)
{
    g_autofree char *tmpdir = g_dir_make_tmp("vpendbaser-migration-XXXXXX",
                                              NULL);
    g_autofree char *socket = g_build_filename(tmpdir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    QTestState *src = vpendbaser_qtest_start(16);
    QTestState *dst;

    configure_virtual_its(src, 13);
    write_vpendbaser(src, VPENDBASER_VALID | TEST_VPEID);
    trigger_virtual_lpi(src);
    g_assert_cmphex(read_vpendbaser(src) & VPENDBASER_DIRTY, ==,
                    VPENDBASER_DIRTY);

    dst = vpendbaser_qtest_start_full(16, incoming);
    qtest_qmp_assert_success(src,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(src, "STOP");
    qtest_qmp_eventwait(dst, "RESUME");

    g_assert_cmphex(read_vpendbaser(dst) &
                    (VPENDBASER_VALID | VPENDBASER_DIRTY |
                     VPENDBASER_VPEID_MASK), ==,
                    VPENDBASER_VALID | VPENDBASER_DIRTY | TEST_VPEID);
    g_assert_cmphex(qtest_readb(dst, VPT_ADDR + TEST_VINTID / 8) & BIT(0),
                    ==, BIT(0));

    write_vpendbaser(dst, TEST_VPEID);
    g_assert_cmphex(read_vpendbaser(dst) & VPENDBASER_DIRTY, ==, 0);
    qtest_quit(src);
    qtest_quit(dst);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv4-1-vpendbaser/valid-rvpeid",
                   test_valid_and_rvpeid);
    qtest_add_func("/arm-gicv4-1-vpendbaser/invalid-vpeid",
                   test_invalid_vpeid_preserves_state);
    qtest_add_func("/arm-gicv4-1-vpendbaser/reset",
                   test_reset_clears_state);
    qtest_add_func("/arm-gicv4-1-vpendbaser/dirty-routing",
                   test_dirty_and_rvpeid_routing);
    qtest_add_func("/arm-gicv4-1-vpendbaser/invalid-vpt-size",
                   test_invalid_vpt_size_preserves_state);
    qtest_add_func("/arm-gicv4-1-vpendbaser/invalid-legacy-alignment",
                   test_invalid_legacy_alignment_preserves_state);
    qtest_add_func("/arm-gicv4-1-vpendbaser/migration",
                   test_migration_preserves_state);
    return g_test_run();
}
