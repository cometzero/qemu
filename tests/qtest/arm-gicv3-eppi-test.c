/*
 * QTest coverage for Arm GICv3 extended PPIs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "hw/intc/arm_gicv3_common.h"

MemTxResult gicv3_redist_read(void *opaque, hwaddr offset, uint64_t *data,
                              unsigned int size, MemTxAttrs attrs);
MemTxResult gicv3_redist_write(void *opaque, hwaddr offset, uint64_t data,
                               unsigned int size, MemTxAttrs attrs);
void gicv3_redist_update(GICv3CPUState *cs);
void gicv3_cpuif_virt_irq_fiq_update(GICv3CPUState *cs);

#define GIC_PATH                 "/machine/unattached/device[5]"
#define GICR_BASE                0x080a0000
#define GICR_STRIDE              0x20000
#define GICR_TYPER               0x0008
#define GICR_SGI_OFFSET          0x10000
#define GICR_IGROUPR0            0x0080
#define GICR_ISENABLER0          0x0100
#define GICR_ICENABLER0          0x0180
#define GICR_ISPENDR0            0x0200
#define GICR_ICPENDR0            0x0280
#define GICR_ISACTIVER0          0x0300
#define GICR_ICACTIVER0          0x0380
#define GICR_IPRIORITYR0         0x0400
#define GICR_ICFGR0              0x0c00
#define GICR_IGRPMODR0           0x0d00
#define GICR_NSACR0              0x0e00
#define GICR_TYPER_PPINUM_MASK   (0x1fULL << 27)
#define GICR_TYPER_PPINUM_SHIFT  27
#define GICD_CTLR_DS_BIT         BIT(6)
#define NORMAL_SPI_COUNT         256
#define INTERNAL_IRQ_COUNT       32
#define TEST_CPU_COUNT           5
#define MAX_EPPI_COUNT           64
#define EPPI_GPIO_BASE           (NORMAL_SPI_COUNT + \
                                  INTERNAL_IRQ_COUNT * TEST_CPU_COUNT)

typedef struct DirectRedistFixture {
    GICv3State gic;
    GICv3CPUState cpu;
    GICv3RedistRegion region;
} DirectRedistFixture;

void gicv3_redist_update(GICv3CPUState *cs)
{
}

void gicv3_cpuif_virt_irq_fiq_update(GICv3CPUState *cs)
{
}

MemoryRegion *flatview_translate(FlatView *fv, hwaddr addr, hwaddr *xlat,
                                 hwaddr *len, bool is_write,
                                 MemTxAttrs attrs)
{
    g_assert_not_reached();
}

MemTxResult flatview_read_continue(FlatView *fv, hwaddr addr,
                                   MemTxAttrs attrs, void *buf,
                                   hwaddr len, hwaddr addr1, hwaddr l,
                                   MemoryRegion *mr)
{
    g_assert_not_reached();
}

void *qemu_map_ram_ptr(RAMBlock *ram_block, ram_addr_t addr)
{
    g_assert_not_reached();
}

bool memory_region_is_ram_device(const MemoryRegion *mr)
{
    g_assert_not_reached();
}

MemTxResult address_space_write(AddressSpace *as, hwaddr addr,
                                MemTxAttrs attrs, const void *buf,
                                hwaddr len)
{
    g_assert_not_reached();
}

static void direct_redist_init(DirectRedistFixture *fixture,
                               unsigned int num_eppi, bool ds)
{
    fixture->gic.num_cpu = 1;
    fixture->gic.num_eppi = num_eppi;
    fixture->gic.gicd_ctlr = ds ? GICD_CTLR_DS_BIT : 0;
    fixture->gic.cpu = &fixture->cpu;
    fixture->cpu.gic = &fixture->gic;
    fixture->cpu.hppi.prio = 0xff;
    fixture->region.gic = &fixture->gic;
}

static uint64_t direct_redist_read(DirectRedistFixture *fixture,
                                   hwaddr offset, unsigned int size,
                                   bool secure)
{
    MemTxAttrs attrs = { .secure = secure };
    uint64_t value = UINT64_MAX;

    g_assert_cmpint(gicv3_redist_read(&fixture->region,
                                      GICR_SGI_OFFSET + offset,
                                      &value, size, attrs), ==, MEMTX_OK);
    return value;
}

static void direct_redist_write(DirectRedistFixture *fixture,
                                hwaddr offset, uint64_t value,
                                unsigned int size, bool secure)
{
    MemTxAttrs attrs = { .secure = secure };

    g_assert_cmpint(gicv3_redist_write(&fixture->region,
                                       GICR_SGI_OFFSET + offset,
                                       value, size, attrs), ==, MEMTX_OK);
}

static uint64_t gicr_base(unsigned int cpu)
{
    return GICR_BASE + cpu * GICR_STRIDE;
}

static uint64_t gicr_sgi_base(unsigned int cpu)
{
    return gicr_base(cpu) + GICR_SGI_OFFSET;
}

static QTestState *gicv3_qtest_start(const char *extra_args)
{
    return qtest_initf("-machine virt,gic-version=3 -m 64M "
                       "-nodefaults -smp %u %s", TEST_CPU_COUNT,
                       extra_args ? extra_args : "");
}

static unsigned int unnamed_gpio_count(QTestState *qts)
{
    QDict *response = qtest_qmp(
        qts, "{ 'execute': 'qom-list', 'arguments': { 'path': %s } }",
        GIC_PATH);
    QList *properties = qdict_get_qlist(response, "return");
    QListEntry *entry;
    unsigned int count = 0;

    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_has_prefix(qdict_get_str(property, "name"),
                             "unnamed-gpio-in[")) {
            count++;
        }
    }
    qobject_unref(response);
    return count;
}

static unsigned int ppinum(QTestState *qts, unsigned int cpu)
{
    uint64_t typer = qtest_readq(qts, gicr_base(cpu) + GICR_TYPER);

    return (typer & GICR_TYPER_PPINUM_MASK) >> GICR_TYPER_PPINUM_SHIFT;
}

static void test_pin_normal_ppi_layout(void)
{
    /* Given: a legacy GIC with no extended PPIs. */
    QTestState *qts = gicv3_qtest_start(NULL);
    uint64_t sgi = gicr_sgi_base(0);

    /* When: the normal PPI bank and redistributor discovery are exercised. */
    qtest_writel(qts, sgi + GICR_ISENABLER0, BIT(16));
    qtest_writel(qts, sgi + GICR_ISPENDR0, BIT(16));

    /* Then: the old layout and PPInum=0 discovery remain exact. */
    g_assert_cmpuint(ppinum(qts, 0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0), ==, BIT(16));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISPENDR0), ==, BIT(16));
    g_assert_cmpuint(unnamed_gpio_count(qts), ==,
                     NORMAL_SPI_COUNT + INTERNAL_IRQ_COUNT * TEST_CPU_COUNT);
    qtest_quit(qts);
}

static void test_eppi_discovery(void)
{
    /* Given: GICs implementing 32 and 64 EPPIs on five CPUs. */
    QTestState *eppi32 = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=32");
    QTestState *eppi64 = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=64");

    /* When: software reads GICR_TYPER on CPU0 and CPU4. */
    unsigned int first32 = ppinum(eppi32, 0);
    unsigned int last32 = ppinum(eppi32, 4);
    unsigned int first64 = ppinum(eppi64, 0);
    unsigned int last64 = ppinum(eppi64, 4);

    /* Then: PPInum encodes one or two architected 32-EPPI blocks. */
    g_assert_cmpuint(first32, ==, 1);
    g_assert_cmpuint(last32, ==, 1);
    g_assert_cmpuint(first64, ==, 2);
    g_assert_cmpuint(last64, ==, 2);
    qtest_quit(eppi32);
    qtest_quit(eppi64);
}

static void test_eppi_group_priority_config(void)
{
    /* Given: CPU0 and CPU4 redistributors with the full EPPI range. */
    QTestState *qts = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=64");
    uint64_t cpu0 = gicr_sgi_base(0);
    uint64_t cpu4 = gicr_sgi_base(4);

    /* When: first and last EPPI group, priority, config and modifier vary. */
    qtest_writel(qts, cpu0 + GICR_IGROUPR0 + 4, BIT(0));
    qtest_writeb(qts, cpu0 + GICR_IPRIORITYR0 + 32, 0x28);
    qtest_writel(qts, cpu0 + GICR_ICFGR0 + 8, BIT(1));
    qtest_writel(qts, cpu4 + GICR_IGROUPR0 + 8, BIT(31));
    qtest_writeb(qts, cpu4 + GICR_IPRIORITYR0 + 95, 0xa0);
    qtest_writel(qts, cpu4 + GICR_ICFGR0 + 20, BIT(31));

    /* Then: each CPU-local bank returns only its programmed EPPI state. */
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_IGROUPR0 + 4), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readb(qts, cpu0 + GICR_IPRIORITYR0 + 32), ==,
                    0x28);
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ICFGR0 + 8), ==, BIT(1));
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_IGROUPR0 + 8), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readb(qts, cpu4 + GICR_IPRIORITYR0 + 95), ==,
                    0xa0);
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ICFGR0 + 20), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_IGROUPR0 + 8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_IGROUPR0 + 4), ==, 0);
    qtest_quit(qts);
}

static void test_eppi_enable_pending_active(void)
{
    /* Given: reset first and last EPPI bitmap banks on separate CPUs. */
    QTestState *qts = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=64");
    uint64_t cpu0 = gicr_sgi_base(0);
    uint64_t cpu4 = gicr_sgi_base(4);

    /* When: software sets enable, pending, and active state. */
    qtest_writel(qts, cpu0 + GICR_ISENABLER0 + 4, BIT(0));
    qtest_writel(qts, cpu0 + GICR_ISPENDR0 + 4, BIT(0));
    qtest_writel(qts, cpu0 + GICR_ISACTIVER0 + 4, BIT(0));
    qtest_writel(qts, cpu4 + GICR_ISENABLER0 + 8, BIT(31));
    qtest_writel(qts, cpu4 + GICR_ISPENDR0 + 8, BIT(31));
    qtest_writel(qts, cpu4 + GICR_ISACTIVER0 + 8, BIT(31));

    /* Then: state is visible through both set and clear register aliases. */
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ICENABLER0 + 4), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ICPENDR0 + 4), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ICACTIVER0 + 4), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ICENABLER0 + 8), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ICPENDR0 + 8), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ICACTIVER0 + 8), ==,
                    BIT(31));
    qtest_writel(qts, cpu0 + GICR_ICENABLER0 + 4, BIT(0));
    qtest_writel(qts, cpu0 + GICR_ICPENDR0 + 4, BIT(0));
    qtest_writel(qts, cpu0 + GICR_ICACTIVER0 + 4, BIT(0));
    qtest_writel(qts, cpu4 + GICR_ICENABLER0 + 8, BIT(31));
    qtest_writel(qts, cpu4 + GICR_ICPENDR0 + 8, BIT(31));
    qtest_writel(qts, cpu4 + GICR_ICACTIVER0 + 8, BIT(31));
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ISENABLER0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ISENABLER0 + 8), ==, 0);
    qtest_quit(qts);
}

static void test_eppi_gpio_injection_is_cpu_local(void)
{
    /* Given: level-sensitive EPPI1056 on CPU0 and EPPI1119 on CPU4. */
    QTestState *qts = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=64");
    uint64_t cpu0 = gicr_sgi_base(0);
    uint64_t cpu4 = gicr_sgi_base(4);
    unsigned int cpu4_last = EPPI_GPIO_BASE + 4 * MAX_EPPI_COUNT + 63;

    /* When: the two CPU-major EPPI GPIO inputs are asserted. */
    g_assert_cmpuint(unnamed_gpio_count(qts), ==,
                     EPPI_GPIO_BASE + TEST_CPU_COUNT * MAX_EPPI_COUNT);
    qtest_set_irq_in(qts, GIC_PATH, NULL, EPPI_GPIO_BASE, 1);
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ISPENDR0 + 4), ==,
                    BIT(0));
    qtest_set_irq_in(qts, GIC_PATH, NULL, cpu4_last, 1);

    /* Then: pending state appears only in the owning redistributor. */
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ISPENDR0 + 4), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ISPENDR0 + 8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ISPENDR0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ISPENDR0 + 8), ==,
                    BIT(31));
    qtest_set_irq_in(qts, GIC_PATH, NULL, EPPI_GPIO_BASE, 0);
    qtest_set_irq_in(qts, GIC_PATH, NULL, cpu4_last, 0);
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ISPENDR0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ISPENDR0 + 8), ==, 0);
    qtest_quit(qts);
}

static void test_eppi_reset(void)
{
    /* Given: configured first and last EPPI state on CPU0 and CPU4. */
    QTestState *qts = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=64");
    uint64_t cpu0 = gicr_sgi_base(0);
    uint64_t cpu4 = gicr_sgi_base(4);

    qtest_writel(qts, cpu0 + GICR_IGROUPR0 + 4, BIT(0));
    qtest_writel(qts, cpu0 + GICR_ISENABLER0 + 4, BIT(0));
    qtest_writel(qts, cpu0 + GICR_ISPENDR0 + 4, BIT(0));
    qtest_writeb(qts, cpu4 + GICR_IPRIORITYR0 + 95, 0x80);
    qtest_writel(qts, cpu4 + GICR_ICFGR0 + 20, BIT(31));

    /* When: the machine completes a system reset. */
    qtest_system_reset(qts);

    /* Then: all EPPI state returns to architectural reset values. */
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_IGROUPR0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ISENABLER0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, cpu0 + GICR_ISPENDR0 + 4), ==, 0);
    g_assert_cmphex(qtest_readb(qts, cpu4 + GICR_IPRIORITYR0 + 95), ==, 0);
    g_assert_cmphex(qtest_readl(qts, cpu4 + GICR_ICFGR0 + 20), ==, 0);
    g_assert_cmpuint(ppinum(qts, 0), ==, 2);
    g_assert_cmpuint(ppinum(qts, 4), ==, 2);
    qtest_quit(qts);
}

static void test_eppi_vmstate(void)
{
    /* Given: distinct CPU0/CPU4 EPPI state and a fresh migration target. */
    g_autofree char *tmpdir = g_dir_make_tmp("gicv3-eppi-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(tmpdir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf(
        "-global arm-gicv3.num-eppi=64 -incoming %s", uri);
    QTestState *source = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=64");
    QTestState *destination;
    uint64_t cpu0 = gicr_sgi_base(0);
    uint64_t cpu4 = gicr_sgi_base(4);

    qtest_writel(source, cpu0 + GICR_IGROUPR0 + 4, BIT(0));
    qtest_writel(source, cpu0 + GICR_ISPENDR0 + 4, BIT(0));
    qtest_writeb(source, cpu0 + GICR_IPRIORITYR0 + 32, 0x30);
    qtest_writel(source, cpu4 + GICR_ISACTIVER0 + 8, BIT(31));
    qtest_writeb(source, cpu4 + GICR_IPRIORITYR0 + 95, 0x90);
    destination = gicv3_qtest_start(incoming);

    /* When: the production parent VMState migrates between fresh processes. */
    qtest_qmp_assert_success(source,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(source, "STOP");
    qtest_qmp_eventwait(destination, "RESUME");

    /* Then: CPU-local EPPI group, pending, active and priority survive. */
    g_assert_cmphex(qtest_readl(destination, cpu0 + GICR_IGROUPR0 + 4), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readl(destination, cpu0 + GICR_ISPENDR0 + 4), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readb(destination,
                               cpu0 + GICR_IPRIORITYR0 + 32), ==, 0x30);
    g_assert_cmphex(qtest_readl(destination,
                               cpu4 + GICR_ISACTIVER0 + 8), ==, BIT(31));
    g_assert_cmphex(qtest_readb(destination,
                               cpu4 + GICR_IPRIORITYR0 + 95), ==, 0x90);
    qtest_quit(source);
    qtest_quit(destination);
    g_rmdir(tmpdir);
}

static void test_disabled_eppi_range_is_raz_wi(void)
{
    /* Given: PPInum=0 and therefore no EPPI GPIO or register state. */
    QTestState *qts = gicv3_qtest_start(NULL);
    uint64_t sgi = gicr_sgi_base(0);

    /* When: software writes the first and last possible EPPI banks. */
    qtest_writel(qts, sgi + GICR_IGROUPR0 + 4, BIT(0));
    qtest_writel(qts, sgi + GICR_ISENABLER0 + 4, BIT(0));
    qtest_writel(qts, sgi + GICR_ISPENDR0 + 8, BIT(31));
    qtest_writeb(qts, sgi + GICR_IPRIORITYR0 + 95, 0xa0);
    qtest_writel(qts, sgi + GICR_ICFGR0 + 20, BIT(31));

    /* Then: all banks are RAZ/WI and no injection line is exposed. */
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_IGROUPR0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISPENDR0 + 8), ==, 0);
    g_assert_cmphex(qtest_readb(qts, sgi + GICR_IPRIORITYR0 + 95), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ICFGR0 + 20), ==, 0);
    g_assert_cmpuint(unnamed_gpio_count(qts), ==,
                     NORMAL_SPI_COUNT + INTERNAL_IRQ_COUNT * TEST_CPU_COUNT);
    qtest_quit(qts);
}

static void test_eppi_range_limits_and_access_width(void)
{
    /* Given: only one architected 32-EPPI block. */
    QTestState *qts = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=32");
    uint64_t sgi = gicr_sgi_base(0);

    qtest_writel(qts, sgi + GICR_ISENABLER0 + 4, BIT(0));
    qtest_writel(qts, sgi + GICR_ICFGR0 + 8, BIT(1));

    /* When: partial-width and out-of-range bitmap accesses are attempted. */
    qtest_writeb(qts, sgi + GICR_ISENABLER0 + 4, UINT8_MAX);
    qtest_writew(qts, sgi + GICR_ISPENDR0 + 4, UINT16_MAX);
    qtest_writeq(qts, sgi + GICR_ISACTIVER0 + 4, UINT64_MAX);
    qtest_writeq(qts, sgi + GICR_ICFGR0 + 8, UINT64_MAX);
    qtest_writel(qts, sgi + GICR_ISENABLER0 + 5, UINT32_MAX);
    qtest_writel(qts, sgi + GICR_ISENABLER0 + 8, UINT32_MAX);
    qtest_writel(qts, sgi + GICR_ICFGR0 + 16, UINT32_MAX);

    /* Then: valid state survives while malformed and disabled banks are WI. */
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 4), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 5), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ICFGR0 + 8), ==, BIT(1));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ICFGR0 + 16), ==, 0);
    qtest_quit(qts);
}

static void test_eppi1087_num32_tail_boundary(void)
{
    /* Given: EPPI1087 is live and EPPI1088 is beyond a 32-EPPI device. */
    QTestState *qts = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=32");
    uint64_t sgi = gicr_sgi_base(0);

    qtest_writel(qts, sgi + GICR_ISENABLER0, BIT(16));
    qtest_writel(qts, sgi + GICR_IGROUPR0 + 4, BIT(31));
    qtest_writel(qts, sgi + GICR_ISENABLER0 + 4, BIT(31));
    qtest_writel(qts, sgi + GICR_ISPENDR0 + 4, BIT(31));
    qtest_writel(qts, sgi + GICR_ISACTIVER0 + 4, BIT(31));
    qtest_writeb(qts, sgi + GICR_IPRIORITYR0 + 63, 0x78);
    qtest_writel(qts, sgi + GICR_ICFGR0 + 12, BIT(31));

    /* When: every EPPI1088 register family is written at bank 1 bit 0. */
    qtest_writel(qts, sgi + GICR_IGROUPR0 + 8, BIT(0));
    qtest_writel(qts, sgi + GICR_ISENABLER0 + 8, BIT(0));
    qtest_writel(qts, sgi + GICR_ISPENDR0 + 8, BIT(0));
    qtest_writel(qts, sgi + GICR_ISACTIVER0 + 8, BIT(0));
    qtest_writeb(qts, sgi + GICR_IPRIORITYR0 + 64, 0x88);
    qtest_writel(qts, sgi + GICR_ICFGR0 + 16, BIT(1));

    /* Then: EPPI1087 and the normal-PPI canary survive; 1088 is RAZ/WI. */
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0), ==, BIT(16));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_IGROUPR0 + 4), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 4), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISPENDR0 + 4), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISACTIVER0 + 4), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readb(qts, sgi + GICR_IPRIORITYR0 + 63), ==,
                    0x78);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ICFGR0 + 12), ==, BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_IGROUPR0 + 8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISPENDR0 + 8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISACTIVER0 + 8), ==, 0);
    g_assert_cmphex(qtest_readb(qts, sgi + GICR_IPRIORITYR0 + 64), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ICFGR0 + 16), ==, 0);
    qtest_quit(qts);
}

static void test_eppi1088_num64_live_boundary(void)
{
    /* Given: adjacent EPPI1087/1088 banks on a 64-EPPI device. */
    QTestState *qts = gicv3_qtest_start(
        "-global arm-gicv3.num-eppi=64");
    uint64_t sgi = gicr_sgi_base(0);

    /* When: both sides of the boundary receive distinct complete state. */
    qtest_writel(qts, sgi + GICR_IGROUPR0 + 4, BIT(31));
    qtest_writel(qts, sgi + GICR_IGROUPR0 + 8, BIT(0));
    qtest_writel(qts, sgi + GICR_ISENABLER0 + 4, BIT(31));
    qtest_writel(qts, sgi + GICR_ISENABLER0 + 8, BIT(0));
    qtest_writel(qts, sgi + GICR_ISPENDR0 + 4, BIT(31));
    qtest_writel(qts, sgi + GICR_ISPENDR0 + 8, BIT(0));
    qtest_writel(qts, sgi + GICR_ISACTIVER0 + 4, BIT(31));
    qtest_writel(qts, sgi + GICR_ISACTIVER0 + 8, BIT(0));
    qtest_writeb(qts, sgi + GICR_IPRIORITYR0 + 63, 0x68);
    qtest_writeb(qts, sgi + GICR_IPRIORITYR0 + 64, 0x88);
    qtest_writel(qts, sgi + GICR_ICFGR0 + 12, BIT(31));
    qtest_writel(qts, sgi + GICR_ICFGR0 + 16, BIT(1));

    /* Then: neither bank aliases or masks the other boundary interrupt. */
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_IGROUPR0 + 4), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_IGROUPR0 + 8), ==, BIT(0));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 4), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 8), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISPENDR0 + 4), ==, BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISPENDR0 + 8), ==, BIT(0));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISACTIVER0 + 4), ==,
                    BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISACTIVER0 + 8), ==,
                    BIT(0));
    g_assert_cmphex(qtest_readb(qts, sgi + GICR_IPRIORITYR0 + 63), ==,
                    0x68);
    g_assert_cmphex(qtest_readb(qts, sgi + GICR_IPRIORITYR0 + 64), ==,
                    0x88);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ICFGR0 + 12), ==, BIT(31));
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ICFGR0 + 16), ==, BIT(1));
    qtest_quit(qts);
}

static void test_eppi_ds_nonsecure_access(void)
{
    /* Given: DS=1 removes Security-state partitioning for 64 EPPIs. */
    DirectRedistFixture fixture = { 0 };

    direct_redist_init(&fixture, 64, true);
    fixture.cpu.gicr_ienabler0 = BIT(16);

    /* When: Non-secure callbacks program EPPI1056 and secure-only registers. */
    direct_redist_write(&fixture, GICR_IGROUPR0 + 4, BIT(0), 4, false);
    direct_redist_write(&fixture, GICR_ISENABLER0 + 4, BIT(0), 4, false);
    direct_redist_write(&fixture, GICR_ISPENDR0 + 4, BIT(0), 4, false);
    direct_redist_write(&fixture, GICR_ISACTIVER0 + 4, BIT(0), 4, false);
    direct_redist_write(&fixture, GICR_IPRIORITYR0 + 32, 0x44, 1, false);
    direct_redist_write(&fixture, GICR_ICFGR0 + 8, BIT(1), 4, false);
    direct_redist_write(&fixture, GICR_IGRPMODR0 + 4, BIT(0), 4, false);
    direct_redist_write(&fixture, GICR_NSACR0, UINT32_MAX, 4, false);

    /* Then: EPPI state is live, while IGRPMODR/NSACR remain DS RAZ/WI. */
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IGROUPR0 + 4,
                                       4, false), ==, BIT(0));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISENABLER0 + 4,
                                       4, false), ==, BIT(0));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISPENDR0 + 4,
                                       4, false), ==, BIT(0));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISACTIVER0 + 4,
                                       4, false), ==, BIT(0));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IPRIORITYR0 + 32,
                                       1, false), ==, 0x44);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ICFGR0 + 8,
                                       4, false), ==, BIT(1));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IGRPMODR0 + 4,
                                       4, false), ==, 0);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_NSACR0,
                                       4, false), ==, 0);
    g_assert_cmphex(fixture.cpu.gicr_ienabler0, ==, BIT(16));
}

static void test_eppi_secure_positive_access(void)
{
    /* Given: Security extensions are active and EPPI1087 is Secure Group0. */
    DirectRedistFixture fixture = { 0 };

    direct_redist_init(&fixture, 64, false);
    fixture.cpu.eppi_enabled[1] = BIT(0);

    /* When: Secure callbacks program every Task22 state family. */
    direct_redist_write(&fixture, GICR_ISENABLER0 + 4, BIT(31), 4, true);
    direct_redist_write(&fixture, GICR_ISPENDR0 + 4, BIT(31), 4, true);
    direct_redist_write(&fixture, GICR_ISACTIVER0 + 4, BIT(31), 4, true);
    direct_redist_write(&fixture, GICR_IPRIORITYR0 + 63, 0x38, 1, true);
    direct_redist_write(&fixture, GICR_ICFGR0 + 12, BIT(31), 4, true);
    direct_redist_write(&fixture, GICR_IGRPMODR0 + 4, BIT(31), 4, true);

    /* Then: Secure state is visible, Non-secure is RAZ, and bank 1 survives. */
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISENABLER0 + 4,
                                       4, true), ==, BIT(31));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISPENDR0 + 4,
                                       4, true), ==, BIT(31));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISACTIVER0 + 4,
                                       4, true), ==, BIT(31));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IPRIORITYR0 + 63,
                                       1, true), ==, 0x38);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ICFGR0 + 12,
                                       4, true), ==, BIT(31));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IGRPMODR0 + 4,
                                       4, true), ==, BIT(31));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISENABLER0 + 4,
                                       4, false), ==, 0);
    g_assert_cmphex(fixture.cpu.eppi_enabled[1], ==, BIT(0));
}

static void test_eppi_group1ns_access(void)
{
    /* Given: Secure software assigns EPPI1087/1088 to Group1NS. */
    DirectRedistFixture fixture = { 0 };

    direct_redist_init(&fixture, 64, false);
    direct_redist_write(&fixture, GICR_IGROUPR0 + 4, BIT(31), 4, true);
    direct_redist_write(&fixture, GICR_IGROUPR0 + 8, BIT(0), 4, true);

    /* When: Non-secure callbacks program both sides of the bank boundary. */
    direct_redist_write(&fixture, GICR_ISENABLER0 + 4, BIT(31), 4, false);
    direct_redist_write(&fixture, GICR_ISENABLER0 + 8, BIT(0), 4, false);
    direct_redist_write(&fixture, GICR_IPRIORITYR0 + 63, 0x40, 1, false);
    direct_redist_write(&fixture, GICR_IPRIORITYR0 + 64, 0x60, 1, false);
    direct_redist_write(&fixture, GICR_ICFGR0 + 12, BIT(31), 4, false);
    direct_redist_write(&fixture, GICR_ICFGR0 + 16, BIT(1), 4, false);

    /* Then: Group1NS permits access with architectural priority translation. */
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IGROUPR0 + 4,
                                       4, true), ==, BIT(31));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IGROUPR0 + 8,
                                       4, true), ==, BIT(0));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISENABLER0 + 4,
                                       4, false), ==, BIT(31));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISENABLER0 + 8,
                                       4, false), ==, BIT(0));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IPRIORITYR0 + 63,
                                       1, false), ==, 0x40);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IPRIORITYR0 + 64,
                                       1, false), ==, 0x60);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IPRIORITYR0 + 63,
                                       1, true), ==, 0xa0);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_IPRIORITYR0 + 64,
                                       1, true), ==, 0xb0);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ICFGR0 + 12,
                                       4, false), ==, BIT(31));
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ICFGR0 + 16,
                                       4, false), ==, BIT(1));
}

static void test_eppi_nsacr_does_not_grant_access(void)
{
    /* Given: Secure software grants maximum SGI NSACR access. */
    DirectRedistFixture fixture = { 0 };

    direct_redist_init(&fixture, 64, false);
    direct_redist_write(&fixture, GICR_NSACR0, UINT32_MAX, 4, true);

    /* When: Non-secure callbacks attempt to set and clear Group0 EPPI1056. */
    direct_redist_write(&fixture, GICR_ISPENDR0 + 4, BIT(0), 4, false);
    direct_redist_write(&fixture, GICR_ISPENDR0 + 4, BIT(0), 4, true);
    direct_redist_write(&fixture, GICR_ICPENDR0 + 4, BIT(0), 4, false);

    /* Then: NSACR reads securely but does not grant PPI/EPPI access. */
    g_assert_cmphex(direct_redist_read(&fixture, GICR_NSACR0,
                                       4, true), ==, UINT32_MAX);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_NSACR0,
                                       4, false), ==, 0);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISPENDR0 + 4,
                                       4, false), ==, 0);
    g_assert_cmphex(direct_redist_read(&fixture, GICR_ISPENDR0 + 4,
                                       4, true), ==, BIT(0));
}

static void test_invalid_eppi_properties(void)
{
    /* Given: counts outside the three architected property values. */
    const char *qemu = qtest_qemu_binary(NULL);
    const unsigned int invalid_counts[] = { 1, 31, 33, 65 };

    /* When: the production binary realizes each malformed count. */
    for (size_t i = 0; i < G_N_ELEMENTS(invalid_counts); i++) {
        g_autofree char *property = g_strdup_printf(
            "arm-gicv3.num-eppi=%u", invalid_counts[i]);
        const char *argv[] = {
            qemu,
            "-machine", "virt,gic-version=3",
            "-global", property,
            "-display", "none",
            "-nodefaults",
            NULL,
        };
        g_autofree char *stderr_text = NULL;
        int wait_status;

        /* Then: realization rejects the count with the range diagnostic. */
        g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL,
                                   NULL, &stderr_text, &wait_status, NULL));
        g_assert_true(WIFEXITED(wait_status));
        g_assert_cmpint(WEXITSTATUS(wait_status), !=, 0);
        g_assert_nonnull(strstr(stderr_text, "num-eppi must be 0, 32, or 64"));
    }
}

static void test_eppi_nonsecure_group0_access(void)
{
    /* Given: security extensions are active and EPPIs remain Group0. */
    QTestState *qts = qtest_initf(
        "-machine virt,gic-version=3,secure=on -m 64M "
        "-nodefaults -smp %u -global arm-gicv3.num-eppi=64",
        TEST_CPU_COUNT);
    uint64_t sgi = gicr_sgi_base(0);

    /* When: Non-secure qtest transactions write every Task22 state family. */
    qtest_writel(qts, sgi + GICR_IGROUPR0 + 4, BIT(0));
    qtest_writel(qts, sgi + GICR_ISENABLER0 + 4, BIT(0));
    qtest_writel(qts, sgi + GICR_ISPENDR0 + 4, BIT(0));
    qtest_writel(qts, sgi + GICR_ISACTIVER0 + 4, BIT(0));
    qtest_writeb(qts, sgi + GICR_IPRIORITYR0 + 32, 0x40);
    qtest_writel(qts, sgi + GICR_ICFGR0 + 8, BIT(1));
    qtest_writel(qts, sgi + GICR_IGRPMODR0 + 4, BIT(0));

    /* Then: Secure Group0 EPPI state is consistently RAZ/WI. */
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_IGROUPR0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISENABLER0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISPENDR0 + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ISACTIVER0 + 4), ==, 0);
    g_assert_cmphex(qtest_readb(qts, sgi + GICR_IPRIORITYR0 + 32), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_ICFGR0 + 8), ==, 0);
    g_assert_cmphex(qtest_readl(qts, sgi + GICR_IGRPMODR0 + 4), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv3-eppi/pin/normal-ppi-layout",
                   test_pin_normal_ppi_layout);
    qtest_add_func("/arm-gicv3-eppi/discovery", test_eppi_discovery);
    qtest_add_func("/arm-gicv3-eppi/group-priority-config",
                   test_eppi_group_priority_config);
    qtest_add_func("/arm-gicv3-eppi/enable-pending-active",
                   test_eppi_enable_pending_active);
    qtest_add_func("/arm-gicv3-eppi/gpio-cpu-isolation",
                   test_eppi_gpio_injection_is_cpu_local);
    qtest_add_func("/arm-gicv3-eppi/reset", test_eppi_reset);
    qtest_add_func("/arm-gicv3-eppi/vmstate", test_eppi_vmstate);
    qtest_add_func("/arm-gicv3-eppi/disabled-raz-wi",
                   test_disabled_eppi_range_is_raz_wi);
    qtest_add_func("/arm-gicv3-eppi/range-width",
                   test_eppi_range_limits_and_access_width);
    qtest_add_func("/arm-gicv3-eppi/bank-boundary/eppi1087-num32-tail",
                   test_eppi1087_num32_tail_boundary);
    qtest_add_func("/arm-gicv3-eppi/bank-boundary/eppi1088-num64-live",
                   test_eppi1088_num64_live_boundary);
    qtest_add_func("/arm-gicv3-eppi/security/ds-nonsecure",
                   test_eppi_ds_nonsecure_access);
    qtest_add_func("/arm-gicv3-eppi/security/secure-positive",
                   test_eppi_secure_positive_access);
    qtest_add_func("/arm-gicv3-eppi/security/group1ns",
                   test_eppi_group1ns_access);
    qtest_add_func("/arm-gicv3-eppi/security/nsacr-no-eppi-grant",
                   test_eppi_nsacr_does_not_grant_access);
    qtest_add_func("/arm-gicv3-eppi/invalid-property",
                   test_invalid_eppi_properties);
    qtest_add_func("/arm-gicv3-eppi/nonsecure-group0-access",
                   test_eppi_nonsecure_group0_access);
    return g_test_run();
}
