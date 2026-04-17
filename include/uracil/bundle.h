#ifndef URACIL_BUNDLE_H
#define URACIL_BUNDLE_H

#include "uracil/common.h"
#include "uracil/permissions.h"

struct ur_service_context;

typedef int (*ur_function_entry)(const struct ur_service_context *context);

typedef struct ur_function_descriptor {
    const char *name;
    const char *description;
    ur_permission_mask required_permissions;
    ur_function_entry entry;
} ur_function_descriptor;

typedef struct ur_service_bundle {
    const char *service_name;
    const char *service_version;
    const char *description;
    const char *runtime_abi;
    const char *hotswap_group;
    const char *payload_revision;
    ur_permission_mask requested_permissions;
    size_t function_count;
    const ur_function_descriptor *functions;
} ur_service_bundle;

const ur_function_descriptor *ur_bundle_find_function(
    const ur_service_bundle *bundle,
    const char *function_name
);

#endif
