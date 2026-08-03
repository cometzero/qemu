/*
 * QTest coverage for Arm GICv3 extended interrupt VMState.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/intc/gicv3_internal.h"
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "migration/vmstate.h"
#include "io/channel-file.h"

#define GIC_PATH               "/machine/unattached/device[1]"
#define GICD_BASE              0x08000000
#define GICR_BASE              0x080a0000
#define GICR_SGI_BASE          (GICR_BASE + 0x10000)
#define GICD_ISPENDR           0x0200
#define TEST_GICR_ISPENDR0     0x0200
#define NORMAL_SPI_COUNT       256

static GICv3State *observed_dist_state;
static GICv3CPUState *observed_redist_cpu;
static int observed_irq;
static int observed_level;

void gicv3_dist_set_irq(GICv3State *s, int irq, int level)
{
    observed_dist_state = s;
    observed_irq = irq;
    observed_level = level;
}

void gicv3_redist_set_irq(GICv3CPUState *cs, int irq, int level)
{
    observed_redist_cpu = cs;
    observed_irq = irq;
    observed_level = level;
}

void gicv3_full_update(GICv3State *s)
{
}

void gicv3_redist_update(GICv3CPUState *cs)
{
}

static QTestState *gicv3_qtest_start(const char *extra_args)
{
    return qtest_initf("-machine virt,gic-version=3 -m 64M -nodefaults %s",
                       extra_args ? extra_args : "");
}

static char *fixture_path(const char *name)
{
    return g_build_filename(ARM_GICV3_EXT_RANGE_TEST_DATA, name, NULL);
}

static QEMUFile *qemu_file_from_fd(int fd, bool output)
{
    QIOChannel *ioc = QIO_CHANNEL(qio_channel_file_new_fd(fd));
    QEMUFile *file = output ? qemu_file_new_output(ioc) :
                              qemu_file_new_input(ioc);

    object_unref(OBJECT(ioc));
    return file;
}

static char *qmp_read_until_response(FILE *stream)
{
    char *line = NULL;
    size_t capacity = 0;

    while (getline(&line, &capacity, stream) >= 0) {
        if (strstr(line, "\"return\"") || strstr(line, "\"error\"")) {
            return line;
        }
    }
    g_free(line);
    return NULL;
}

static int vmstate_round_trip(const VMStateDescription *description,
                              void *source, void *destination)
{
    g_autofree char *path = NULL;
    Error *local_err = NULL;
    QEMUFile *file;
    int fd;
    int ret;

    fd = g_file_open_tmp("arm-gicv3-ext-range-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    file = qemu_file_from_fd(fd, true);
    ret = vmstate_save_state(file, description, source, NULL, &local_err);
    if (local_err) {
        error_report_err(local_err);
        g_assert_not_reached();
    }
    g_assert_cmpint(ret, ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    fd = g_open(path, O_RDONLY, 0);
    g_assert_cmpint(fd, >=, 0);
    file = qemu_file_from_fd(fd, false);
    ret = vmstate_load_state(file, description, destination, 1, &local_err);
    if (ret) {
        error_free(local_err);
    }
    qemu_fclose(file);
    g_unlink(path);
    return ret;
}

static void test_base_state_round_trip(void)
{
    /* Given: pending legacy SPI and PPI state in a version-1 GIC VMState. */
    g_autofree char *tmpdir = g_dir_make_tmp("gicv3-base-vmstate-XXXXXX",
                                             NULL);
    g_autofree char *socket = g_build_filename(tmpdir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    QTestState *src = gicv3_qtest_start(NULL);
    QTestState *dst;

    qtest_set_irq_in(src, GIC_PATH, NULL, 0, 1);
    qtest_set_irq_in(src, GIC_PATH, NULL, NORMAL_SPI_COUNT + 16, 1);

    /* When: the source migrates to a fresh legacy destination process. */
    dst = gicv3_qtest_start(incoming);
    qtest_qmp_assert_success(src,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(src, "STOP");
    qtest_qmp_eventwait(dst, "RESUME");

    /* Then: the unchanged base VMState preserves both pending inputs. */
    g_assert_cmphex(qtest_readl(dst, GICD_BASE + GICD_ISPENDR + 4) & 1,
                    ==, 1);
    g_assert_cmphex(qtest_readl(dst, GICR_SGI_BASE + TEST_GICR_ISPENDR0) &
                    BIT(16),
                    ==, BIT(16));
    qtest_quit(src);
    qtest_quit(dst);
    g_rmdir(tmpdir);
}

static void test_espi_state_round_trip(void)
{
    /* Given: ESPI state at both ends of the architected extended range. */
    GICv3State source = { .num_espi = GICV3_MAX_ESPI };
    GICv3State destination = { .num_espi = GICV3_MAX_ESPI };

    set_bit32(0, source.espi_pending);
    set_bit32(GICV3_MAX_ESPI - 1, source.espi_active);
    source.espi_priority[0] = 0x28;
    source.espi_priority[GICV3_MAX_ESPI - 1] = 0x90;
    source.espi_irouter[GICV3_MAX_ESPI - 1] = 0x01020304;

    /* When: the version-1 ESPI subsection is saved and loaded. */
    g_assert_cmpint(vmstate_round_trip(&vmstate_gicv3_espi, &source,
                                      &destination), ==, 0);

    /* Then: pending, active, priority, and routing state round-trip. */
    g_assert_true(test_bit32(0, destination.espi_pending));
    g_assert_true(test_bit32(GICV3_MAX_ESPI - 1,
                             destination.espi_active));
    g_assert_cmphex(destination.espi_priority[0], ==, 0x28);
    g_assert_cmphex(destination.espi_priority[GICV3_MAX_ESPI - 1], ==, 0x90);
    g_assert_cmphex(destination.espi_irouter[GICV3_MAX_ESPI - 1], ==,
                    0x01020304);
}

static void test_eppi_state_round_trip(void)
{
    /* Given: two CPUs with distinct EPPI state and priorities. */
    GICv3State source = {
        .num_cpu = 2,
        .num_eppi = GICV3_MAX_EPPI,
        .cpu = g_new0(GICv3CPUState, 2),
    };
    GICv3State destination = {
        .num_cpu = 2,
        .num_eppi = GICV3_MAX_EPPI,
        .cpu = g_new0(GICv3CPUState, 2),
    };

    set_bit32(0, source.cpu[0].eppi_pending);
    set_bit32(GICV3_MAX_EPPI - 1, source.cpu[1].eppi_active);
    source.cpu[0].eppi_priority[0] = 0x30;
    source.cpu[1].eppi_priority[GICV3_MAX_EPPI - 1] = 0xa0;

    /* When: the version-1 EPPI subsection is saved and loaded. */
    g_assert_cmpint(vmstate_round_trip(&vmstate_gicv3_eppi, &source,
                                      &destination), ==, 0);

    /* Then: CPU ownership and family-local state are preserved. */
    g_assert_true(test_bit32(0, destination.cpu[0].eppi_pending));
    g_assert_false(test_bit32(0, destination.cpu[1].eppi_pending));
    g_assert_true(test_bit32(GICV3_MAX_EPPI - 1,
                             destination.cpu[1].eppi_active));
    g_assert_cmphex(destination.cpu[0].eppi_priority[0], ==, 0x30);
    g_assert_cmphex(destination.cpu[1].eppi_priority[GICV3_MAX_EPPI - 1],
                    ==, 0xa0);
    g_free(source.cpu);
    g_free(destination.cpu);
}

static void test_extended_full_state_round_trip(void)
{
    /* Given: two production GICs with both extension subsections enabled. */
    g_autofree char *tmpdir = g_dir_make_tmp("gicv3-ext-vmstate-XXXXXX",
                                             NULL);
    g_autofree char *socket = g_build_filename(tmpdir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf(
        "-smp 2 -global arm-gicv3.num-espi=1024 "
        "-global arm-gicv3.num-eppi=64 -incoming %s", uri);
    const char *source_args =
        "-smp 2 -global arm-gicv3.num-espi=1024 "
        "-global arm-gicv3.num-eppi=64";
    QTestState *source = gicv3_qtest_start(source_args);
    QTestState *destination = gicv3_qtest_start(incoming);

    /* When: the full parent VMState migrates with ESPI and EPPI present. */
    qtest_qmp_assert_success(source,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(source, "STOP");
    qtest_qmp_eventwait(destination, "RESUME");

    /* Then: both optional subsections load for both destination CPUs. */
    QDict *status = qtest_qmp_assert_success_ref(
        destination, "{ 'execute': 'query-status' }");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "running");
    qobject_unref(status);
    qtest_quit(source);
    qtest_quit(destination);
    g_rmdir(tmpdir);
}

static void test_count_mismatch_rejected(void)
{
    /* Given: an ESPI subsection produced for 1024 implemented ESPIs. */
    GICv3State source = { .num_espi = GICV3_MAX_ESPI };
    GICv3State destination = { .num_espi = 32 };

    /* When: a destination with a different configured count loads it. */
    int ret = vmstate_round_trip(&vmstate_gicv3_espi, &source, &destination);

    /* Then: the equal-count migration guard rejects the stream. */
    g_assert_cmpint(ret, !=, 0);
}

static void test_reset_clears_extension_state(void)
{
    /* Given: nonzero ESPI and EPPI state left by a previous VMState. */
    GICv3State state = {
        .num_cpu = 1,
        .num_espi = 32,
        .num_eppi = 32,
        .cpu = g_new0(GICv3CPUState, 1),
    };

    memset(state.espi_pending, 0xff, sizeof(state.espi_pending));
    memset(state.espi_priority, 0xff, sizeof(state.espi_priority));
    memset(state.cpu[0].eppi_pending, 0xff,
           sizeof(state.cpu[0].eppi_pending));
    memset(state.cpu[0].eppi_priority, 0xff,
           sizeof(state.cpu[0].eppi_priority));

    /* When: reset or a base-only pre-load clears optional state. */
    gicv3_ext_range_reset(&state);

    /* Then: no stale extension bit or priority remains. */
    g_assert_cmphex(state.espi_pending[0], ==, 0);
    g_assert_cmphex(state.espi_priority[0], ==, 0);
    g_assert_cmphex(state.cpu[0].eppi_pending[0], ==, 0);
    g_assert_cmphex(state.cpu[0].eppi_priority[0], ==, 0);
    g_free(state.cpu);
}

static void test_hole_intids_rejected(void)
{
    /* Given: maximal valid ranges and a literal list of architected holes. */
    g_autofree char *path = fixture_path(
        "arm-gicv3-ext-range-invalid-hole.json");
    g_autofree char *json = NULL;
    GICv3State state = {
        .num_cpu = 2,
        .num_irq = 992,
        .num_espi = GICV3_MAX_ESPI,
        .num_eppi = GICV3_MAX_EPPI,
    };
    QList *holes;
    QListEntry *entry;
    GICv3IRQ irq;
    GICv3State before = state;

    g_assert_true(g_file_get_contents(path, &json, NULL, NULL));
    holes = qobject_to(QList, qobject_from_json(json, &error_abort));

    /* When: every fixture INTID is passed through the checked mapper. */
    QLIST_FOREACH_ENTRY(holes, entry) {
        QDict *hole = qobject_to(QDict, qlist_entry_obj(entry));

        /* Then: reserved holes and invalid CPU ownership are rejected. */
        g_assert_cmpstr(qdict_get_str(hole, "expected"), ==, "reject");
        g_assert_false(gicv3_intid_to_irq(&state,
                                         qdict_get_int(hole, "intid"),
                                         qdict_get_int(hole, "cpu"), &irq));
        g_assert_cmpmem(&state, sizeof(state), &before, sizeof(before));
    }
    qobject_unref(holes);

    g_assert_true(gicv3_intid_to_irq(&state, 991, 0, &irq));
    g_assert_cmpint(irq.type, ==, GICV3_IRQ_SPI);
    g_assert_true(gicv3_intid_to_irq(&state, 4096, 0, &irq));
    g_assert_cmpint(irq.type, ==, GICV3_IRQ_ESPI);
    g_assert_true(gicv3_intid_to_irq(&state, 5119, 0, &irq));
    g_assert_cmpint(irq.type, ==, GICV3_IRQ_ESPI);
    g_assert_true(gicv3_intid_to_irq(&state, 16, 0, &irq));
    g_assert_cmpint(irq.type, ==, GICV3_IRQ_PPI);
    g_assert_cmpuint(irq.cpu, ==, 0);
    g_assert_true(gicv3_intid_to_irq(&state, 31, 1, &irq));
    g_assert_cmpint(irq.type, ==, GICV3_IRQ_PPI);
    g_assert_cmpuint(irq.cpu, ==, 1);
    g_assert_true(gicv3_intid_to_irq(&state, 1056, 0, &irq));
    g_assert_cmpint(irq.type, ==, GICV3_IRQ_EPPI);
    g_assert_cmpuint(irq.cpu, ==, 0);
    g_assert_true(gicv3_intid_to_irq(&state, 1119, 1, &irq));
    g_assert_cmpint(irq.type, ==, GICV3_IRQ_EPPI);
    g_assert_cmpuint(irq.cpu, ==, 1);
}

static void test_gpio_mapper_boundaries(void)
{
    /* Given: maximum ranges on two CPUs and their checked ABI size. */
    GICv3State state = {
        .num_cpu = 2,
        .num_irq = 992,
        .num_espi = GICV3_MAX_ESPI,
        .num_eppi = GICV3_MAX_EPPI,
    };
    GICv3State overflow = state;
    GICv3IRQ irq;
    int count;
    const struct {
        uint32_t gpio;
        GICv3IRQType type;
        uint32_t intid;
        uint32_t cpu;
    } cases[] = {
        { 0, GICV3_IRQ_SPI, 32, UINT32_MAX },
        { 959, GICV3_IRQ_SPI, 991, UINT32_MAX },
        { 960, GICV3_IRQ_ESPI, 4096, UINT32_MAX },
        { 1983, GICV3_IRQ_ESPI, 5119, UINT32_MAX },
        { 1984, GICV3_IRQ_PPI, 0, 0 },
        { 2000, GICV3_IRQ_PPI, 16, 0 },
        { 2015, GICV3_IRQ_PPI, 31, 0 },
        { 2016, GICV3_IRQ_PPI, 0, 1 },
        { 2032, GICV3_IRQ_PPI, 16, 1 },
        { 2047, GICV3_IRQ_PPI, 31, 1 },
        { 2048, GICV3_IRQ_EPPI, 1056, 0 },
        { 2111, GICV3_IRQ_EPPI, 1119, 0 },
        { 2112, GICV3_IRQ_EPPI, 1056, 1 },
        { 2175, GICV3_IRQ_EPPI, 1119, 1 },
    };

    /* When: the four family boundaries are decoded from GPIO indices. */
    g_assert_true(gicv3_gpio_count(&state, &count));
    g_assert_cmpint(count, ==, 2176);
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_true(gicv3_gpio_to_irq(&state, cases[i].gpio, &irq));
        g_assert_cmpint(irq.type, ==, cases[i].type);
        g_assert_cmpuint(irq.intid, ==, cases[i].intid);
        g_assert_cmpuint(irq.cpu, ==, cases[i].cpu);
    }

    /* Then: the last EPPI is CPU1 INTID1119 and overflow fails closed. */
    g_assert_false(gicv3_gpio_to_irq(&state, count, &irq));
    overflow.num_cpu = UINT32_MAX;
    g_assert_false(gicv3_gpio_count(&overflow, &count));
}

static void test_production_gpio_handler_boundaries(void)
{
    /* Given: maximum ranges on two CPUs with zero family-local state. */
    GICv3State state = {
        .num_cpu = 2,
        .num_irq = 992,
        .num_espi = GICV3_MAX_ESPI,
        .num_eppi = GICV3_MAX_EPPI,
        .cpu = g_new0(GICv3CPUState, 2),
    };

    state.cpu[0].gic = &state;
    state.cpu[1].gic = &state;

    /* When: the production handler receives every valid family boundary. */
    gicv3_set_irq(&state, 0, 1);
    g_assert_true(observed_dist_state == &state);
    g_assert_cmpint(observed_irq, ==, 32);
    g_assert_cmpint(observed_level, ==, 1);
    gicv3_set_irq(&state, 959, 1);
    g_assert_cmpint(observed_irq, ==, 991);

    gicv3_set_irq(&state, 960, 1);
    gicv3_set_irq(&state, 1983, 1);
    g_assert_true(test_bit32(0, state.espi_level));
    g_assert_true(test_bit32(1023, state.espi_level));

    gicv3_set_irq(&state, 2000, 1);
    g_assert_true(observed_redist_cpu == &state.cpu[0]);
    g_assert_cmpint(observed_irq, ==, 16);
    gicv3_set_irq(&state, 2015, 1);
    g_assert_cmpint(observed_irq, ==, 31);
    gicv3_set_irq(&state, 2032, 1);
    g_assert_true(observed_redist_cpu == &state.cpu[1]);
    g_assert_cmpint(observed_irq, ==, 16);
    gicv3_set_irq(&state, 2047, 1);
    g_assert_cmpint(observed_irq, ==, 31);

    gicv3_set_irq(&state, 2048, 1);
    gicv3_set_irq(&state, 2111, 1);
    gicv3_set_irq(&state, 2112, 1);
    gicv3_set_irq(&state, 2175, 1);
    g_assert_true(test_bit32(0, state.cpu[0].eppi_level));
    g_assert_true(test_bit32(63, state.cpu[0].eppi_level));
    g_assert_true(test_bit32(0, state.cpu[1].eppi_level));
    g_assert_true(test_bit32(63, state.cpu[1].eppi_level));

    /* Then: deassertion clears the same ESPI and EPPI family-local bits. */
    gicv3_set_irq(&state, 960, 0);
    gicv3_set_irq(&state, 1983, 0);
    gicv3_set_irq(&state, 2048, 0);
    gicv3_set_irq(&state, 2111, 0);
    gicv3_set_irq(&state, 2112, 0);
    gicv3_set_irq(&state, 2175, 0);
    g_assert_false(test_bit32(0, state.espi_level));
    g_assert_false(test_bit32(1023, state.espi_level));
    g_assert_false(test_bit32(0, state.cpu[0].eppi_level));
    g_assert_false(test_bit32(63, state.cpu[0].eppi_level));
    g_assert_false(test_bit32(0, state.cpu[1].eppi_level));
    g_assert_false(test_bit32(63, state.cpu[1].eppi_level));
    g_free(state.cpu);
}

static void test_old_v1_fixture_loads(void)
{
    /* Given: a literal full-machine stream from the pre-extension v1 model. */
    g_autofree char *hex_path = fixture_path("arm-gicv3-ext-range-v1.hex");
    g_autofree char *hex = NULL;
    g_autoptr(GByteArray) migration = g_byte_array_new();
    g_autofree char *binary_path = NULL;
    int high_nibble = -1;
    int fd;

    g_assert_true(g_file_get_contents(hex_path, &hex, NULL, NULL));
    for (const char *cursor = hex; *cursor; cursor++) {
        int nibble;

        if (g_ascii_isspace(*cursor)) {
            continue;
        }
        nibble = g_ascii_xdigit_value(*cursor);
        g_assert_cmpint(nibble, >=, 0);
        if (high_nibble < 0) {
            high_nibble = nibble;
        } else {
            uint8_t byte = high_nibble << 4 | nibble;

            g_byte_array_append(migration, &byte, 1);
            high_nibble = -1;
        }
    }
    g_assert_cmpint(high_nibble, ==, -1);
    g_assert_cmpuint(migration->len, ==, 398411);
    fd = g_file_open_tmp("arm-gicv3-old-v1-XXXXXX", &binary_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(binary_path,
                                      (const char *)migration->data,
                                      migration->len, NULL));

    /* When: the current TCG binary starts an extended destination from it. */
    const char *qemu = qtest_qemu_binary(NULL);
    const char *argv[] = {
        qemu,
        "-machine", "virt,gic-version=3",
        "-global", "arm-gicv3.num-espi=1024",
        "-global", "arm-gicv3.num-eppi=64",
        "-m", "16M",
        "-nodefaults",
        "-display", "none",
        "-accel", "tcg",
        "-incoming", NULL,
        "-qmp", "stdio",
        NULL,
    };
    g_autofree char *incoming = g_strdup_printf("file:%s", binary_path);
    g_autofree char *response = NULL;
    GPid pid;
    int input_fd;
    int output_fd;
    int error_fd;
    int wait_status;
    FILE *input;
    FILE *output;
    char *greeting = NULL;
    size_t greeting_capacity = 0;

    argv[15] = incoming;
    g_assert_true(g_spawn_async_with_pipes(NULL, (char **)argv, NULL,
                                          G_SPAWN_DO_NOT_REAP_CHILD,
                                          NULL, NULL, &pid, &input_fd,
                                          &output_fd, &error_fd, NULL));
    input = fdopen(input_fd, "w");
    output = fdopen(output_fd, "r");
    g_assert_nonnull(input);
    g_assert_nonnull(output);
    g_assert_cmpint(getline(&greeting, &greeting_capacity, output), >, 0);
    g_assert_nonnull(strstr(greeting, "\"QMP\""));
    g_free(greeting);
    fputs("{\"execute\":\"qmp_capabilities\"}\n", input);
    fflush(input);
    response = qmp_read_until_response(output);
    g_assert_nonnull(response);
    g_assert_nonnull(strstr(response, "\"return\""));
    g_clear_pointer(&response, g_free);
    fputs("{\"execute\":\"query-status\"}\n", input);
    fflush(input);
    response = qmp_read_until_response(output);

    /* Then: base v1 loads without requiring either extension subsection. */
    g_assert_nonnull(response);
    g_assert_nonnull(strstr(response, "\"return\""));
    fputs("{\"execute\":\"quit\"}\n", input);
    fflush(input);
    fclose(input);
    fclose(output);
    close(error_fd);
    g_assert_cmpint(waitpid(pid, &wait_status, 0), ==, pid);
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), ==, 0);
    g_spawn_close_pid(pid);
    g_unlink(binary_path);
}

int main(int argc, char **argv)
{
    module_call_init(MODULE_INIT_QOM);
    g_setenv("QTEST_SILENT_ERRORS", "1", true);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/pin/base-round-trip",
                   test_base_state_round_trip);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/espi/round-trip",
                   test_espi_state_round_trip);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/eppi/round-trip",
                   test_eppi_state_round_trip);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/full/round-trip",
                   test_extended_full_state_round_trip);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/count-mismatch",
                   test_count_mismatch_rejected);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/reset",
                   test_reset_clears_extension_state);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/hole-intids",
                   test_hole_intids_rejected);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/gpio-mapper",
                   test_gpio_mapper_boundaries);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/gpio-handler",
                   test_production_gpio_handler_boundaries);
    qtest_add_func("/arm-gicv3-ext-range-vmstate/old-v1-fixture",
                   test_old_v1_fixture_loads);
    return g_test_run();
}
