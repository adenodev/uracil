#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "uracil/platform.h"

static const char *ur_platform_home_directory(void)
{
    const char *home = getenv("HOME");

    if ((home == NULL) || (home[0] == '\0')) {
        return ".";
    }

    return home;
}

static const char *ur_platform_configured_state_dir(void)
{
    const char *state_dir = getenv("URACIL_STATE_DIR");

    if ((state_dir == NULL) || (state_dir[0] == '\0')) {
        return NULL;
    }

    return state_dir;
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
    const char *configured_state_dir = ur_platform_configured_state_dir();
    const int written = snprintf(
        buffer,
        buffer_size,
        "%s",
        configured_state_dir != NULL ? configured_state_dir : ""
    );

    if (configured_state_dir != NULL) {
        if ((written < 0) || ((size_t)written >= buffer_size)) {
            return UR_IO_ERROR;
        }

        return UR_OK;
    }

    if (snprintf(buffer, buffer_size, "%s/.uracil", ur_platform_home_directory()) < 0) {
        return UR_IO_ERROR;
    }

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

/* Filesystem operations */
ur_status ur_platform_read_file(const char *path, void **out_data, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return UR_IO_ERROR;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return UR_IO_ERROR;
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return UR_IO_ERROR;
    }

    rewind(file);

    void *buffer = malloc((size_t)file_size + 1);
    if (buffer == NULL) {
        fclose(file);
        return UR_OUT_OF_MEMORY;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);

    if (bytes_read != (size_t)file_size) {
        free(buffer);
        return UR_IO_ERROR;
    }

    ((char *)buffer)[bytes_read] = '\0';
    *out_data = buffer;
    *out_size = bytes_read;
    return UR_OK;
}

ur_status ur_platform_write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return UR_IO_ERROR;
    }

    size_t written = fwrite(data, 1, size, file);
    fclose(file);

    return (written == size) ? UR_OK : UR_IO_ERROR;
}

ur_status ur_platform_remove_file(const char *path)
{
    if (remove(path) == 0) {
        return UR_OK;
    }
    return UR_IO_ERROR;
}

ur_status ur_platform_create_directory(const char *path)
{
    return ur_platform_make_directory(path);
}

/* Time operations */
ur_status ur_platform_get_time(double *out_timestamp)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        *out_timestamp = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        return UR_OK;
    }
    return UR_ERROR;
}

ur_status ur_platform_sleep_ms(uint32_t ms)
{
    struct timespec req = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000
    };
    nanosleep(&req, NULL);
    return UR_OK;
}

/* Network operations - stub for host */
ur_status ur_platform_network_request(
    const char *method,
    const char *url,
    const char *headers,
    const void *body,
    size_t body_len,
    char *response_buf,
    size_t response_buf_len,
    size_t *out_response_len
)
{
    (void)method;
    (void)url;
    (void)headers;
    (void)body;
    (void)body_len;
    (void)response_buf;
    (void)response_buf_len;
    (void)out_response_len;
    return UR_NOT_SUPPORTED;
}

/* Random number generation */
ur_status ur_platform_random_bytes(void *buf, size_t len)
{
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom == NULL) {
        return UR_IO_ERROR;
    }
    size_t read = fread(buf, 1, len, urandom);
    fclose(urandom);
    return (read == len) ? UR_OK : UR_IO_ERROR;
}

/* Environment variables */
ur_status ur_platform_get_env(const char *name, char *buf, size_t buf_len)
{
    const char *value = getenv(name);
    if (value == NULL) {
        return UR_NOT_FOUND;
    }
    size_t len = strlen(value);
    if (len >= buf_len) {
        return UR_INVALID_ARGUMENT;
    }
    strcpy(buf, value);
    return UR_OK;
}

ur_status ur_platform_set_env(const char *name, const char *value)
{
    if (setenv(name, value, 1) == 0) {
        return UR_OK;
    }
    return UR_ERROR;
}

/* Process management - stub for host */
ur_status ur_platform_spawn_process(const char *command, char *const argv[], const char *envp[], int *out_pid)
{
    (void)command;
    (void)argv;
    (void)envp;
    (void)out_pid;
    return UR_NOT_SUPPORTED;
}

ur_status ur_platform_wait_process(int pid, int *out_status)
{
    (void)pid;
    (void)out_status;
    return UR_NOT_SUPPORTED;
}

ur_status ur_platform_kill_process(int pid, int signal)
{
    (void)pid;
    (void)signal;
    return UR_NOT_SUPPORTED;
}

/* Path utilities */
const char *ur_platform_get_temp_dir(void)
{
    return "/tmp";
}

const char *ur_platform_get_cwd(void)
{
    static char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        return cwd;
    }
    return ".";
}

ur_status ur_platform_set_cwd(const char *path)
{
    if (chdir(path) == 0) {
        return UR_OK;
    }
    return UR_IO_ERROR;
}

/* Lifecycle */
ur_status ur_platform_init(void)
{
    return UR_OK;
}

void ur_platform_cleanup(void)
{
    /* Nothing to clean up */
}
