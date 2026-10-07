/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define GICD 0x08000000
#define GICR 0x080a0000
#define SGI (GICR + 0x10000)
#define TIMER_PPI (1U << 27)
#define WAKE_IRQ 6 /* One CPU, after IRQ/FIQ/VIRQ/VFIQ/NMI/VNMI. */

static void test_redistributor_wake(void)
{
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3 -cpu max -m 64M -nodefaults");

    qtest_irq_intercept_out_named(qts, "/machine/unattached/device[1]",
                                  "sysbus-irq");
    qtest_writel(qts, GICD, (1U << 6) | 1); /* DS, Group 0 enabled. */
    qtest_writeb(qts, SGI + 0x400 + 27, 0x80);
    qtest_writel(qts, GICR + 0x14, 0); /* Awake; ICC groups remain disabled. */
    qtest_writel(qts, SGI + 0x200, TIMER_PPI);
    qtest_writel(qts, GICR + 0x14, 2);
    g_assert_false(qtest_get_irq(qts, WAKE_IRQ)); /* Interrupt disabled. */
    qtest_writel(qts, SGI + 0x100, TIMER_PPI);
    g_assert_true(qtest_get_irq(qts, WAKE_IRQ));
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_writel(qts, SGI + 0x280, TIMER_PPI);
    g_assert_false(qtest_get_irq(qts, WAKE_IRQ));
    qtest_writel(qts, GICR + 0x14, 0);
    qtest_writel(qts, SGI + 0x200, TIMER_PPI);
    g_assert_false(qtest_get_irq(qts, WAKE_IRQ));
    qtest_writel(qts, GICR + 0x14, 2); /* Pending before sleep: no lost wake. */
    g_assert_true(qtest_get_irq(qts, WAKE_IRQ));
    qtest_writel(qts, GICR + 0x14, 0);
    g_assert_false(qtest_get_irq(qts, WAKE_IRQ));
    qtest_quit(qts);
}

static void test_spi_wake(void)
{
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3 -cpu max -m 64M -nodefaults");

    qtest_irq_intercept_out_named(qts, "/machine/unattached/device[1]",
                                  "sysbus-irq");
    qtest_writel(qts, GICD, (1U << 6) | (1U << 4) | 1);
    qtest_writeq(qts, GICD + 0x6100, 0); /* Route SPI 32 to affinity 0. */
    qtest_writeb(qts, GICD + 0x420, 0x80);
    qtest_writel(qts, GICD + 0x104, 1);
    qtest_writel(qts, GICR + 0x14, 0);
    qtest_set_irq_in(qts, "/machine/unattached/device[1]", NULL, 0, 1);
    g_assert_false(qtest_get_irq(qts, WAKE_IRQ));
    qtest_writel(qts, GICR + 0x14, 2);
    g_assert_true(qtest_get_irq(qts, WAKE_IRQ));
    qtest_set_irq_in(qts, "/machine/unattached/device[1]", NULL, 0, 0);
    g_assert_false(qtest_get_irq(qts, WAKE_IRQ));
    qtest_quit(qts);
}

static void run_wfi(bool powerdown)
{
    uint32_t code[] = {
        0xd2800020, /* mov x0, #1 */
        0xd518f2e0, /* msr S3_0_C15_C2_7, x0: CORE_PWRDN_EN */
        0xd5033fdf, /* isb */
        0xd503207f, /* wfi */
        0x14000000, /* b . */
    };
    QTestState *qts = qtest_init(
        "-machine virt,gic-version=3,secure=on -cpu cortex-a720ae "
        "-m 64M -nodefaults -accel tcg -S "
        "-device loader,addr=0x40000000,cpu-num=0");
    int64_t deadline;

    code[0] = powerdown ? 0xd2800020 : 0xd2800000; /* mov x0, #1 / #0 */
    qtest_irq_intercept_out_named(qts, "/machine/unattached/device[0]",
                                  "powerdown-wfi");
    for (unsigned i = 0; i < G_N_ELEMENTS(code); i++) {
        qtest_writel(qts, 0x40000000 + 4 * i, code[i]);
    }
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    bool reached_wfi = false;
    while (!reached_wfi && g_get_monotonic_time() < deadline) {
        QDict *response = qtest_qmp(qts,
            "{'execute':'human-monitor-command',"
            "'arguments':{'command-line':'info registers'}}");
        const char *registers = qdict_get_str(response, "return");

        /* PC after the WFI instruction proves the guest reached idle. */
        reached_wfi = strstr(registers, "PC=0000000040000010") != NULL;
        qobject_unref(response);
        if (!reached_wfi) {
            g_usleep(1000);
        }
    }
    g_assert_true(reached_wfi);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, powerdown);
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_system_reset(qts);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_powerdown_wfi(void)
{
    run_wfi(true);
}

static void test_ordinary_wfi(void)
{
    run_wfi(false);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv3/wake-request", test_redistributor_wake);
    qtest_add_func("/arm-gicv3/spi-wake-request", test_spi_wake);
    qtest_add_func("/arm-a720ae/powerdown-wfi", test_powerdown_wfi);
    qtest_add_func("/arm-a720ae/ordinary-wfi", test_ordinary_wfi);
    return g_test_run();
}
