#ifndef URACIL_PLATFORM_H
#define URACIL_PLATFORM_H

#include <stdio.h>

#include "uracil/common.h"

/* State directory management */
ur_status ur_platform_state_dir(char *buffer, size_t buffer_size);
ur_status ur_platform_permission_manifest_path(
    const char *service_name,
    char *buffer,
    size_t buffer_size
);
ur_status ur_platform_ensure_state_dirs(void);

/* Filesystem operations */
ur_status ur_platform_list_directory(const char *path, FILE *stream);
ur_status ur_platform_read_file(const char *path, void **out_data, size_t *out_size);
ur_status ur_platform_write_file(const char *path, const void *data, size_t size);
ur_status ur_platform_remove_file(const char *path);
ur_status ur_platform_create_directory(const char *path);

/* Time operations */
ur_status ur_platform_get_time(double *out_timestamp);
ur_status ur_platform_sleep_ms(uint32_t ms);

/* Network operations */
ur_status ur_platform_network_request(
    const char *method,
    const char *url,
    const char *headers,
    const void *body,
    size_t body_len,
    char *response_buf,
    size_t response_buf_len,
    size_t *out_response_len
);

/* Random number generation */
ur_status ur_platform_random_bytes(void *buf, size_t len);

/* Environment variables */
ur_status ur_platform_get_env(const char *name, char *buf, size_t buf_len);
ur_status ur_platform_set_env(const char *name, const char *value);

/* Process management */
ur_status ur_platform_spawn_process(const char *command, char *const argv[], const char *envp[], int *out_pid);
ur_status ur_platform_wait_process(int pid, int *out_status);
ur_status ur_platform_kill_process(int pid, int signal);

/* Path utilities */
const char *ur_platform_get_temp_dir(void);
const char *ur_platform_get_cwd(void);
ur_status ur_platform_set_cwd(const char *path);

/* Lifecycle */
ur_status ur_platform_init(void);
void ur_platform_cleanup(void);

#endif
