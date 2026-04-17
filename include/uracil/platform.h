#ifndef URACIL_PLATFORM_H
#define URACIL_PLATFORM_H

#include <stdio.h>

#include "uracil/common.h"

ur_status ur_platform_state_dir(char *buffer, size_t buffer_size);
ur_status ur_platform_permission_manifest_path(
    const char *service_name,
    char *buffer,
    size_t buffer_size
);
ur_status ur_platform_ensure_state_dirs(void);
ur_status ur_platform_list_directory(const char *path, FILE *stream);

#endif
