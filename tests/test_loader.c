/*
 * Uracil – Bundle loader tests.
 *
 * Uses the example manifest in examples/ to verify that the loader
 * correctly parses a Uracil service manifest.
 *
 * Set URACIL_TEST_MANIFEST_DIR to the examples/ directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uracil/common.h"
#include "uracil/loader.h"
#include "uracil/permissions.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) static void name(const char *examples_dir)
#define RUN(name)                                                   \
    do {                                                            \
        ++g_tests_run;                                              \
        printf("  %-48s ", #name);                                  \
        name(examples_dir);                                         \
        ++g_tests_passed;                                           \
        printf("PASS\n");                                           \
    } while (0)

#define ASSERT(condition)                                           \
    do {                                                            \
        if (!(condition)) {                                         \
            printf("FAIL\n    assertion failed: %s\n    at %s:%d\n",\
                   #condition, __FILE__, __LINE__);                 \
            exit(1);                                                \
        }                                                           \
    } while (0)

/* ── Tests ────────────────────────────────────────────────────────────── */

TEST(test_load_example_bundle)
{
    ur_loaded_bundle *loaded = NULL;
    ur_status status = ur_loader_load_bundle(examples_dir, &loaded);
    ASSERT(status == UR_OK);
    ASSERT(loaded != NULL);
    ASSERT(strcmp(loaded->bundle.service_name, "aps.sample.echo-service") == 0);
    ASSERT(strcmp(loaded->bundle.service_version, "0.1.0") == 0);
    ur_loader_free_bundle(loaded);
}

TEST(test_loaded_runtime_fields)
{
    ur_loaded_bundle *loaded = NULL;
    ur_status status = ur_loader_load_bundle(examples_dir, &loaded);
    ASSERT(status == UR_OK);
    ASSERT(strcmp(loaded->bundle.runtime_abi, "uracil.service.v1") == 0);
    ASSERT(strcmp(loaded->bundle.hotswap_group, "aps.sample.echo-service") == 0);
    ASSERT(strcmp(loaded->bundle.payload_revision, "demo-001") == 0);
    ur_loader_free_bundle(loaded);
}

TEST(test_loaded_permissions)
{
    ur_loaded_bundle *loaded = NULL;
    ur_status status = ur_loader_load_bundle(examples_dir, &loaded);
    ASSERT(status == UR_OK);

    ur_permission_mask expected =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT) |
        UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ);
    ASSERT(loaded->bundle.requested_permissions == expected);

    ur_loader_free_bundle(loaded);
}

TEST(test_loaded_functions)
{
    ur_loaded_bundle *loaded = NULL;
    ur_status status = ur_loader_load_bundle(examples_dir, &loaded);
    ASSERT(status == UR_OK);
    ASSERT(loaded->bundle.function_count == 4);

    const ur_function_descriptor *health = ur_bundle_find_function(
        &loaded->bundle, "health");
    ASSERT(health != NULL);
    ASSERT(health->required_permissions == UR_PERMISSION_MASK_NONE);
    ASSERT(health->entry == NULL); /* external function */

    const ur_function_descriptor *clock_fn = ur_bundle_find_function(
        &loaded->bundle, "clock-now");
    ASSERT(clock_fn != NULL);
    ASSERT(clock_fn->required_permissions == UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ));

    ur_loader_free_bundle(loaded);
}

TEST(test_load_nonexistent_fails)
{
    ur_loaded_bundle *loaded = NULL;
    ur_status status;
    UR_UNUSED(examples_dir);
    status = ur_loader_load_bundle("/nonexistent/path", &loaded);
    ASSERT(status != UR_OK);
    ASSERT(loaded == NULL);
}

TEST(test_load_null_fails)
{
    ur_loaded_bundle *loaded = NULL;
    UR_UNUSED(examples_dir);
    ASSERT(ur_loader_load_bundle(NULL, &loaded) == UR_INVALID_ARGUMENT);
    ASSERT(ur_loader_load_bundle("/tmp", NULL) == UR_INVALID_ARGUMENT);
}

/* ── Runner ───────────────────────────────────────────────────────────── */

int main(void)
{
    const char *examples_dir = getenv("URACIL_TEST_MANIFEST_DIR");

    if (examples_dir == NULL) {
        fprintf(stderr, "URACIL_TEST_MANIFEST_DIR not set, skipping loader tests\n");
        return 0;
    }

    printf("Bundle loader tests (dir=%s):\n", examples_dir);

    RUN(test_load_example_bundle);
    RUN(test_loaded_runtime_fields);
    RUN(test_loaded_permissions);
    RUN(test_loaded_functions);
    RUN(test_load_nonexistent_fails);
    RUN(test_load_null_fails);

    printf("\n%d/%d tests passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
