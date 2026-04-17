#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "uracil/platform.h"

static const char *ur_platform_home_directory(void)
{
    const char *home = getenv("HOME");

    if ((home == NULL) || (home[0] == '\0')) {
        return ".";
    }

    return home;
}

static void ur_platform_sanitize_service_name(const char *service_name, char *buffer, size_t buffer_size)
{
    size_t index = 0U;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }

    while ((service_name != NULL) && (*service_name != '\0') && (index + 1U < buffer_size)) {
        const char current = *service_name++;

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

static ur_status ur_platform_make_directory(const char *path)
{
    if (mkdir(path, 0755) == 0) {
        return UR_OK;
    }

    if (errno == EEXIST) {
        return UR_OK;
    }

    return UR_IO_ERROR;
}

ur_status ur_platform_state_dir(char *buffer, size_t buffer_size)
{
    const int written = snprintf(buffer, buffer_size, "%s/.uracil", ur_platform_home_directory());

    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

ur_status ur_platform_permission_manifest_path(
    const char *service_name,
    char *buffer,
    size_t buffer_size
)
{
    char state_dir[512];
    char sanitized_name[256];
    int written;

    if ((service_name == NULL) || (buffer == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_platform_state_dir(state_dir, sizeof(state_dir)) != UR_OK) {
        return UR_IO_ERROR;
    }

    ur_platform_sanitize_service_name(service_name, sanitized_name, sizeof(sanitized_name));
    written = snprintf(buffer, buffer_size, "%s/grants/%s.perm", state_dir, sanitized_name);

    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

ur_status ur_platform_ensure_state_dirs(void)
{
    char state_dir[512];
    char grants_dir[512];
    int written;

    if (ur_platform_state_dir(state_dir, sizeof(state_dir)) != UR_OK) {
        return UR_IO_ERROR;
    }

    if (ur_platform_make_directory(state_dir) != UR_OK) {
        return UR_IO_ERROR;
    }

    written = snprintf(grants_dir, sizeof(grants_dir), "%s/grants", state_dir);
    if ((written < 0) || ((size_t)written >= sizeof(grants_dir))) {
        return UR_IO_ERROR;
    }

    return ur_platform_make_directory(grants_dir);
}

ur_status ur_platform_list_directory(const char *path, FILE *stream)
{
    DIR *directory;
    struct dirent *entry;

    if ((path == NULL) || (stream == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    directory = opendir(path);
    if (directory == NULL) {
        fprintf(stream, "unable to open directory '%s': %s\n", path, strerror(errno));
        return UR_IO_ERROR;
    }

    while ((entry = readdir(directory)) != NULL) {
        if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }

        fprintf(stream, "- %s\n", entry->d_name);
    }

    closedir(directory);
    return UR_OK;
}
