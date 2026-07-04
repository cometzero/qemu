/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "hw/timer/arm_arch_timer_mmio.h"

#define TIMER_BASE              0x0c000000ULL
#define FRAME0_BASE             (TIMER_BASE + 0x10000)
#define FRAME1_BASE             (TIMER_BASE + 0x20000)
#define TEST_CNTFRQ             ((uint64_t)100000000)
#define NS_PER_10_TICKS         100ULL

typedef struct TimerQTest {
    QTestState *qts;
    char *dtb_path;
} TimerQTest;

static const uint8_t minimal_virt_dtb[] = {
    0xd0, 0x0d, 0xfe, 0xed, 0x00, 0x00, 0x00, 0xbe,
    0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x98,
    0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x11,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 0x60,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x11,
    0x00, 0x00, 0x00, 0x1b, 0x6c, 0x69, 0x6e, 0x75,
    0x78, 0x2c, 0x64, 0x75, 0x6d, 0x6d, 0x79, 0x2d,
    0x76, 0x69, 0x72, 0x74, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x63, 0x68, 0x6f, 0x73,
    0x65, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x09,
    0x23, 0x61, 0x64, 0x64, 0x72, 0x65, 0x73, 0x73,
    0x2d, 0x63, 0x65, 0x6c, 0x6c, 0x73, 0x00, 0x23,
    0x73, 0x69, 0x7a, 0x65, 0x2d, 0x63, 0x65, 0x6c,
    0x6c, 0x73, 0x00, 0x63, 0x6f, 0x6d, 0x70, 0x61,
    0x74, 0x69, 0x62, 0x6c, 0x65, 0x00,
};

static uint64_t timer_read64(QTestState *qts, uint64_t base, uint32_t offset)
{
    uint32_t lo = qtest_readl(qts, base + offset);
    uint32_t hi = qtest_readl(qts, base + offset + 4);

    return ((uint64_t)hi << 32) | lo;
}

static void timer_write64(QTestState *qts, uint64_t base, uint32_t offset,
                          uint64_t value)
{
    qtest_writel(qts, base + offset, extract64(value, 0, 32));
    qtest_writel(qts, base + offset + 4, extract64(value, 32, 32));
}

static void timer_qtest_write_dtb(char **path)
{
    GError *err = NULL;
    ssize_t len;
    int fd;

    fd = g_file_open_tmp("arm-arch-timer-mmio-XXXXXX.dtb", path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);

    len = qemu_write_full(fd, minimal_virt_dtb, sizeof(minimal_virt_dtb));
    g_assert_cmpint(len, ==, sizeof(minimal_virt_dtb));
    close(fd);
}

static TimerQTest timer_qtest_start(const char *extra)
{
    TimerQTest t;

    timer_qtest_write_dtb(&t.dtb_path);
    t.qts = qtest_initf("-nodefaults "
                        "-machine virt "
                        "-dtb %s "
                        "-device " TYPE_ARM_ARCH_TIMER_MMIO
                        ",id=archtimer"
                        ",cntfrq=%" PRIu64
                        ",nr-frames=2"
                        ",frame-offset-0=0x10000"
                        ",frame-offset-1=0x20000"
                        "%s",
                        t.dtb_path, TEST_CNTFRQ, extra ? extra : "");
    return t;
}

static void timer_qtest_stop(TimerQTest *t)
{
    qtest_quit(t->qts);
    unlink(t->dtb_path);
    g_free(t->dtb_path);
}

static void test_shared_counter_independent_frames(void)
{
    TimerQTest t = timer_qtest_start("");
    QTestState *qts = t.qts;
    uint64_t f0_before, f1_before, f0_after, f1_after, target;
    uint32_t ctl0, ctl1;

    qtest_irq_intercept_out_named(qts, "/machine/peripheral/archtimer",
                                  "sysbus-irq");

    g_assert_cmpuint(qtest_readl(qts, TIMER_BASE + ARM_ARCH_TIMER_MMIO_CNTCTL_CNTFRQ),
                     ==, TEST_CNTFRQ);
    g_assert_cmpuint(qtest_readl(qts, FRAME0_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTFRQ),
                     ==, TEST_CNTFRQ);
    g_assert_cmpuint(qtest_readl(qts, FRAME1_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTFRQ),
                     ==, TEST_CNTFRQ);

    f0_before = timer_read64(qts, FRAME0_BASE,
                             ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_LO);
    f1_before = timer_read64(qts, FRAME1_BASE,
                             ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_LO);
    g_assert_cmpuint(f0_before, ==, f1_before);

    qtest_clock_step(qts, NS_PER_10_TICKS);

    f0_after = timer_read64(qts, FRAME0_BASE,
                            ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_LO);
    f1_after = timer_read64(qts, FRAME1_BASE,
                            ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_LO);
    g_assert_cmpuint(f0_after, ==, f1_after);
    g_assert_cmpuint(f0_after - f0_before, ==, 10);

    target = f0_after + 5;
    timer_write64(qts, FRAME0_BASE, ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO,
                  target);
    timer_write64(qts, FRAME1_BASE, ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO,
                  target + 50);

    qtest_writel(qts, FRAME0_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL,
                 ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE);
    qtest_writel(qts, FRAME1_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL,
                 ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE |
                 ARM_ARCH_TIMER_MMIO_CNTP_CTL_IMASK);

    qtest_clock_step(qts, 50);

    ctl0 = qtest_readl(qts, FRAME0_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL);
    ctl1 = qtest_readl(qts, FRAME1_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL);
    g_assert_cmpuint(ctl0 & ARM_ARCH_TIMER_MMIO_CNTP_CTL_ISTAT, !=, 0);
    g_assert_cmpuint(ctl1 & ARM_ARCH_TIMER_MMIO_CNTP_CTL_ISTAT, ==, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_clock_step(qts, 500);

    ctl1 = qtest_readl(qts, FRAME1_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL);
    g_assert_cmpuint(ctl1 & ARM_ARCH_TIMER_MMIO_CNTP_CTL_ISTAT, !=, 0);
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_writel(qts, FRAME1_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL,
                 ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE);
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_writel(qts, FRAME1_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL,
                 ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE |
                 ARM_ARCH_TIMER_MMIO_CNTP_CTL_IMASK);
    ctl1 = qtest_readl(qts, FRAME1_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL);
    g_assert_cmpuint(ctl1 & ARM_ARCH_TIMER_MMIO_CNTP_CTL_ISTAT, !=, 0);
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_writel(qts, FRAME0_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL, 0);
    ctl0 = qtest_readl(qts, FRAME0_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL);
    ctl1 = qtest_readl(qts, FRAME1_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CTL);
    g_assert_cmpuint(ctl0 & ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE, ==, 0);
    g_assert_cmpuint(ctl1 & ARM_ARCH_TIMER_MMIO_CNTP_CTL_ENABLE, !=, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    timer_qtest_stop(&t);
}

static void test_non_monotonic_frame_offsets(void)
{
    TimerQTest t = timer_qtest_start(",frame-offset-0=0x20000"
                                     ",frame-offset-1=0x10000");
    QTestState *qts = t.qts;
    uint64_t ns_frame0_count, s_frame1_count;

    qtest_clock_step(qts, NS_PER_10_TICKS);

    ns_frame0_count = timer_read64(qts, TIMER_BASE + 0x20000,
                                   ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_LO);
    s_frame1_count = timer_read64(qts, TIMER_BASE + 0x10000,
                                  ARM_ARCH_TIMER_MMIO_CNTBASE_CNTPCT_LO);

    g_assert_cmpuint(ns_frame0_count, ==, s_frame1_count);
    g_assert_cmpuint(qtest_readl(qts, TIMER_BASE + 0x20000 +
                                ARM_ARCH_TIMER_MMIO_CNTBASE_CNTFID),
                     ==, 0);
    g_assert_cmpuint(qtest_readl(qts, TIMER_BASE + 0x10000 +
                                ARM_ARCH_TIMER_MMIO_CNTBASE_CNTFID),
                     ==, 1);

    timer_qtest_stop(&t);
}

static void test_64bit_cval_access(void)
{
    TimerQTest t = timer_qtest_start("");
    QTestState *qts = t.qts;
    uint64_t cval = 0x123456789abcdef0ULL;
    uint64_t split_cval = 0x0fedcba987654321ULL;

    qtest_writeq(qts, FRAME0_BASE + ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO,
                 cval);
    g_assert_cmphex(qtest_readq(qts, FRAME0_BASE +
                                ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO),
                    ==, cval);
    g_assert_cmphex(qtest_readl(qts, FRAME0_BASE +
                                ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO),
                    ==, extract64(cval, 0, 32));
    g_assert_cmphex(qtest_readl(qts, FRAME0_BASE +
                                ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_HI),
                    ==, extract64(cval, 32, 32));

    timer_write64(qts, FRAME0_BASE, ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO,
                  split_cval);
    g_assert_cmphex(qtest_readq(qts, FRAME0_BASE +
                                ARM_ARCH_TIMER_MMIO_CNTBASE_CNTP_CVAL_LO),
                    ==, split_cval);

    timer_qtest_stop(&t);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/arm-arch-timer-mmio/shared-counter-independent-frames",
                   test_shared_counter_independent_frames);
    qtest_add_func("/arm-arch-timer-mmio/non-monotonic-frame-offsets",
                   test_non_monotonic_frame_offsets);
    qtest_add_func("/arm-arch-timer-mmio/64bit-cval-access",
                   test_64bit_cval_access);

    return g_test_run();
}
