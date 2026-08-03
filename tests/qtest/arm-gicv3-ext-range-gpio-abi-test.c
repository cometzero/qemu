/*
 * QTest coverage for the Arm GICv3 extended interrupt GPIO ABI.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"

#define GIC_PATH               "/machine/unattached/device[1]"
#define GICD_BASE              0x08000000
#define GICR_BASE              0x080a0000
#define GICR_SGI_BASE          (GICR_BASE + 0x10000)
#define GICD_ISPENDR           0x0200
#define GICR_ISPENDR0          0x0200
#define NORMAL_SPI_COUNT       256
#define INTERNAL_IRQ_COUNT     32
#define DEFAULT_GPIO_COUNT     (NORMAL_SPI_COUNT + INTERNAL_IRQ_COUNT)
#define MAX_NORMAL_SPI_COUNT   NORMAL_SPI_COUNT
#define MAX_ESPI_COUNT         1024
#define MAX_EPPI_COUNT         64
#define MAX_GPIO_COUNT         (MAX_NORMAL_SPI_COUNT + MAX_ESPI_COUNT + \
                                INTERNAL_IRQ_COUNT + MAX_EPPI_COUNT)

static QTestState *gicv3_qtest_start(const char *extra_args)
{
    return qtest_initf("-machine virt,gic-version=3 -m 64M -nodefaults %s",
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

static void test_zero_extension_gpio_count(void)
{
    /* Given: the legacy virt GICv3 configuration with no extension options. */
    QTestState *qts = gicv3_qtest_start(NULL);

    /* When: the realized unnamed input GPIOs are enumerated. */
    unsigned int count = unnamed_gpio_count(qts);

    /* Then: the pre-extension GPIO count is byte-for-byte unchanged. */
    g_assert_cmpuint(count, ==, DEFAULT_GPIO_COUNT);
    qtest_quit(qts);
}

static void test_zero_extension_gpio_indices(void)
{
    /* Given: reset state in the legacy GPIO layout. */
    QTestState *qts = gicv3_qtest_start(NULL);

    /* When: the first/last SPI and first PPI GPIO indices are asserted. */
    qtest_set_irq_in(qts, GIC_PATH, NULL, 0, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL, NORMAL_SPI_COUNT - 1, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL, NORMAL_SPI_COUNT + 16, 1);

    /* Then: they still select INTIDs 32, 287, and CPU0 PPI16. */
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISPENDR + 4) & 1,
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts, GICD_BASE + GICD_ISPENDR + 32) &
                    BIT(31), ==, BIT(31));
    g_assert_cmphex(qtest_readl(qts, GICR_SGI_BASE + GICR_ISPENDR0) & BIT(16),
                    ==, BIT(16));
    qtest_quit(qts);
}

static void test_valid_extension_properties(void)
{
    /* Given: architecturally valid maximum normal and extended ranges. */
    const char *properties =
        "-global arm-gicv3.num-espi=1024 "
        "-global arm-gicv3.num-eppi=64";

    /* When: the production QEMU binary realizes the software GIC. */
    QTestState *qts = gicv3_qtest_start(properties);

    /* Then: the valid extended configuration is accepted. */
    qtest_quit(qts);
}

static void test_extended_gpio_order(void)
{
    /* Given: one CPU with all normal and extended interrupt ranges enabled. */
    const char *properties =
        "-global arm-gicv3.num-espi=1024 "
        "-global arm-gicv3.num-eppi=64";
    QTestState *qts = gicv3_qtest_start(properties);

    /* When: every family boundary is driven in ABI order. */
    g_assert_cmpuint(unnamed_gpio_count(qts), ==, MAX_GPIO_COUNT);
    qtest_set_irq_in(qts, GIC_PATH, NULL, MAX_NORMAL_SPI_COUNT - 1, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL, MAX_NORMAL_SPI_COUNT, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL,
                     MAX_NORMAL_SPI_COUNT + MAX_ESPI_COUNT - 1, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL,
                     MAX_NORMAL_SPI_COUNT + MAX_ESPI_COUNT + 16, 1);
    qtest_set_irq_in(qts, GIC_PATH, NULL, MAX_GPIO_COUNT - 1, 1);

    /* Then: the exact ABI count is exposed and all boundary inputs survive. */
    qtest_quit(qts);
}

static void assert_realize_fails(const char *property, const char *message)
{
    const char *qemu = qtest_qemu_binary(NULL);
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

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL,
                               NULL, &stderr_text, &wait_status, NULL));
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), !=, 0);
    g_assert_nonnull(strstr(stderr_text, message));
}

static void test_invalid_extension_properties(void)
{
    /* Given: counts that cross a hole or violate a range granule. */
    const struct {
        const char *property;
        const char *message;
    } cases[] = {
        { "arm-gicv3.num-espi=31", "num-espi must be zero" },
        { "arm-gicv3.num-espi=1056", "num-espi must be zero" },
        { "arm-gicv3.num-eppi=33", "num-eppi must be 0" },
    };

    /* When: production QEMU realizes each invalid configuration. */
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        /* Then: realization fails closed with an actionable count error. */
        assert_realize_fails(cases[i].property, cases[i].message);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv3-ext-range-gpio-abi/pin/count",
                   test_zero_extension_gpio_count);
    qtest_add_func("/arm-gicv3-ext-range-gpio-abi/pin/indices",
                   test_zero_extension_gpio_indices);
    qtest_add_func("/arm-gicv3-ext-range-gpio-abi/extensions/properties",
                   test_valid_extension_properties);
    qtest_add_func("/arm-gicv3-ext-range-gpio-abi/extensions/order",
                   test_extended_gpio_order);
    qtest_add_func("/arm-gicv3-ext-range-gpio-abi/extensions/invalid",
                   test_invalid_extension_properties);
    return g_test_run();
}
