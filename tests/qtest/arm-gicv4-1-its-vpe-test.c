/*
 * QTest coverage for GICv4.1 ITS vPE commands.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define GITS_BASE                  0x08080000
#define GITS_CTLR                  0x0000
#define GITS_TYPER                 0x0008
#define GITS_CBASER                0x0080
#define GITS_CWRITER               0x0088
#define GITS_CREADR                0x0090
#define GITS_BASER                 0x0100
#define GITS_TRANSLATER            0x10040
#define GICR_BASE(cpu)             (0x080a0000 + (cpu) * 0x40000)
#define GICR_VLPI_BASE(cpu)        (GICR_BASE(cpu) + 0x20000)
#define GICR_VPROPBASER            0x0070
#define GICR_VPENDBASER            0x0078

#define CMDQ_ADDR                  0x40010000
#define DEVICE_TABLE_ADDR          0x40100000
#define COLLECTION_TABLE_ADDR      0x40200000
#define VPE_TABLE_ADDR             0x40300000
#define ITT_ADDR                   0x40400000
#define VPT_ADDR                   0x40500000
#define VCONF_ADDR                 0x40600000
#define GUEST_RAM_END              0x44000000
#define TEST_VPEID                 0x42
#define TEST_VINTID                8192
#define TEST_DEFAULT_DB            8193

#define GITS_BASER_VALID           BIT_ULL(63)
#define GITS_TYPER_VMAPP           BIT_ULL(40)
#define GITS_TYPER_SVPET_SHIFT     41
#define VPENDBASER_DIRTY           BIT_ULL(60)
#define VPENDBASER_VALID           BIT_ULL(63)

static QTestState *start_qtest(bool its_gicv4_1, const char *extra)
{
    g_autofree char *args = g_strdup_printf(
        "-machine virt,gic-version=4,virtualization=on,its=on "
        "-global arm-gicv3.has-gicv4-1=on "
        "-global arm-gicv3.has-rvpeid=on "
        "-global arm-gicv3.has-vpend-valid-dirty=on "
        "-global arm-gicv3.vpeid-bits=16 "
        "%s -smp 2 -m 64M -nodefaults %s",
        its_gicv4_1 ?
        "-global arm-gicv3-its.has-gicv4-1=on "
        "-global arm-gicv3-its.gicv4-1-svpet=1 "
        "-global arm-gicv3-its.gicv4-1-cte-size=2" : "",
        extra ? extra : "");

    return qtest_init(args);
}

static void set_baser(QTestState *qts, unsigned int index, uint64_t addr,
                      bool valid)
{
    uint64_t value = qtest_readq(qts, GITS_BASE + GITS_BASER + index * 8);

    value = (value & ~0x0000fffffffff000ULL) | addr;
    if (valid) {
        value |= GITS_BASER_VALID;
    } else {
        value &= ~GITS_BASER_VALID;
    }
    qtest_writeq(qts, GITS_BASE + GITS_BASER + index * 8, value);
}

static void configure_its(QTestState *qts, bool valid_vpe_table)
{
    set_baser(qts, 0, DEVICE_TABLE_ADDR, true);
    set_baser(qts, 1, COLLECTION_TABLE_ADDR, true);
    set_baser(qts, 2, VPE_TABLE_ADDR, valid_vpe_table);
    qtest_writeq(qts, GITS_BASE + GITS_CBASER,
                 CMDQ_ADDR | GITS_BASER_VALID);
    qtest_writel(qts, GITS_BASE + GITS_CTLR, 1);
}

static void submit(QTestState *qts, unsigned int slot, uint64_t word0,
                   uint64_t word1, uint64_t word2, uint64_t word3)
{
    uint64_t addr = CMDQ_ADDR + slot * 32;

    qtest_writeq(qts, addr, word0);
    qtest_writeq(qts, addr + 8, word1);
    qtest_writeq(qts, addr + 16, word2);
    qtest_writeq(qts, addr + 24, word3);
    qtest_writeq(qts, GITS_BASE + GITS_CWRITER, (slot + 1) * 32);
    g_assert_cmphex(qtest_readq(qts, GITS_BASE + GITS_CREADR), ==,
                    (slot + 1) * 32);
}

static void vmapp(QTestState *qts, unsigned int slot, uint32_t rdbase)
{
    submit(qts, slot, 0x29 | BIT_ULL(8) | BIT_ULL(9) | VCONF_ADDR,
           ((uint64_t)TEST_VPEID << 32) | TEST_DEFAULT_DB,
           BIT_ULL(63) | ((uint64_t)rdbase << 16),
           VPT_ADDR | 13);
}

static void map_device_and_vlpi(QTestState *qts, unsigned int slot)
{
    submit(qts, slot, 0x08, 0, ITT_ADDR | BIT_ULL(63), 0);
    submit(qts, slot + 1, 0x2a, (uint64_t)TEST_VPEID << 32,
           ((uint64_t)1023 << 32) | TEST_VINTID, 0);
}

static void vmovp(QTestState *qts, unsigned int slot, uint32_t rdbase)
{
    submit(qts, slot, 0x22, (uint64_t)TEST_VPEID << 32,
           BIT_ULL(63) | ((uint64_t)rdbase << 16), TEST_DEFAULT_DB + 1);
}

static uint64_t vpe_addr(void)
{
    return VPE_TABLE_ADDR + TEST_VPEID * 32;
}

static uint32_t vpe_table_entries(QTestState *qts)
{
    uint64_t baser = qtest_readq(qts, GITS_BASE + GITS_BASER + 16);
    uint32_t entry_size = ((baser >> 48) & 0x1f) + 1;
    uint32_t page_size;

    switch ((baser >> 8) & 3) {
    case 0:
        page_size = 4096;
        break;
    case 1:
        page_size = 16384;
        break;
    case 2:
        page_size = 65536;
        break;
    default:
        g_assert_not_reached();
    }
    return ((baser & 0xff) + 1) * page_size / entry_size;
}

static void assert_vte(QTestState *qts, uint32_t rdbase, uint32_t doorbell)
{
    uint64_t flags = BIT_ULL(0) | BIT_ULL(1) | BIT_ULL(2) |
                     ((uint64_t)13 << 8) | ((uint64_t)rdbase << 16);

    g_assert_cmphex(qtest_readq(qts, vpe_addr()), ==, flags);
    g_assert_cmphex(qtest_readq(qts, vpe_addr() + 8), ==, VPT_ADDR);
    g_assert_cmphex(qtest_readq(qts, vpe_addr() + 16), ==, VCONF_ADDR);
    g_assert_cmphex(qtest_readq(qts, vpe_addr() + 24), ==, doorbell);
}

static uint64_t vpendbaser(QTestState *qts, unsigned int cpu)
{
    return qtest_readq(qts, GICR_VLPI_BASE(cpu) + GICR_VPENDBASER);
}

static void configure_residency(QTestState *qts)
{
    unsigned int cpu;

    qtest_writeb(qts, VCONF_ADDR, 0x21);
    for (cpu = 0; cpu < 2; cpu++) {
        qtest_writeq(qts, GICR_VLPI_BASE(cpu) + GICR_VPROPBASER,
                     VCONF_ADDR | 13);
        qtest_writeq(qts, GICR_VLPI_BASE(cpu) + GICR_VPENDBASER,
                     VPENDBASER_VALID | TEST_VPEID);
    }
}

static void test_typer_and_entry_sizes(void)
{
    QTestState *qts = start_qtest(true, NULL);
    uint64_t typer = qtest_readq(qts, GITS_BASE + GITS_TYPER);

    g_assert_cmphex(typer & GITS_TYPER_VMAPP, ==, GITS_TYPER_VMAPP);
    g_assert_cmpuint((typer >> GITS_TYPER_SVPET_SHIFT) & 3, ==, 1);
    g_assert_cmpuint(((qtest_readq(qts, GITS_BASE + GITS_BASER + 8) >>
                       48) & 0x1f) + 1, ==, 2);
    g_assert_cmpuint(((qtest_readq(qts, GITS_BASE + GITS_BASER + 16) >>
                       48) & 0x1f) + 1, ==, 32);
    configure_its(qts, true);
    submit(qts, 0, 0x09, 0, BIT_ULL(63) | BIT_ULL(16), 0);
    g_assert_cmphex(qtest_readw(qts, COLLECTION_TABLE_ADDR), ==, 3);
    qtest_quit(qts);
}

static void test_vmapp_vmapti_vmovp(void)
{
    QTestState *qts = start_qtest(true, NULL);

    configure_its(qts, true);
    vmapp(qts, 0, 0);
    map_device_and_vlpi(qts, 1);
    assert_vte(qts, 0, TEST_DEFAULT_DB);
    configure_residency(qts);

    qtest_writel(qts, GITS_BASE + GITS_TRANSLATER, 0);
    g_assert_cmphex(vpendbaser(qts, 0) & VPENDBASER_DIRTY, ==,
                    VPENDBASER_DIRTY);
    g_assert_cmphex(vpendbaser(qts, 1) & VPENDBASER_DIRTY, ==, 0);

    qtest_writeb(qts, VPT_ADDR + TEST_VINTID / 8, 0);
    qtest_writeq(qts, GICR_VLPI_BASE(0) + GICR_VPENDBASER, TEST_VPEID);
    qtest_writeq(qts, GICR_VLPI_BASE(0) + GICR_VPENDBASER,
                 VPENDBASER_VALID | TEST_VPEID);
    vmovp(qts, 3, 1);
    assert_vte(qts, 1, TEST_DEFAULT_DB + 1);
    qtest_writel(qts, GITS_BASE + GITS_TRANSLATER, 0);
    g_assert_cmphex(vpendbaser(qts, 0) & VPENDBASER_DIRTY, ==, 0);
    g_assert_cmphex(vpendbaser(qts, 1) & VPENDBASER_DIRTY, ==,
                    VPENDBASER_DIRTY);
    qtest_quit(qts);
}

static void test_invalid_table_and_id(void)
{
    QTestState *qts = start_qtest(true, NULL);
    const uint64_t invalid_vmapp_canary[4] = {
        0xdecafbad00000000, 0x0123456789abcdef,
        0xfedcba9876543210, 0x55aa55aa55aa55aa,
    };
    const uint64_t invalid_vte_canary[4] = {
        BIT_ULL(0) | BIT_ULL(1) | ((uint64_t)13 << 8),
        VPT_ADDR, VCONF_ADDR, TEST_DEFAULT_DB,
    };
    const uint64_t invalid_ite_canary = 0x5a5aa5a55a5aa5a5;
    const uint32_t invalid_ite_hi_canary = 0xa55a5aa5;
    uint64_t valid_vte[4];
    uint64_t valid_ite;
    uint64_t invalid_addr;
    uint64_t invalid_ite_addr = ITT_ADDR + 12;
    uint64_t dirty0, dirty1;
    uint32_t invalid_vpeid;
    uint32_t i;
    uint32_t valid_ite_hi;
    uint8_t pending;

    configure_its(qts, false);
    vmapp(qts, 0, 0);
    g_assert_cmphex(qtest_readq(qts, vpe_addr()), ==, 0);
    qtest_quit(qts);

    qts = start_qtest(true, NULL);
    configure_its(qts, true);
    invalid_vpeid = vpe_table_entries(qts);
    invalid_addr = VPE_TABLE_ADDR + (uint64_t)invalid_vpeid * 32;
    g_assert_cmpuint(invalid_vpeid, <=, 0xffff);
    g_assert_cmphex(invalid_addr + 32, <=, GUEST_RAM_END);

    vmapp(qts, 0, 0);
    map_device_and_vlpi(qts, 1);
    configure_residency(qts);
    qtest_writel(qts, GITS_BASE + GITS_TRANSLATER, 0);
    for (i = 0; i < G_N_ELEMENTS(valid_vte); i++) {
        valid_vte[i] = qtest_readq(qts, vpe_addr() + i * 8);
        qtest_writeq(qts, invalid_addr + i * 8, invalid_vmapp_canary[i]);
    }
    valid_ite = qtest_readq(qts, ITT_ADDR);
    valid_ite_hi = qtest_readl(qts, ITT_ADDR + 8);
    pending = qtest_readb(qts, VPT_ADDR + TEST_VINTID / 8);
    dirty0 = vpendbaser(qts, 0);
    dirty1 = vpendbaser(qts, 1);
    g_assert_cmphex(valid_vte[0], !=, 0);
    g_assert_cmphex(valid_ite, !=, 0);
    g_assert_cmphex(valid_ite_hi, !=, 0);
    g_assert_cmphex(pending, !=, 0);
    g_assert_cmphex(dirty0 & VPENDBASER_DIRTY, ==, VPENDBASER_DIRTY);
    g_assert_cmphex(dirty1 & VPENDBASER_DIRTY, ==, 0);

    submit(qts, 3, 0x29 | BIT_ULL(8) | VCONF_ADDR,
           ((uint64_t)invalid_vpeid << 32) | TEST_DEFAULT_DB, BIT_ULL(63),
           VPT_ADDR | 13);
    for (i = 0; i < G_N_ELEMENTS(invalid_vmapp_canary); i++) {
        g_assert_cmphex(qtest_readq(qts, invalid_addr + i * 8), ==,
                        invalid_vmapp_canary[i]);
        qtest_writeq(qts, invalid_addr + i * 8, invalid_vte_canary[i]);
    }

    qtest_writeq(qts, invalid_ite_addr, invalid_ite_canary);
    qtest_writel(qts, invalid_ite_addr + 8, invalid_ite_hi_canary);
    submit(qts, 4, 0x2a, ((uint64_t)invalid_vpeid << 32) | 1,
           ((uint64_t)1023 << 32) | (TEST_VINTID + 1), 0);
    g_assert_cmphex(qtest_readq(qts, invalid_ite_addr), ==,
                    invalid_ite_canary);
    g_assert_cmphex(qtest_readl(qts, invalid_ite_addr + 8), ==,
                    invalid_ite_hi_canary);

    submit(qts, 5, 0x22, (uint64_t)invalid_vpeid << 32,
           BIT_ULL(63) | BIT_ULL(16), TEST_DEFAULT_DB + 1);
    for (i = 0; i < G_N_ELEMENTS(invalid_vte_canary); i++) {
        g_assert_cmphex(qtest_readq(qts, invalid_addr + i * 8), ==,
                        invalid_vte_canary[i]);
    }

    submit(qts, 6, 0x22, (uint64_t)TEST_VPEID << 32,
           BIT_ULL(63) | ((uint64_t)2 << 16), TEST_DEFAULT_DB + 1);
    for (i = 0; i < G_N_ELEMENTS(valid_vte); i++) {
        g_assert_cmphex(qtest_readq(qts, vpe_addr() + i * 8), ==,
                        valid_vte[i]);
    }
    g_assert_cmphex(qtest_readq(qts, ITT_ADDR), ==, valid_ite);
    g_assert_cmphex(qtest_readl(qts, ITT_ADDR + 8), ==, valid_ite_hi);
    g_assert_cmphex(qtest_readb(qts, VPT_ADDR + TEST_VINTID / 8), ==,
                    pending);
    g_assert_cmphex(vpendbaser(qts, 0), ==, dirty0);
    g_assert_cmphex(vpendbaser(qts, 1), ==, dirty1);
    qtest_quit(qts);
}

static void test_feature_gate_rejects_enhanced_command(void)
{
    QTestState *qts = start_qtest(false, NULL);

    g_assert_cmphex(qtest_readq(qts, GITS_BASE + GITS_TYPER) &
                    (GITS_TYPER_VMAPP | (3ULL << GITS_TYPER_SVPET_SHIFT)),
                    ==, 0);
    configure_its(qts, true);
    vmapp(qts, 0, 0);
    g_assert_cmphex(qtest_readq(qts, VPE_TABLE_ADDR + TEST_VPEID * 8), ==, 0);
    qtest_quit(qts);
}

static void test_migration_preserves_vpe_state(void)
{
    g_autofree char *tmpdir = g_dir_make_tmp("its-vpe-migration-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(tmpdir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    QTestState *src = start_qtest(true, NULL);
    QTestState *dst;

    configure_its(src, true);
    vmapp(src, 0, 0);
    map_device_and_vlpi(src, 1);
    vmovp(src, 3, 1);
    dst = start_qtest(true, incoming);
    qtest_qmp_assert_success(src,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(src, "STOP");
    qtest_qmp_eventwait(dst, "RESUME");
    assert_vte(dst, 1, TEST_DEFAULT_DB + 1);
    configure_residency(dst);
    qtest_writel(dst, GITS_BASE + GITS_TRANSLATER, 0);
    g_assert_cmphex(vpendbaser(dst, 0) & VPENDBASER_DIRTY, ==, 0);
    g_assert_cmphex(vpendbaser(dst, 1) & VPENDBASER_DIRTY, ==,
                    VPENDBASER_DIRTY);
    qtest_quit(src);
    qtest_quit(dst);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv4-1-its-vpe/typer-entry-sizes",
                   test_typer_and_entry_sizes);
    qtest_add_func("/arm-gicv4-1-its-vpe/vmapp-vmapti-vmovp",
                   test_vmapp_vmapti_vmovp);
    qtest_add_func("/arm-gicv4-1-its-vpe/invalid-table-id",
                   test_invalid_table_and_id);
    qtest_add_func("/arm-gicv4-1-its-vpe/feature-gate",
                   test_feature_gate_rejects_enhanced_command);
    qtest_add_func("/arm-gicv4-1-its-vpe/migration",
                   test_migration_preserves_vpe_state);
    return g_test_run();
}
