#include <stdio.h>
#include <string.h>

#include "uracil/platform.h"
#include "uracil/runtime.h"

static ur_status ur_runtime_load_grants(
    const ur_service_bundle *bundle,
    ur_permission_mask *out_grants
)
{
    return ur_permission_load_grants(bundle->service_name, bundle->requested_permissions, out_grants);
}

static ur_status ur_runtime_resolve_permissions(
    FILE *stream,
    const ur_service_bundle *bundle,
    int permission_count,
    const char *const permission_names[],
    ur_permission_mask *out_mask
)
{
    int index;
    ur_permission_mask mask = UR_PERMISSION_MASK_NONE;

    if ((bundle == NULL) || (permission_names == NULL) || (out_mask == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    for (index = 0; index < permission_count; ++index) {
        const ur_permission_id permission_id = ur_permission_from_name(permission_names[index]);
        const ur_permission_mask bit = UR_PERMISSION_MASK(permission_id);

        if (permission_id >= UR_PERMISSION_COUNT) {
            fprintf(stream, "unknown permission: %s\n", permission_names[index]);
            return UR_NOT_FOUND;
        }

        if ((bundle->requested_permissions & bit) == 0U) {
            fprintf(stream,
                "permission '%s' is not declared by service '%s'\n",
                permission_names[index],
                bundle->service_name
            );
            return UR_INVALID_ARGUMENT;
        }

        mask |= bit;
    }

    *out_mask = mask;
    return UR_OK;
}

const char *ur_status_string(ur_status status)
{
    switch (status) {
        case UR_OK:
            return "ok";
        case UR_ERROR:
            return "error";
        case UR_NOT_FOUND:
            return "not-found";
        case UR_PERMISSION_DENIED:
            return "permission-denied";
        case UR_INVALID_ARGUMENT:
            return "invalid-argument";
        case UR_IO_ERROR:
            return "io-error";
        default:
            return "unknown";
    }
}

ur_status ur_runtime_print_bundle(FILE *stream, const ur_service_bundle *bundle)
{
    char requested[256];
    size_t index;

    if ((stream == NULL) || (bundle == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    ur_permission_mask_format(bundle->requested_permissions, requested, sizeof(requested));

    fprintf(stream, "service:         %s\n", bundle->service_name);
    fprintf(stream, "version:         %s\n", bundle->service_version);
    fprintf(stream, "description:     %s\n", bundle->description);
    fprintf(stream, "runtime abi:     %s\n", bundle->runtime_abi);
    fprintf(stream, "hot-swap group:  %s\n", bundle->hotswap_group);
    fprintf(stream, "payload rev:     %s\n", bundle->payload_revision);
    fprintf(stream, "rootless:        true\n");
    fprintf(stream, "requested perms: %s\n", requested);
    fprintf(stream, "functions:\n");

    for (index = 0U; index < bundle->function_count; ++index) {
        char required[256];
        const ur_function_descriptor *descriptor = &bundle->functions[index];

        ur_permission_mask_format(descriptor->required_permissions, required, sizeof(required));
        fprintf(stream, "  - %s\n", descriptor->name);
        fprintf(stream, "    description: %s\n", descriptor->description);
        fprintf(stream, "    requires:    %s\n", required);
    }

    return UR_OK;
}

ur_status ur_runtime_print_permissions(FILE *stream, const ur_service_bundle *bundle)
{
    char requested[256];
    char granted[256];
    char withheld[256];
    ur_permission_mask grant_mask = UR_PERMISSION_MASK_NONE;
    ur_status status;

    if ((stream == NULL) || (bundle == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    status = ur_runtime_load_grants(bundle, &grant_mask);
    if (status != UR_OK) {
        return status;
    }

    ur_permission_mask_format(bundle->requested_permissions, requested, sizeof(requested));
    ur_permission_mask_format(grant_mask, granted, sizeof(granted));
    ur_permission_mask_format(bundle->requested_permissions & ~grant_mask, withheld, sizeof(withheld));

    fprintf(stream, "service:   %s\n", bundle->service_name);
    fprintf(stream, "requested: %s\n", requested);
    fprintf(stream, "granted:   %s\n", granted);
    fprintf(stream, "withheld:  %s\n", withheld);
    return UR_OK;
}

ur_status ur_runtime_grant_permissions(
    FILE *stream,
    const ur_service_bundle *bundle,
    int permission_count,
    const char *const permission_names[]
)
{
    ur_permission_mask grant_mask = UR_PERMISSION_MASK_NONE;
    ur_permission_mask update_mask = UR_PERMISSION_MASK_NONE;
    char formatted[256];
    ur_status status;

    status = ur_runtime_load_grants(bundle, &grant_mask);
    if (status != UR_OK) {
        return status;
    }

    status = ur_runtime_resolve_permissions(stream, bundle, permission_count, permission_names, &update_mask);
    if (status != UR_OK) {
        return status;
    }

    grant_mask |= update_mask;
    status = ur_permission_save_grants(bundle->service_name, bundle->requested_permissions, grant_mask);
    if (status != UR_OK) {
        return status;
    }

    ur_permission_mask_format(grant_mask, formatted, sizeof(formatted));
    fprintf(stream, "updated grant manifest: %s\n", formatted);
    return UR_OK;
}

ur_status ur_runtime_revoke_permissions(
    FILE *stream,
    const ur_service_bundle *bundle,
    int permission_count,
    const char *const permission_names[]
)
{
    ur_permission_mask grant_mask = UR_PERMISSION_MASK_NONE;
    ur_permission_mask update_mask = UR_PERMISSION_MASK_NONE;
    char formatted[256];
    ur_status status;

    status = ur_runtime_load_grants(bundle, &grant_mask);
    if (status != UR_OK) {
        return status;
    }

    status = ur_runtime_resolve_permissions(stream, bundle, permission_count, permission_names, &update_mask);
    if (status != UR_OK) {
        return status;
    }

    grant_mask &= ~update_mask;
    status = ur_permission_save_grants(bundle->service_name, bundle->requested_permissions, grant_mask);
    if (status != UR_OK) {
        return status;
    }

    ur_permission_mask_format(grant_mask, formatted, sizeof(formatted));
    fprintf(stream, "updated grant manifest: %s\n", formatted);
    return UR_OK;
}

ur_status ur_runtime_run(
    FILE *stream,
    const ur_service_bundle *bundle,
    const char *function_name
)
{
    const ur_function_descriptor *descriptor;
    ur_permission_mask grants = UR_PERMISSION_MASK_NONE;
    ur_permission_mask missing;
    ur_service_context context;
    char missing_buffer[256];
    char state_directory[512];
    ur_status status;

    if ((stream == NULL) || (bundle == NULL) || (function_name == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    descriptor = ur_bundle_find_function(bundle, function_name);
    if (descriptor == NULL) {
        fprintf(stream, "unknown function: %s\n", function_name);
        return UR_NOT_FOUND;
    }

    status = ur_runtime_load_grants(bundle, &grants);
    if (status != UR_OK) {
        return status;
    }

    missing = descriptor->required_permissions & ~grants;
    if (missing != UR_PERMISSION_MASK_NONE) {
        ur_permission_mask_format(missing, missing_buffer, sizeof(missing_buffer));
        fprintf(stream,
            "refusing to run '%s'; missing permissions: %s\n",
            descriptor->name,
            missing_buffer
        );
        return UR_PERMISSION_DENIED;
    }

    if (ur_platform_state_dir(state_directory, sizeof(state_directory)) != UR_OK) {
        return UR_IO_ERROR;
    }

    fprintf(stream, "running '%s' inside rootless Uracil context\n", descriptor->name);

    context.bundle = bundle;
    context.granted_permissions = grants;
    context.state_directory = state_directory;
    context.rootless = true;
    context.out = stream;

    return descriptor->entry(&context) == 0 ? UR_OK : UR_ERROR;
}
