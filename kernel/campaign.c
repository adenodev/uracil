#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "uracil/campaign.h"
#include "uracil/platform.h"

#define UR_CAMPAIGN_STATE_FILE "kernel.state"

static void ur_campaign_safe_copy(char *dst, const char *src, size_t size)
{
    size_t length = 0U;

    if ((dst == NULL) || (size == 0U)) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    length = strlen(src);
    if (length + 1U > size) {
        length = size - 1U;
    }

    memcpy(dst, src, length);
    dst[length] = '\0';
}

static uint64_t ur_campaign_now(void)
{
    return (uint64_t)time(NULL);
}

static void ur_campaign_sanitize_id(const char *campaign_id, char *buffer, size_t buffer_size)
{
    size_t index = 0U;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }

    if (campaign_id == NULL) {
        buffer[0] = '\0';
        return;
    }

    while ((*campaign_id != '\0') && (index + 1U < buffer_size)) {
        const char current = *campaign_id++;

        if (((current >= 'a') && (current <= 'z')) ||
            ((current >= 'A') && (current <= 'Z')) ||
            ((current >= '0') && (current <= '9')) ||
            (current == '-') ||
            (current == '_')) {
            buffer[index++] = current;
        } else {
            buffer[index++] = '-';
        }
    }

    buffer[index] = '\0';
}

static ur_status ur_campaign_make_directory(const char *path)
{
    if (mkdir(path, 0755) == 0) {
        return UR_OK;
    }

    if (errno == EEXIST) {
        return UR_OK;
    }

    return UR_IO_ERROR;
}

static ur_status ur_campaigns_root_path(char *buffer, size_t buffer_size)
{
    char state_dir[512];
    int written;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_platform_state_dir(state_dir, sizeof(state_dir)) != UR_OK) {
        return UR_IO_ERROR;
    }

    written = snprintf(buffer, buffer_size, "%s/campaigns", state_dir);
    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

ur_status ur_campaign_kernel_root_path(
    const char *campaign_id,
    char *buffer,
    size_t buffer_size
)
{
    char campaigns_root[512];
    char sanitized_id[UR_CAMPAIGN_MAX_ID];
    int written;

    if ((campaign_id == NULL) || (buffer == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_campaigns_root_path(campaigns_root, sizeof(campaigns_root)) != UR_OK) {
        return UR_IO_ERROR;
    }

    ur_campaign_sanitize_id(campaign_id, sanitized_id, sizeof(sanitized_id));
    if (sanitized_id[0] == '\0') {
        return UR_INVALID_ARGUMENT;
    }

    written = snprintf(buffer, buffer_size, "%s/%s", campaigns_root, sanitized_id);
    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

static ur_status ur_campaign_kernel_state_path(
    const char *campaign_id,
    char *buffer,
    size_t buffer_size
)
{
    char root_path[UR_CAMPAIGN_MAX_PATH];
    int written;

    if (ur_campaign_kernel_root_path(campaign_id, root_path, sizeof(root_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    written = snprintf(buffer, buffer_size, "%s/%s", root_path, UR_CAMPAIGN_STATE_FILE);
    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

ur_status ur_campaign_kernel_artifacts_path(
    const char *campaign_id,
    char *buffer,
    size_t buffer_size
)
{
    char root_path[UR_CAMPAIGN_MAX_PATH];
    int written;

    if (ur_campaign_kernel_root_path(campaign_id, root_path, sizeof(root_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    written = snprintf(buffer, buffer_size, "%s/artifacts", root_path);
    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

static ur_status ur_campaign_kernel_ensure_dirs(const char *campaign_id)
{
    char campaigns_root[512];
    char root_path[UR_CAMPAIGN_MAX_PATH];
    char artifacts_path[UR_CAMPAIGN_MAX_PATH];

    if (ur_platform_ensure_state_dirs() != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_campaigns_root_path(campaigns_root, sizeof(campaigns_root)) != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_campaign_make_directory(campaigns_root) != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_campaign_kernel_root_path(campaign_id, root_path, sizeof(root_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_campaign_make_directory(root_path) != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_campaign_kernel_artifacts_path(campaign_id, artifacts_path, sizeof(artifacts_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    return ur_campaign_make_directory(artifacts_path);
}

const char *ur_campaign_phase_name(ur_campaign_phase phase)
{
    switch (phase) {
        case UR_CAMPAIGN_PHASE_BOOTSTRAP:
            return "bootstrap";
        case UR_CAMPAIGN_PHASE_RUNNING:
            return "running";
        case UR_CAMPAIGN_PHASE_COMPLETED:
            return "completed";
        case UR_CAMPAIGN_PHASE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

ur_campaign_phase ur_campaign_phase_from_name(const char *name)
{
    if (name == NULL) {
        return UR_CAMPAIGN_PHASE_COUNT;
    }

    if (strcmp(name, "bootstrap") == 0) {
        return UR_CAMPAIGN_PHASE_BOOTSTRAP;
    }
    if (strcmp(name, "running") == 0) {
        return UR_CAMPAIGN_PHASE_RUNNING;
    }
    if (strcmp(name, "completed") == 0) {
        return UR_CAMPAIGN_PHASE_COMPLETED;
    }
    if (strcmp(name, "failed") == 0) {
        return UR_CAMPAIGN_PHASE_FAILED;
    }

    return UR_CAMPAIGN_PHASE_COUNT;
}

ur_status ur_campaign_kernel_store(const ur_campaign_kernel *campaign)
{
    char state_path[UR_CAMPAIGN_MAX_PATH];
    FILE *state_file;
    size_t index;

    if ((campaign == NULL) || (campaign->campaign_id[0] == '\0')) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_campaign_kernel_ensure_dirs(campaign->campaign_id) != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_campaign_kernel_state_path(campaign->campaign_id, state_path, sizeof(state_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    state_file = fopen(state_path, "w");
    if (state_file == NULL) {
        return UR_IO_ERROR;
    }

    fprintf(state_file, "campaign_id %s\n", campaign->campaign_id);
    fprintf(state_file, "owner_service %s\n", campaign->owner_service);
    fprintf(state_file, "target_env %s\n", campaign->target_env);
    fprintf(state_file, "runtime_abi %s\n", campaign->runtime_abi);
    fprintf(state_file, "security_profile %s\n", campaign->security_profile);
    fprintf(state_file, "phase %s\n", ur_campaign_phase_name(campaign->phase));
    fprintf(state_file, "created_at %llu\n", (unsigned long long)campaign->created_at);
    fprintf(state_file, "updated_at %llu\n", (unsigned long long)campaign->updated_at);
    fprintf(state_file, "completed_at %llu\n", (unsigned long long)campaign->completed_at);

    for (index = 0U; index < campaign->artifact_count; ++index) {
        fprintf(state_file, "artifact %s\n", campaign->artifacts[index]);
    }

    fclose(state_file);
    return UR_OK;
}

ur_status ur_campaign_kernel_boot(
    const char *campaign_id,
    const char *owner_service,
    const char *target_env,
    const char *runtime_abi,
    const char *security_profile,
    ur_campaign_kernel *out_campaign
)
{
    ur_campaign_kernel campaign;

    if ((campaign_id == NULL) || (owner_service == NULL) || (out_campaign == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    memset(&campaign, 0, sizeof(campaign));
    ur_campaign_safe_copy(campaign.campaign_id, campaign_id, sizeof(campaign.campaign_id));
    ur_campaign_safe_copy(campaign.owner_service, owner_service, sizeof(campaign.owner_service));
    ur_campaign_safe_copy(campaign.target_env, target_env != NULL ? target_env : "generic", sizeof(campaign.target_env));
    ur_campaign_safe_copy(campaign.runtime_abi, runtime_abi != NULL ? runtime_abi : UR_CAMPAIGN_KERNEL_ABI, sizeof(campaign.runtime_abi));
    ur_campaign_safe_copy(campaign.security_profile, security_profile != NULL ? security_profile : "none", sizeof(campaign.security_profile));
    campaign.phase = UR_CAMPAIGN_PHASE_BOOTSTRAP;
    campaign.created_at = ur_campaign_now();
    campaign.updated_at = campaign.created_at;

    if (ur_campaign_kernel_store(&campaign) != UR_OK) {
        return UR_IO_ERROR;
    }

    *out_campaign = campaign;
    return UR_OK;
}

ur_status ur_campaign_kernel_load(
    const char *campaign_id,
    ur_campaign_kernel *out_campaign
)
{
    char state_path[UR_CAMPAIGN_MAX_PATH];
    FILE *state_file;
    char line[1024];
    ur_campaign_kernel campaign;

    if ((campaign_id == NULL) || (out_campaign == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_campaign_kernel_state_path(campaign_id, state_path, sizeof(state_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    state_file = fopen(state_path, "r");
    if (state_file == NULL) {
        return (errno == ENOENT) ? UR_NOT_FOUND : UR_IO_ERROR;
    }

    memset(&campaign, 0, sizeof(campaign));
    campaign.phase = UR_CAMPAIGN_PHASE_BOOTSTRAP;

    while (fgets(line, sizeof(line), state_file) != NULL) {
        char *value;
        char *key = line;

        line[strcspn(line, "\r\n")] = '\0';
        if ((line[0] == '\0') || (line[0] == '#')) {
            continue;
        }

        value = strchr(line, ' ');
        if (value == NULL) {
            continue;
        }

        *value = '\0';
        ++value;

        if (strcmp(key, "campaign_id") == 0) {
            ur_campaign_safe_copy(campaign.campaign_id, value, sizeof(campaign.campaign_id));
        } else if (strcmp(key, "owner_service") == 0) {
            ur_campaign_safe_copy(campaign.owner_service, value, sizeof(campaign.owner_service));
        } else if (strcmp(key, "target_env") == 0) {
            ur_campaign_safe_copy(campaign.target_env, value, sizeof(campaign.target_env));
        } else if (strcmp(key, "runtime_abi") == 0) {
            ur_campaign_safe_copy(campaign.runtime_abi, value, sizeof(campaign.runtime_abi));
        } else if (strcmp(key, "security_profile") == 0) {
            ur_campaign_safe_copy(campaign.security_profile, value, sizeof(campaign.security_profile));
        } else if (strcmp(key, "phase") == 0) {
            campaign.phase = ur_campaign_phase_from_name(value);
        } else if (strcmp(key, "created_at") == 0) {
            campaign.created_at = (uint64_t)strtoull(value, NULL, 10);
        } else if (strcmp(key, "updated_at") == 0) {
            campaign.updated_at = (uint64_t)strtoull(value, NULL, 10);
        } else if (strcmp(key, "completed_at") == 0) {
            campaign.completed_at = (uint64_t)strtoull(value, NULL, 10);
        } else if ((strcmp(key, "artifact") == 0) && (campaign.artifact_count < UR_CAMPAIGN_MAX_ARTIFACTS)) {
            ur_campaign_safe_copy(
                campaign.artifacts[campaign.artifact_count],
                value,
                sizeof(campaign.artifacts[campaign.artifact_count])
            );
            campaign.artifact_count++;
        }
    }

    fclose(state_file);

    if (campaign.phase >= UR_CAMPAIGN_PHASE_COUNT) {
        return UR_ERROR;
    }

    *out_campaign = campaign;
    return UR_OK;
}

ur_status ur_campaign_kernel_transition(
    ur_campaign_kernel *campaign,
    ur_campaign_phase phase
)
{
    if ((campaign == NULL) || (phase >= UR_CAMPAIGN_PHASE_COUNT)) {
        return UR_INVALID_ARGUMENT;
    }

    campaign->phase = phase;
    campaign->updated_at = ur_campaign_now();
    if ((phase == UR_CAMPAIGN_PHASE_COMPLETED) || (phase == UR_CAMPAIGN_PHASE_FAILED)) {
        campaign->completed_at = campaign->updated_at;
    }

    return ur_campaign_kernel_store(campaign);
}

static ur_status ur_campaign_kernel_normalize_artifact(
    const ur_campaign_kernel *campaign,
    const char *artifact_path,
    char *buffer,
    size_t buffer_size
)
{
    char root_path[UR_CAMPAIGN_MAX_PATH];
    int written;

    if ((campaign == NULL) || (artifact_path == NULL) || (buffer == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (artifact_path[0] == '/') {
        ur_campaign_safe_copy(buffer, artifact_path, buffer_size);
        return UR_OK;
    }

    if (ur_campaign_kernel_root_path(campaign->campaign_id, root_path, sizeof(root_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    written = snprintf(buffer, buffer_size, "%s/%s", root_path, artifact_path);
    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

ur_status ur_campaign_kernel_attach_artifact(
    ur_campaign_kernel *campaign,
    const char *artifact_path
)
{
    char normalized[UR_CAMPAIGN_MAX_PATH];
    size_t index;

    if ((campaign == NULL) || (artifact_path == NULL) || (artifact_path[0] == '\0')) {
        return UR_INVALID_ARGUMENT;
    }

    if (campaign->artifact_count >= UR_CAMPAIGN_MAX_ARTIFACTS) {
        return UR_ERROR;
    }

    if (ur_campaign_kernel_normalize_artifact(campaign, artifact_path, normalized, sizeof(normalized)) != UR_OK) {
        return UR_IO_ERROR;
    }

    for (index = 0U; index < campaign->artifact_count; ++index) {
        if (strcmp(campaign->artifacts[index], normalized) == 0) {
            return UR_OK;
        }
    }

    ur_campaign_safe_copy(
        campaign->artifacts[campaign->artifact_count],
        normalized,
        sizeof(campaign->artifacts[campaign->artifact_count])
    );
    campaign->artifact_count++;
    campaign->updated_at = ur_campaign_now();

    return ur_campaign_kernel_store(campaign);
}

static ur_status ur_campaign_remove_path_recursive(const char *path)
{
    struct stat stat_buffer;

    if (lstat(path, &stat_buffer) != 0) {
        return (errno == ENOENT) ? UR_OK : UR_IO_ERROR;
    }

    if (S_ISDIR(stat_buffer.st_mode)) {
        DIR *directory = opendir(path);
        struct dirent *entry;

        if (directory == NULL) {
            return UR_IO_ERROR;
        }

        while ((entry = readdir(directory)) != NULL) {
            char child_path[UR_CAMPAIGN_MAX_PATH];
            int written;

            if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
                continue;
            }

            written = snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
            if ((written < 0) || ((size_t)written >= sizeof(child_path))) {
                closedir(directory);
                return UR_IO_ERROR;
            }

            if (ur_campaign_remove_path_recursive(child_path) != UR_OK) {
                closedir(directory);
                return UR_IO_ERROR;
            }
        }

        closedir(directory);
        return (rmdir(path) == 0) ? UR_OK : UR_IO_ERROR;
    }

    return (unlink(path) == 0) ? UR_OK : UR_IO_ERROR;
}

ur_status ur_campaign_kernel_cleanup_artifacts(
    ur_campaign_kernel *campaign,
    FILE *stream,
    size_t *out_removed
)
{
    size_t removed = 0U;
    size_t index;

    if ((campaign == NULL) || (stream == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    for (index = 0U; index < campaign->artifact_count; ++index) {
        if (campaign->artifacts[index][0] == '\0') {
            continue;
        }

        if (ur_campaign_remove_path_recursive(campaign->artifacts[index]) == UR_OK) {
            fprintf(stream, "cleaned artifact: %s\n", campaign->artifacts[index]);
            removed++;
        }
    }

    campaign->artifact_count = 0U;
    campaign->updated_at = ur_campaign_now();

    if (out_removed != NULL) {
        *out_removed = removed;
    }

    return ur_campaign_kernel_store(campaign);
}

ur_status ur_campaign_kernel_complete(
    ur_campaign_kernel *campaign,
    FILE *stream,
    size_t *out_removed
)
{
    ur_status status;

    status = ur_campaign_kernel_cleanup_artifacts(campaign, stream, out_removed);
    if (status != UR_OK) {
        return status;
    }

    return ur_campaign_kernel_transition(campaign, UR_CAMPAIGN_PHASE_COMPLETED);
}

ur_status ur_campaign_kernel_destroy(
    ur_campaign_kernel *campaign,
    bool cleanup_artifacts,
    FILE *stream,
    size_t *out_removed
)
{
    char state_path[UR_CAMPAIGN_MAX_PATH];
    char artifacts_path[UR_CAMPAIGN_MAX_PATH];
    char root_path[UR_CAMPAIGN_MAX_PATH];

    if ((campaign == NULL) || (stream == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (cleanup_artifacts) {
        if (ur_campaign_kernel_cleanup_artifacts(campaign, stream, out_removed) != UR_OK) {
            return UR_IO_ERROR;
        }
    } else if (out_removed != NULL) {
        *out_removed = 0U;
    }

    if (ur_campaign_kernel_state_path(campaign->campaign_id, state_path, sizeof(state_path)) != UR_OK) {
        return UR_IO_ERROR;
    }
    if (ur_campaign_kernel_artifacts_path(campaign->campaign_id, artifacts_path, sizeof(artifacts_path)) != UR_OK) {
        return UR_IO_ERROR;
    }
    if (ur_campaign_kernel_root_path(campaign->campaign_id, root_path, sizeof(root_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    if (unlink(state_path) != 0) {
        if (errno != ENOENT) {
            return UR_IO_ERROR;
        }
    }

    if (rmdir(artifacts_path) != 0) {
        if (errno != ENOENT) {
            return UR_IO_ERROR;
        }
    }

    if (rmdir(root_path) != 0) {
        if (errno != ENOENT) {
            return UR_IO_ERROR;
        }
    }

    return UR_OK;
}

ur_status ur_campaign_kernel_print(FILE *stream, const ur_campaign_kernel *campaign)
{
    char root_path[UR_CAMPAIGN_MAX_PATH];
    char artifacts_path[UR_CAMPAIGN_MAX_PATH];
    size_t index;

    if ((stream == NULL) || (campaign == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_campaign_kernel_root_path(campaign->campaign_id, root_path, sizeof(root_path)) != UR_OK) {
        return UR_IO_ERROR;
    }
    if (ur_campaign_kernel_artifacts_path(campaign->campaign_id, artifacts_path, sizeof(artifacts_path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    fprintf(stream, "campaign:         %s\n", campaign->campaign_id);
    fprintf(stream, "owner service:    %s\n", campaign->owner_service);
    fprintf(stream, "phase:            %s\n", ur_campaign_phase_name(campaign->phase));
    fprintf(stream, "target env:       %s\n", campaign->target_env);
    fprintf(stream, "runtime abi:      %s\n", campaign->runtime_abi);
    fprintf(stream, "security profile: %s\n", campaign->security_profile);
    fprintf(stream, "created at:       %llu\n", (unsigned long long)campaign->created_at);
    fprintf(stream, "updated at:       %llu\n", (unsigned long long)campaign->updated_at);
    fprintf(stream, "completed at:     %llu\n", (unsigned long long)campaign->completed_at);
    fprintf(stream, "root path:        %s\n", root_path);
    fprintf(stream, "artifacts path:   %s\n", artifacts_path);
    fprintf(stream, "artifacts:        %zu\n", campaign->artifact_count);
    for (index = 0U; index < campaign->artifact_count; ++index) {
        fprintf(stream, "  - %s\n", campaign->artifacts[index]);
    }

    return UR_OK;
}