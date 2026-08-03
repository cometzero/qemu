/*
 * QTest coverage for GICv3 extended-range CPU-interface selection.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/intc/gicv3_internal.h"

#define GIC_PATH "/machine/unattached/device[2]"
#define GICD_BASE 0x08000000
#define GICR_BASE 0x080a0000
#define GICR_STRIDE 0x20000
#define GICR_SGI_OFFSET 0x10000
#define MAILBOX_BASE 0x40600000
#define MAILBOX_READY(cpu) (MAILBOX_BASE + 0x100 + (cpu) * 8)
#define MAILBOX_COMMAND(cpu) (MAILBOX_BASE + 0x120 + (cpu) * 8)
#define MAILBOX_ACK(cpu) (MAILBOX_BASE + 0x140 + (cpu) * 8)
#define MAILBOX_ARG (MAILBOX_BASE + 0x160)
#define MAILBOX_SPLIT_MODE (MAILBOX_BASE + 0x168)
#define MAILBOX_BOOT_MODE (MAILBOX_BASE + 0x170)
#define MAILBOX_CPUIF_GROUP (MAILBOX_BASE + 0x178)
#define MAILBOX_IAR(cpu) (MAILBOX_BASE + 0x180 + (cpu) * 8)
#define MAILBOX_PHASE(cpu) (MAILBOX_BASE + 0x1a0 + (cpu) * 8)
#define MAILBOX_DIR_GO(cpu) (MAILBOX_BASE + 0x1c0 + (cpu) * 8)
#define MAILBOX_SECURE_ACTIVE (MAILBOX_BASE + 0x1e0)
#define NORMAL_SPI_COUNT 256
#define ESPI_GPIO_BASE NORMAL_SPI_COUNT
#define PPI_GPIO_BASE (ESPI_GPIO_BASE + GICV3_MAX_ESPI)
#define EPPI_GPIO_BASE (PPI_GPIO_BASE + 2 * GIC_INTERNAL)
#define SECURE_EPPI_GPIO_BASE (PPI_GPIO_BASE + GIC_INTERNAL)
#define SECURE_GIC_PATH "/machine/unattached/device[1]"

static char *guest_directory;
static char *guest_elf;

void device_class_set_parent_realize(DeviceClass *dc,
                                     DeviceRealize dev_realize,
                                     DeviceRealize *parent_realize)
{
    *parent_realize = NULL;
}

#include "../../hw/intc/arm_gicv3.c"

typedef struct ExtRangeFixture {
    GICv3State gic;
    GICv3CPUState cpu[2];
} ExtRangeFixture;

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

static void fixture_init(ExtRangeFixture *f)
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

static void set_normal_spi(ExtRangeFixture *f, uint32_t intid,
                           uint8_t priority, unsigned int cpu)
{
    set_bit32(intid, f->gic.enabled);
    set_bit32(intid, f->gic.pending);
    f->gic.gicd_ipriority[intid] = priority;
    f->gic.gicd_irouter_target[intid] = &f->cpu[cpu];
}

static void set_normal_ppi(ExtRangeFixture *f, uint32_t intid,
                           uint8_t priority, unsigned int cpu)
{
    f->cpu[cpu].gicr_ienabler0 |= BIT(intid);
    f->cpu[cpu].gicr_ipendr0 |= BIT(intid);
    f->cpu[cpu].gicr_ipriorityr[intid] = priority;
}

static void set_espi(ExtRangeFixture *f, uint32_t intid,
                     uint8_t priority, unsigned int cpu)
{
    uint32_t index = intid - GICV3_ESPI_INTID_START;

    set_bit32(index, f->gic.espi_enabled);
    set_bit32(index, f->gic.espi_pending);
    f->gic.espi_priority[index] = priority;
    f->gic.espi_irouter_target[index] = &f->cpu[cpu];
}

static void set_eppi(ExtRangeFixture *f, uint32_t intid,
                     uint8_t priority, unsigned int cpu)
{
    uint32_t index = intid - GICV3_EPPI_INTID_START;

    set_bit32(index, f->cpu[cpu].eppi_enabled);
    set_bit32(index, f->cpu[cpu].eppi_pending);
    f->cpu[cpu].eppi_priority[index] = priority;
}

static void assert_hppi(ExtRangeFixture *f, unsigned int cpu,
                        uint32_t intid, uint8_t priority)
{
    gicv3_full_update(&f->gic);
    g_assert_cmpuint(f->cpu[cpu].hppi.irq, ==, intid);
    g_assert_cmphex(f->cpu[cpu].hppi.prio, ==, priority);
    g_assert_cmpint(f->cpu[cpu].hppi.grp, ==, GICV3_G0);
}

static void test_normal_spi_selection_pin(void)
{
    ExtRangeFixture f = { 0 };

    fixture_init(&f);
    set_normal_spi(&f, 32, 0x40, 0);
    set_normal_spi(&f, 991, 0x80, 1);
    assert_hppi(&f, 0, 32, 0x40);
    g_assert_cmpuint(f.cpu[1].hppi.irq, ==, 991);
}

static void test_normal_ppi_selection_pin(void)
{
    ExtRangeFixture f = { 0 };

    fixture_init(&f);
    set_normal_ppi(&f, 16, 0x30, 0);
    set_normal_ppi(&f, 31, 0x70, 1);
    assert_hppi(&f, 0, 16, 0x30);
    g_assert_cmpuint(f.cpu[1].hppi.irq, ==, 31);
}

static void test_espi_first_last_selection(void)
{
    ExtRangeFixture f = { 0 };

    fixture_init(&f);
    set_espi(&f, 4096, 0x40, 0);
    set_espi(&f, 5119, 0x20, 1);
    assert_hppi(&f, 0, 4096, 0x40);
    g_assert_cmpuint(f.cpu[1].hppi.irq, ==, 5119);
}

static void test_eppi_first_last_selection(void)
{
    ExtRangeFixture f = { 0 };

    fixture_init(&f);
    set_eppi(&f, 1056, 0x40, 0);
    set_eppi(&f, 1119, 0x20, 1);
    assert_hppi(&f, 0, 1056, 0x40);
    g_assert_cmpuint(f.cpu[1].hppi.irq, ==, 1119);
}

static void test_espi_priority_competition(void)
{
    ExtRangeFixture f = { 0 };

    fixture_init(&f);
    set_normal_spi(&f, 32, 0x80, 0);
    set_espi(&f, 5119, 0x20, 0);
    assert_hppi(&f, 0, 5119, 0x20);
}

static void test_eppi_priority_competition(void)
{
    ExtRangeFixture f = { 0 };

    fixture_init(&f);
    set_normal_ppi(&f, 16, 0x80, 1);
    set_eppi(&f, 1056, 0x20, 1);
    assert_hppi(&f, 1, 1056, 0x20);
}

static void test_extended_group_disable(void)
{
    ExtRangeFixture f = { 0 };

    fixture_init(&f);
    f.gic.gicd_ctlr = GICD_CTLR_DS;
    set_espi(&f, 4096, 0x20, 0);
    set_eppi(&f, 1056, 0x10, 1);
    gicv3_full_update(&f.gic);
    g_assert_cmphex(f.cpu[0].hppi.prio, ==, 0xff);
    g_assert_cmphex(f.cpu[1].hppi.prio, ==, 0xff);
}

static uint64_t gicr_sgi_base(unsigned int cpu)
{
    return GICR_BASE + cpu * GICR_STRIDE;
}

static void wait_for_value(QTestState *qts, uint64_t address,
                           uint64_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time() < deadline) {
        if (qtest_readq(qts, address) == expected) {
            return;
        }
        g_usleep(1000);
    }
    g_error("timeout waiting for guest value at 0x%" PRIx64, address);
}

static void compile_guest(void)
{
    const char *compiler = g_getenv("AARCH64_CC");
    const char *source = ARM_GICV3_EXT_RANGE_CPUIF_GUEST;
    g_autofree char *stderr_text = NULL;
    int wait_status;

    guest_directory = g_dir_make_tmp("gicv3-ext-range-cpuif-XXXXXX", NULL);
    g_assert_nonnull(guest_directory);
    guest_elf = g_build_filename(guest_directory, "guest.elf", NULL);
    if (!compiler) {
        compiler = "aarch64-linux-gnu-gcc";
    }
    const char *argv[] = {
        compiler, "-nostdlib", "-Wl,-Ttext=0x40400000",
        "-Wl,--build-id=none", "-o", guest_elf, source, NULL,
    };

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                               &stderr_text, &wait_status, NULL));
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), ==, 0);
    g_assert_cmpstr(stderr_text, ==, "");
}

static QTestState *live_qtest_start(void)
{
    QTestState *qts = qtest_initf(
        "-machine virt,gic-version=3,virtualization=off "
        "-cpu max -m 64M -nodefaults -smp 2 -accel tcg -S "
        "-global arm-gicv3.num-espi=1024 "
        "-global arm-gicv3.num-eppi=64 "
        "-device loader,file=%s,cpu-num=0", guest_elf);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_value(qts, MAILBOX_READY(0), 1);
    wait_for_value(qts, MAILBOX_READY(1), 1);
    qtest_writel(qts, GICD_BASE,
                 qtest_readl(qts, GICD_BASE) | GICD_CTLR_EN_GRP1NS);
    return qts;
}

static QTestState *live_qtest_start_960_spis(void)
{
    QTestState *qts = qtest_initf(
        "-machine virt,gic-version=3,virtualization=off,gic-num-spi=960 "
        "-cpu max -m 64M -nodefaults -smp 2 -accel tcg -S "
        "-global arm-gicv3.num-espi=1024 "
        "-global arm-gicv3.num-eppi=64 "
        "-device loader,file=%s,cpu-num=0", guest_elf);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_value(qts, MAILBOX_READY(0), 1);
    wait_for_value(qts, MAILBOX_READY(1), 1);
    qtest_writel(qts, GICD_BASE,
                 qtest_readl(qts, GICD_BASE) | GICD_CTLR_EN_GRP1NS);
    return qts;
}

static void assert_invalid_spi_count(uint32_t count,
                                     bool expect_property_error)
{
    const char *qemu = g_getenv("QTEST_QEMU_BINARY");
    g_autofree char *machine =
        g_strdup_printf("virt,gic-version=3,gic-num-spi=%" PRIu32, count);
    g_autofree char *stderr_text = NULL;
    int wait_status;
    const char *argv[] = {
        qemu, "-machine", machine, "-cpu", "max", "-m", "64M",
        "-nodefaults", "-smp", "2", "-accel", "tcg", "-S",
        "-display", "none", NULL,
    };

    g_assert_nonnull(qemu);
    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL,
                               NULL, &stderr_text, &wait_status, NULL));
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), !=, 0);
    g_assert_null(strstr(stderr_text, "Assertion"));
    if (expect_property_error) {
        g_assert_nonnull(strstr(stderr_text, "gic-num-spi"));
    }
}

static QTestState *live_secure_qtest_start(void)
{
    QTestState *qts = qtest_initf(
        "-machine virt,gic-version=3,virtualization=off,secure=on "
        "-cpu max -m 64M -nodefaults -smp 1 -accel tcg -S "
        "-global arm-gicv3.num-espi=1024 "
        "-global arm-gicv3.num-eppi=64 "
        "-device loader,file=%s,cpu-num=0", guest_elf);

    qtest_writeq(qts, MAILBOX_BOOT_MODE, 1);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    wait_for_value(qts, MAILBOX_READY(0), 1);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE) & GICD_CTLR_DS, ==, 0);
    return qts;
}

static void guest_command(QTestState *qts, unsigned int cpu,
                          uint64_t command)
{
    uint64_t ack = qtest_readq(qts, MAILBOX_ACK(cpu));

    qtest_writeq(qts, MAILBOX_COMMAND(cpu), command);
    wait_for_value(qts, MAILBOX_ACK(cpu), ack + 1);
}

static void configure_spi(QTestState *qts, uint32_t intid,
                          uint8_t priority, unsigned int cpu)
{
    uint32_t bit = intid % 32;

    qtest_writel(qts, GICD_BASE + GICD_IGROUPR + (intid / 32) * 4,
                 BIT(bit));
    qtest_writeb(qts, GICD_BASE + GICD_IPRIORITYR + intid, priority);
    qtest_writel(qts, GICD_BASE + GICD_ICFGR + (intid / 16) * 4,
                 BIT((intid % 16) * 2 + 1));
    qtest_writeq(qts, GICD_BASE + GICD_IROUTER + intid * 8, cpu);
    qtest_writel(qts, GICD_BASE + GICD_ISENABLER + (intid / 32) * 4,
                 BIT(bit));
}

static void configure_ppi(QTestState *qts, uint32_t intid,
                          uint8_t priority, unsigned int cpu)
{
    uint64_t base = gicr_sgi_base(cpu);

    qtest_writel(qts, base + GICR_IGROUPR0, BIT(intid));
    qtest_writeb(qts, base + GICR_IPRIORITYR + intid, priority);
    qtest_writel(qts, base + GICR_ICFGR1,
                 BIT((intid - GIC_NR_SGIS) * 2 + 1));
    qtest_writel(qts, base + GICR_ISENABLER0, BIT(intid));
}

static void configure_espi(QTestState *qts, uint32_t intid,
                           uint8_t priority, unsigned int cpu)
{
    uint32_t index = intid - GICV3_ESPI_INTID_START;
    uint32_t word = index / 32;
    uint32_t bit = index % 32;

    qtest_writel(qts, GICD_BASE + GICD_IGROUPRnE + word * 4, BIT(bit));
    qtest_writeb(qts, GICD_BASE + GICD_IPRIORITYRnE + index, priority);
    qtest_writel(qts, GICD_BASE + GICD_ICFGRnE + (index / 16) * 4,
                 BIT((index % 16) * 2 + 1));
    qtest_writeq(qts, GICD_BASE + GICD_IROUTERnE + index * 8, cpu);
    qtest_writel(qts, GICD_BASE + GICD_ISENABLERnE + word * 4, BIT(bit));
}

static void configure_eppi(QTestState *qts, uint32_t intid,
                           uint8_t priority, unsigned int cpu)
{
    uint32_t index = intid - GICV3_EPPI_INTID_START;
    uint64_t base = gicr_sgi_base(cpu);

    qtest_writel(qts, base + GICR_IGROUPR0 + 4 + (index / 32) * 4,
                 BIT(index % 32));
    qtest_writeb(qts, base + GICR_IPRIORITYR + GIC_INTERNAL + index,
                 priority);
    qtest_writel(qts, base + GICR_ICFGR0 + 8 + (index / 16) * 4,
                 BIT((index % 16) * 2 + 1));
    qtest_writel(qts, base + GICR_ISENABLER0 + 4 + (index / 32) * 4,
                 BIT(index % 32));
}

static void inject_and_wait(QTestState *qts, unsigned int gpio,
                            unsigned int cpu, uint32_t intid)
{
    qtest_set_irq_in(qts, GIC_PATH, NULL, gpio, 1);
    wait_for_value(qts, MAILBOX_PHASE(cpu), 2);
    qtest_set_irq_in(qts, GIC_PATH, NULL, gpio, 0);
    g_assert_cmpuint(qtest_readq(qts, MAILBOX_IAR(cpu)), ==, intid);
}

static void inject_and_wait_path(QTestState *qts, const char *path,
                                 unsigned int gpio, unsigned int cpu,
                                 uint32_t intid)
{
    qtest_set_irq_in(qts, path, NULL, gpio, 1);
    wait_for_value(qts, MAILBOX_PHASE(cpu), 2);
    qtest_set_irq_in(qts, path, NULL, gpio, 0);
    g_assert_cmpuint(qtest_readq(qts, MAILBOX_IAR(cpu)), ==, intid);
}

static void reset_irq_result(QTestState *qts, unsigned int cpu)
{
    qtest_writeq(qts, MAILBOX_PHASE(cpu), 0);
    qtest_writeq(qts, MAILBOX_IAR(cpu), 0);
}

static void test_live_normal_spi_ppi(void)
{
    QTestState *qts = live_qtest_start();

    guest_command(qts, 0, 3);
    guest_command(qts, 1, 3);
    configure_spi(qts, 32, 0x40, 0);
    inject_and_wait(qts, 0, 0, 32);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISACTIVER + 4), ==,
                    0);
    reset_irq_result(qts, 0);

    configure_ppi(qts, 16, 0x40, 0);
    inject_and_wait(qts, PPI_GPIO_BASE + 16, 0, 16);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(0) +
                               GICR_ISACTIVER0), ==, 0);
    reset_irq_result(qts, 0);

    configure_ppi(qts, 31, 0x40, 1);
    inject_and_wait(qts, PPI_GPIO_BASE + GIC_INTERNAL + 31, 1, 31);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(1) +
                               GICR_ISACTIVER0), ==, 0);
    qtest_quit(qts);
}

static void test_live_spi991_eoimode1_dir(void)
{
    static const uint32_t below_virt_minimum[] = {
        0, 1, 31, 32, 33, 224, 255,
    };
    static const uint32_t invalid_gic_sizes[] = {
        257, 959, 961, 992, 1024, UINT32_MAX,
    };
    QTestState *qts = live_qtest_start_960_spis();

    for (size_t i = 0; i < ARRAY_SIZE(below_virt_minimum); i++) {
        assert_invalid_spi_count(below_virt_minimum[i], true);
    }
    for (size_t i = 0; i < ARRAY_SIZE(invalid_gic_sizes); i++) {
        assert_invalid_spi_count(invalid_gic_sizes[i], false);
    }

    qtest_writeq(qts, MAILBOX_SPLIT_MODE, 1);
    guest_command(qts, 0, 4);
    configure_spi(qts, 991, 0x40, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISENABLER +
                               (991 / 32) * 4), ==, BIT(31));
    inject_and_wait(qts, 991 - GIC_INTERNAL, 0, 991);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISACTIVER +
                               (991 / 32) * 4), ==, BIT(31));
    qtest_writeq(qts, MAILBOX_DIR_GO(0), 1);
    wait_for_value(qts, MAILBOX_PHASE(0), 3);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISACTIVER +
                               (991 / 32) * 4), ==, 0);
    qtest_quit(qts);
}

static void test_live_espi_eoimode0(void)
{
    QTestState *qts = live_qtest_start();

    guest_command(qts, 0, 3);
    configure_espi(qts, 4096, 0x40, 0);
    inject_and_wait(qts, ESPI_GPIO_BASE, 0, 4096);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISACTIVERnE) & BIT(0),
                    ==, 0);
    qtest_quit(qts);
}

static void test_live_espi_eoimode1_dir(void)
{
    QTestState *qts = live_qtest_start();

    qtest_writeq(qts, MAILBOX_SPLIT_MODE, 1);
    guest_command(qts, 0, 4);
    configure_espi(qts, 5119, 0x40, 0);
    inject_and_wait(qts, ESPI_GPIO_BASE + GICV3_MAX_ESPI - 1, 0, 5119);
    g_assert_cmphex(qtest_readl(qts,
                               GICD_BASE + GICD_ISACTIVERnE + 31 * 4), ==,
                    BIT(31));
    qtest_writeq(qts, MAILBOX_DIR_GO(0), 1);
    wait_for_value(qts, MAILBOX_PHASE(0), 3);
    g_assert_cmphex(qtest_readl(qts,
                               GICD_BASE + GICD_ISACTIVERnE + 31 * 4), ==,
                    0);
    qtest_quit(qts);
}

static void test_live_eppi_last_cpu_eoimode0(void)
{
    QTestState *qts = live_qtest_start();

    guest_command(qts, 1, 3);
    configure_eppi(qts, 1056, 0x40, 1);
    inject_and_wait(qts, EPPI_GPIO_BASE + GICV3_MAX_EPPI, 1, 1056);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(1) +
                               GICR_ISACTIVER0 + 4), ==, 0);
    qtest_quit(qts);
}

static void test_live_eppi_last_cpu_eoimode1_dir(void)
{
    QTestState *qts = live_qtest_start();

    qtest_writeq(qts, MAILBOX_SPLIT_MODE, 1);
    guest_command(qts, 1, 4);
    configure_eppi(qts, 1119, 0x40, 1);
    inject_and_wait(qts, EPPI_GPIO_BASE + 2 * GICV3_MAX_EPPI - 1, 1, 1119);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(1) +
                               GICR_ISACTIVER0 + 8), ==, BIT(31));
    qtest_writeq(qts, MAILBOX_DIR_GO(1), 1);
    wait_for_value(qts, MAILBOX_PHASE(1), 3);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(1) +
                               GICR_ISACTIVER0 + 8), ==, 0);
    qtest_quit(qts);
}

static void test_live_eppi_cpu0_first_eoimode0(void)
{
    QTestState *qts = live_qtest_start();

    guest_command(qts, 0, 3);
    configure_eppi(qts, 1056, 0x40, 0);
    inject_and_wait(qts, EPPI_GPIO_BASE, 0, 1056);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(0) +
                               GICR_ISACTIVER0 + 4), ==, 0);
    qtest_quit(qts);
}

static void test_live_eppi_cpu0_last_eoimode1_dir(void)
{
    QTestState *qts = live_qtest_start();

    qtest_writeq(qts, MAILBOX_SPLIT_MODE, 1);
    guest_command(qts, 0, 4);
    configure_eppi(qts, 1119, 0x40, 0);
    inject_and_wait(qts, EPPI_GPIO_BASE + GICV3_MAX_EPPI - 1, 0, 1119);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(0) +
                               GICR_ISACTIVER0 + 8), ==, BIT(31));
    qtest_writeq(qts, MAILBOX_DIR_GO(0), 1);
    wait_for_value(qts, MAILBOX_PHASE(0), 3);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(0) +
                               GICR_ISACTIVER0 + 8), ==, 0);
    qtest_quit(qts);
}

static void test_live_wrong_cpu_eoi_dir(void)
{
    QTestState *qts = live_qtest_start();

    qtest_writeq(qts, MAILBOX_SPLIT_MODE, 1);
    guest_command(qts, 0, 4);
    guest_command(qts, 1, 4);
    configure_espi(qts, 4096, 0x40, 0);
    inject_and_wait(qts, ESPI_GPIO_BASE, 0, 4096);
    qtest_writeq(qts, MAILBOX_ARG, 4096);
    guest_command(qts, 1, 1);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISACTIVERnE), ==,
                    BIT(0));
    guest_command(qts, 1, 2);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISACTIVERnE), ==,
                    BIT(0));
    qtest_writeq(qts, MAILBOX_DIR_GO(0), 1);
    wait_for_value(qts, MAILBOX_PHASE(0), 3);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISACTIVERnE), ==, 0);
    qtest_quit(qts);
}

static void test_live_wrong_cpu_eppi_eoi_dir(void)
{
    QTestState *qts = live_qtest_start();

    qtest_writeq(qts, MAILBOX_SPLIT_MODE, 1);
    guest_command(qts, 0, 4);
    guest_command(qts, 1, 4);
    configure_eppi(qts, 1056, 0x40, 1);
    inject_and_wait(qts, EPPI_GPIO_BASE + GICV3_MAX_EPPI, 1, 1056);
    qtest_writeq(qts, MAILBOX_ARG, 1056);
    guest_command(qts, 0, 1);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(1) +
                               GICR_ISACTIVER0 + 4), ==, BIT(0));
    guest_command(qts, 0, 2);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(1) +
                               GICR_ISACTIVER0 + 4), ==, BIT(0));
    qtest_writeq(qts, MAILBOX_DIR_GO(1), 1);
    wait_for_value(qts, MAILBOX_PHASE(1), 3);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(1) +
                               GICR_ISACTIVER0 + 4), ==, 0);
    qtest_quit(qts);
}

static void test_live_secure_group0_espi(void)
{
    QTestState *qts = live_secure_qtest_start();

    qtest_writeq(qts, MAILBOX_SPLIT_MODE, 1);
    guest_command(qts, 0, 4);
    guest_command(qts, 0, 10);
    guest_command(qts, 0, 11);
    inject_and_wait_path(qts, SECURE_GIC_PATH, ESPI_GPIO_BASE, 0, 4096);
    g_assert_cmphex(qtest_readq(qts, MAILBOX_SECURE_ACTIVE), ==, BIT(0));
    qtest_writeq(qts, MAILBOX_DIR_GO(0), 1);
    wait_for_value(qts, MAILBOX_PHASE(0), 3);
    g_assert_cmphex(qtest_readq(qts, MAILBOX_SECURE_ACTIVE), ==, 0);
    qtest_quit(qts);
}

static void test_live_secure_group1s_eppi(void)
{
    QTestState *qts = live_secure_qtest_start();

    guest_command(qts, 0, 3);
    guest_command(qts, 0, 12);
    inject_and_wait_path(qts, SECURE_GIC_PATH, SECURE_EPPI_GPIO_BASE,
                         0, 1056);
    g_assert_cmphex(qtest_readl(qts, gicr_sgi_base(0) +
                               GICR_ISACTIVER0 + 4), ==, 0);
    qtest_quit(qts);
}

static void assert_hole_spurious(QTestState *qts)
{
    reset_irq_result(qts, 0);
    guest_command(qts, 0, 7);
    g_assert_cmpuint(qtest_readq(qts, MAILBOX_IAR(0)), ==, INTID_SPURIOUS);
}

static void test_live_reserved_holes_spurious(void)
{
    QTestState *qts = live_qtest_start();

    guest_command(qts, 0, 5);

    qtest_writel(qts, GICD_BASE + GICD_ISPENDR + (992 / 32) * 4, BIT(0));
    assert_hole_spurious(qts);

    qtest_writel(qts, gicr_sgi_base(0) + GICR_ISPENDR0 + 12, BIT(0));
    assert_hole_spurious(qts);

    qtest_writel(qts, GICD_BASE + GICD_ISPENDR + (4095 / 32) * 4,
                 BIT(31));
    assert_hole_spurious(qts);

    qtest_writel(qts, GICD_BASE + GICD_ISPENDRnE +
                 (GICV3_MAX_ESPI / 32) * 4, BIT(0));
    assert_hole_spurious(qts);

    qtest_quit(qts);
}

static void test_live_wrong_group_not_delivered(void)
{
    QTestState *qts = live_qtest_start();

    configure_espi(qts, 4096, 0x40, 0);
    qtest_writel(qts, GICD_BASE + GICD_IGROUPRnE, 0);
    qtest_set_irq_in(qts, GIC_PATH, NULL, ESPI_GPIO_BASE, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL, ESPI_GPIO_BASE, 0);
    g_usleep(50 * 1000);
    g_assert_cmpuint(qtest_readq(qts, MAILBOX_PHASE(0)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISACTIVERnE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISPENDRnE), ==,
                    BIT(0));
    qtest_quit(qts);
}

static void test_live_extended_priority_competition(void)
{
    QTestState *qts = live_qtest_start();

    guest_command(qts, 0, 5);
    configure_espi(qts, 5119, 0x80, 0);
    configure_eppi(qts, 1056, 0x20, 0);
    qtest_set_irq_in(qts, GIC_PATH, NULL,
                     ESPI_GPIO_BASE + GICV3_MAX_ESPI - 1, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL, EPPI_GPIO_BASE, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL,
                     ESPI_GPIO_BASE + GICV3_MAX_ESPI - 1, 0);
    qtest_set_irq_in(qts, GIC_PATH, NULL, EPPI_GPIO_BASE, 0);
    qtest_writeq(qts, MAILBOX_COMMAND(0), 6);
    wait_for_value(qts, MAILBOX_PHASE(0), 2);
    g_assert_cmpuint(qtest_readq(qts, MAILBOX_IAR(0)), ==, 1056);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/pin/spi",
                   test_normal_spi_selection_pin);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/pin/ppi",
                   test_normal_ppi_selection_pin);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/espi/first-last",
                   test_espi_first_last_selection);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/eppi/first-last",
                   test_eppi_first_last_selection);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/priority/espi",
                   test_espi_priority_competition);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/priority/eppi",
                   test_eppi_priority_competition);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/group/disabled",
                   test_extended_group_disable);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/normal-spi-ppi",
                   test_live_normal_spi_ppi);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/spi991-eoimode1-dir",
                   test_live_spi991_eoimode1_dir);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/espi-eoimode0",
                   test_live_espi_eoimode0);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/espi-eoimode1-dir",
                   test_live_espi_eoimode1_dir);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/eppi-last-cpu-eoimode0",
                   test_live_eppi_last_cpu_eoimode0);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/eppi-last-cpu-eoimode1-dir",
                   test_live_eppi_last_cpu_eoimode1_dir);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/eppi-cpu0-first-eoimode0",
                   test_live_eppi_cpu0_first_eoimode0);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/eppi-cpu0-last-eoimode1-dir",
                   test_live_eppi_cpu0_last_eoimode1_dir);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/wrong-cpu-eoi-dir",
                   test_live_wrong_cpu_eoi_dir);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/wrong-cpu-eppi-eoi-dir",
                   test_live_wrong_cpu_eppi_eoi_dir);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/secure-group0-espi",
                   test_live_secure_group0_espi);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/secure-group1s-eppi",
                   test_live_secure_group1s_eppi);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/reserved-holes-spurious",
                   test_live_reserved_holes_spurious);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/wrong-group",
                   test_live_wrong_group_not_delivered);
    qtest_add_func("/arm-gicv3-ext-range-cpuif/live/priority-competition",
                   test_live_extended_priority_competition);
    compile_guest();
    int ret = g_test_run();

    g_assert_cmpint(g_remove(guest_elf), ==, 0);
    g_assert_cmpint(g_rmdir(guest_directory), ==, 0);
    g_free(guest_elf);
    g_free(guest_directory);
    return ret;
}
