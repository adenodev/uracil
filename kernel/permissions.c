#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uracil/permissions.h"
#include "uracil/platform.h"

static const char *const ur_permission_names[UR_PERMISSION_COUNT] = {
    [UR_PERMISSION_FS_READ] = "fs.read",
    [UR_PERMISSION_FS_WRITE] = "fs.write",
    [UR_PERMISSION_NET_CLIENT] = "net.client",
    [UR_PERMISSION_NET_SERVER] = "net.server",
    [UR_PERMISSION_PROC_SPAWN] = "proc.spawn",
    [UR_PERMISSION_CLOCK_READ] = "clock.read"
};

static void ur_permission_trim(char *value)
{
    char *start = value;
    char *end;

    while ((*start != '\0') && isspace((unsigned char)*start)) {
        ++start;
    }

    if (start != value) {
        memmove(value, start, strlen(start) + 1U);
    }

    end = value + strlen(value);
    while ((end > value) && isspace((unsigned char)end[-1])) {
        --end;
    }

    *end = '\0';
}

const char *ur_permission_name(ur_permission_id permission_id)
{
    if ((permission_id < 0) || (permission_id >= UR_PERMISSION_COUNT)) {
        return NULL;
    }

    return ur_permission_names[permission_id];
}

ur_permission_id ur_permission_from_name(const char *name)
{
    int index;

    if (name == NULL) {
        return UR_PERMISSION_COUNT;
    }

    for (index = 0; index < UR_PERMISSION_COUNT; ++index) {
        if (strcmp(name, ur_permission_names[index]) == 0) {
            return (ur_permission_id)index;
        }
    }

    return UR_PERMISSION_COUNT;
}

bool ur_permission_mask_contains(ur_permission_mask granted, ur_permission_mask required)
{
    return (granted & required) == required;
}

void ur_permission_mask_format(ur_permission_mask mask, char *buffer, size_t buffer_size)
{
    size_t index;
    size_t used = 0U;
    bool first = true;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }

    buffer[0] = '\0';

    if (mask == UR_PERMISSION_MASK_NONE) {
        snprintf(buffer, buffer_size, "(none)");
        return;
    }

    for (index = 0U; index < UR_PERMISSION_COUNT; ++index) {
        const ur_permission_mask bit = UR_PERMISSION_MASK(index);
        const char *name = ur_permission_names[index];
        int written;

        if ((mask & bit) == 0U) {
            continue;
        }

        written = snprintf(
            buffer + used,
            buffer_size - used,
            "%s%s",
            first ? "" : ", ",
            name
        );

        if ((written < 0) || ((size_t)written >= (buffer_size - used))) {
            break;
        }

        used += (size_t)written;
        first = false;
    }
}

ur_status ur_permission_load_grants(
    const char *service_name,
    ur_permission_mask requested,
    ur_permission_mask *out_grants
)
{
    char path[512];
    FILE *file;
    char line[128];
    ur_permission_mask grants = UR_PERMISSION_MASK_NONE;

    if ((service_name == NULL) || (out_grants == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_platform_ensure_state_dirs() != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_platform_permission_manifest_path(service_name, path, sizeof(path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        *out_grants = UR_PERMISSION_MASK_NONE;
        return UR_OK;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        ur_permission_id permission_id;

        ur_permission_trim(line);
        permission_id = ur_permission_from_name(line);

        if (permission_id >= UR_PERMISSION_COUNT) {
            continue;
        }

        if ((requested & UR_PERMISSION_MASK(permission_id)) == 0U) {
            continue;
        }

        grants |= UR_PERMISSION_MASK(permission_id);
    }

    fclose(file);
    *out_grants = grants;
    return UR_OK;
}

ur_status ur_permission_save_grants(
    const char *service_name,
    ur_permission_mask requested,
    ur_permission_mask grants
)
{
    char path[512];
    FILE *file;
    size_t index;

    if (service_name == NULL) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_platform_ensure_state_dirs() != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_platform_permission_manifest_path(service_name, path, sizeof(path)) != UR_OK) {
        return UR_IO_ERROR;
    }

    file = fopen(path, "w");
    if (file == NULL) {
        return UR_IO_ERROR;
    }

    for (index = 0U; index < UR_PERMISSION_COUNT; ++index) {
        const ur_permission_mask bit = UR_PERMISSION_MASK(index);

        if (((requested & bit) == 0U) || ((grants & bit) == 0U)) {
            continue;
        }

        fprintf(file, "%s\n", ur_permission_names[index]);
    }

    fclose(file);
    return UR_OK;
}
