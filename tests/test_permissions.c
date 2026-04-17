/*
 * Uracil – Permission model unit tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uracil/permissions.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name)                                                   \
    do {                                                            \
        ++g_tests_run;                                              \
        printf("  %-48s ", #name);                                  \
        name();                                                     \
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

TEST(test_permission_name_roundtrip)
{
    int index;
    for (index = 0; index < UR_PERMISSION_COUNT; ++index) {
        const char *name = ur_permission_name((ur_permission_id)index);
        ASSERT(name != NULL);
        ASSERT(ur_permission_from_name(name) == (ur_permission_id)index);
    }
}

TEST(test_permission_unknown_name)
{
    ASSERT(ur_permission_from_name("does.not.exist") == UR_PERMISSION_COUNT);
    ASSERT(ur_permission_from_name(NULL) == UR_PERMISSION_COUNT);
}

TEST(test_permission_name_out_of_bounds)
{
    ASSERT(ur_permission_name(UR_PERMISSION_COUNT) == NULL);
    ASSERT(ur_permission_name((ur_permission_id)99) == NULL);
}

TEST(test_permission_mask_operations)
{
    ur_permission_mask mask =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ);

    ASSERT(ur_permission_mask_contains(mask, UR_PERMISSION_MASK(UR_PERMISSION_FS_READ)));
    ASSERT(ur_permission_mask_contains(mask, UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ)));
    ASSERT(!ur_permission_mask_contains(mask, UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT)));

    /* Subset check */
    ur_permission_mask required =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ);
    ASSERT(ur_permission_mask_contains(mask, required));

    ur_permission_mask too_much =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_NET_SERVER);
    ASSERT(!ur_permission_mask_contains(mask, too_much));
}

TEST(test_permission_mask_none)
{
    ASSERT(ur_permission_mask_contains(UR_PERMISSION_MASK_NONE, UR_PERMISSION_MASK_NONE));
    ASSERT(!ur_permission_mask_contains(UR_PERMISSION_MASK_NONE, UR_PERMISSION_MASK(UR_PERMISSION_FS_READ)));
}

TEST(test_permission_mask_format)
{
    char buffer[256];

    ur_permission_mask_format(UR_PERMISSION_MASK_NONE, buffer, sizeof(buffer));
    ASSERT(strcmp(buffer, "(none)") == 0);

    ur_permission_mask_format(UR_PERMISSION_MASK(UR_PERMISSION_FS_READ), buffer, sizeof(buffer));
    ASSERT(strcmp(buffer, "fs.read") == 0);

    ur_permission_mask mask =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ);
    ur_permission_mask_format(mask, buffer, sizeof(buffer));
    ASSERT(strstr(buffer, "fs.read") != NULL);
    ASSERT(strstr(buffer, "clock.read") != NULL);
}

TEST(test_permission_format_small_buffer)
{
    char buffer[8];
    ur_permission_mask mask =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT) |
        UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ);

    /* Should not crash, just truncate */
    ur_permission_mask_format(mask, buffer, sizeof(buffer));
    ASSERT(strlen(buffer) < sizeof(buffer));
}

TEST(test_all_permission_names)
{
    ASSERT(strcmp(ur_permission_name(UR_PERMISSION_FS_READ), "fs.read") == 0);
    ASSERT(strcmp(ur_permission_name(UR_PERMISSION_FS_WRITE), "fs.write") == 0);
    ASSERT(strcmp(ur_permission_name(UR_PERMISSION_NET_CLIENT), "net.client") == 0);
    ASSERT(strcmp(ur_permission_name(UR_PERMISSION_NET_SERVER), "net.server") == 0);
    ASSERT(strcmp(ur_permission_name(UR_PERMISSION_PROC_SPAWN), "proc.spawn") == 0);
    ASSERT(strcmp(ur_permission_name(UR_PERMISSION_CLOCK_READ), "clock.read") == 0);
}

/* ── Runner ───────────────────────────────────────────────────────────── */

int main(void)
{
    printf("Permission model tests:\n");

    RUN(test_permission_name_roundtrip);
    RUN(test_permission_unknown_name);
    RUN(test_permission_name_out_of_bounds);
    RUN(test_permission_mask_operations);
    RUN(test_permission_mask_none);
    RUN(test_permission_mask_format);
    RUN(test_permission_format_small_buffer);
    RUN(test_all_permission_names);

    printf("\n%d/%d tests passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
