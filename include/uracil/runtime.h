#ifndef URACIL_RUNTIME_H
#define URACIL_RUNTIME_H

#include <stdio.h>

#include "uracil/bundle.h"

typedef struct ur_service_context {
    const ur_service_bundle *bundle;
    ur_permission_mask granted_permissions;
    const char *state_directory;
    bool rootless;
    FILE *out;
} ur_service_context;

const char *ur_status_string(ur_status status);

ur_status ur_runtime_print_bundle(FILE *stream, const ur_service_bundle *bundle);
ur_status ur_runtime_print_permissions(FILE *stream, const ur_service_bundle *bundle);

ur_status ur_runtime_grant_permissions(
    FILE *stream,
    const ur_service_bundle *bundle,
    int permission_count,
    const char *const permission_names[]
);

ur_status ur_runtime_revoke_permissions(
    FILE *stream,
    const ur_service_bundle *bundle,
    int permission_count,
    const char *const permission_names[]
);

ur_status ur_runtime_run(
    FILE *stream,
    const ur_service_bundle *bundle,
    const char *function_name
);

#endif
