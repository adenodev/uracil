/*
 * Uracil – Campaign kernel unit tests.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "uracil/campaign.h"

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

static char g_home_dir[512];

static void write_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "w");
    ASSERT(file != NULL);
    ASSERT(fputs(content, file) >= 0);
    fclose(file);
}

static void remove_recursive(const char *path)
{
    struct stat stat_buffer;

    if (lstat(path, &stat_buffer) != 0) {
        return;
    }

    if (S_ISDIR(stat_buffer.st_mode)) {
        DIR *directory = opendir(path);
        struct dirent *entry;
        ASSERT(directory != NULL);

        while ((entry = readdir(directory)) != NULL) {
            char child[512];
            if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
                continue;
            }
            ASSERT(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) > 0);
            remove_recursive(child);
        }

        closedir(directory);
        ASSERT((rmdir(path) == 0) || (errno == ENOENT));
        return;
    }

    ASSERT((unlink(path) == 0) || (errno == ENOENT));
}

static void setup_test_home(void)
{
    char template_path[] = "/tmp/uracil-campaign-test-XXXXXX";
    char *created = mkdtemp(template_path);
    ASSERT(created != NULL);
    ASSERT(setenv("HOME", created, 1) == 0);
    snprintf(g_home_dir, sizeof(g_home_dir), "%s", created);
}

static void teardown_test_home(void)
{
    if (g_home_dir[0] != '\0') {
        remove_recursive(g_home_dir);
        g_home_dir[0] = '\0';
    }
}

TEST(test_campaign_phase_roundtrip)
{
    ASSERT(strcmp(ur_campaign_phase_name(UR_CAMPAIGN_PHASE_BOOTSTRAP), "bootstrap") == 0);
    ASSERT(ur_campaign_phase_from_name("running") == UR_CAMPAIGN_PHASE_RUNNING);
    ASSERT(ur_campaign_phase_from_name("missing") == UR_CAMPAIGN_PHASE_COUNT);
}

TEST(test_campaign_boot_and_reload)
{
    ur_campaign_kernel campaign;
    ur_campaign_kernel loaded;
    char artifacts_path[UR_CAMPAIGN_MAX_PATH];

    ASSERT(ur_campaign_kernel_boot(
        "demo-campaign",
        "keryx",
        "keryx-ami",
        UR_CAMPAIGN_KERNEL_ABI,
        "adeno-cauth",
        &campaign) == UR_OK);

    ASSERT(campaign.phase == UR_CAMPAIGN_PHASE_BOOTSTRAP);
    ASSERT(ur_campaign_kernel_artifacts_path("demo-campaign", artifacts_path, sizeof(artifacts_path)) == UR_OK);
    ASSERT(access(artifacts_path, F_OK) == 0);

    ASSERT(ur_campaign_kernel_load("demo-campaign", &loaded) == UR_OK);
    ASSERT(strcmp(loaded.owner_service, "keryx") == 0);
    ASSERT(strcmp(loaded.target_env, "keryx-ami") == 0);
    ASSERT(strcmp(loaded.security_profile, "adeno-cauth") == 0);
}

TEST(test_campaign_attach_cleanup_and_complete)
{
    ur_campaign_kernel campaign;
    ur_campaign_kernel loaded;
    char artifact_path[UR_CAMPAIGN_MAX_PATH];
    char artifacts_dir[UR_CAMPAIGN_MAX_PATH];
    size_t removed = 0U;

    ASSERT(ur_campaign_kernel_boot(
        "cleanup-campaign",
        "keryx",
        "keryx-ami",
        UR_CAMPAIGN_KERNEL_ABI,
        "adeno-cauth",
        &campaign) == UR_OK);

    ASSERT(ur_campaign_kernel_artifacts_path("cleanup-campaign", artifacts_dir, sizeof(artifacts_dir)) == UR_OK);
    ASSERT(snprintf(artifact_path, sizeof(artifact_path), "%s/%s", artifacts_dir, "writer.ura") > 0);
    write_file(artifact_path, "binary");

    ASSERT(ur_campaign_kernel_attach_artifact(&campaign, artifact_path) == UR_OK);
    ASSERT(campaign.artifact_count == 1U);
    ASSERT(access(artifact_path, F_OK) == 0);

    ASSERT(ur_campaign_kernel_transition(&campaign, UR_CAMPAIGN_PHASE_RUNNING) == UR_OK);
    ASSERT(campaign.phase == UR_CAMPAIGN_PHASE_RUNNING);

    ASSERT(ur_campaign_kernel_complete(&campaign, stdout, &removed) == UR_OK);
    ASSERT(removed == 1U);
    ASSERT(campaign.phase == UR_CAMPAIGN_PHASE_COMPLETED);
    ASSERT(campaign.artifact_count == 0U);
    ASSERT(access(artifact_path, F_OK) != 0);

    ASSERT(ur_campaign_kernel_load("cleanup-campaign", &loaded) == UR_OK);
    ASSERT(loaded.phase == UR_CAMPAIGN_PHASE_COMPLETED);
    ASSERT(loaded.artifact_count == 0U);
}

TEST(test_campaign_destroy_removes_root)
{
    ur_campaign_kernel campaign;
    char root_path[UR_CAMPAIGN_MAX_PATH];
    size_t removed = 0U;

    ASSERT(ur_campaign_kernel_boot(
        "destroy-campaign",
        "keryx",
        "keryx-ami",
        UR_CAMPAIGN_KERNEL_ABI,
        "adeno-cauth",
        &campaign) == UR_OK);

    ASSERT(ur_campaign_kernel_root_path("destroy-campaign", root_path, sizeof(root_path)) == UR_OK);
    ASSERT(access(root_path, F_OK) == 0);

    ASSERT(ur_campaign_kernel_destroy(&campaign, true, stdout, &removed) == UR_OK);
    ASSERT(access(root_path, F_OK) != 0);
    ASSERT(ur_campaign_kernel_load("destroy-campaign", &campaign) == UR_NOT_FOUND);
}

int main(void)
{
    printf("Campaign kernel tests:\n");
    setup_test_home();

    RUN(test_campaign_phase_roundtrip);
    RUN(test_campaign_boot_and_reload);
    RUN(test_campaign_attach_cleanup_and_complete);
    RUN(test_campaign_destroy_removes_root);

    teardown_test_home();

    printf("\n%d/%d tests passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}