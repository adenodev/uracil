#include <stdio.h>
#include <string.h>

#include "demo_bundle.h"
#include "uracil/loader.h"
#include "uracil/platform.h"
#include "uracil/process.h"
#include "uracil/runtime.h"

#define URACIL_VERSION "0.2.0"

static void ur_print_usage(FILE *stream, const char *program_name)
{
    fprintf(stream,
        "Uracil runtime v%s\n\n"
        "usage:\n"
        "  %s describe\n"
        "  %s permissions\n"
        "  %s grant <permission> [permission...]\n"
        "  %s revoke <permission> [permission...]\n"
        "  %s run <function> [args...]\n"
        "\nexternal bundles:\n"
        "  %s --bundle <dir> describe\n"
        "  %s --bundle <dir> run <function> [args...]\n"
        "  %s --bundle <dir> grant <permission> [permission...]\n"
        "  %s --bundle <dir> revoke <permission> [permission...]\n"
        "  %s --bundle <dir> permissions\n"
        "\nflags:\n"
        "  %s --version\n"
        "  %s --help\n"
        "\nexamples:\n"
        "  %s describe\n"
        "  %s grant fs.read clock.read\n"
        "  %s run inspect-fs\n"
        "  %s --bundle ~/.aps/packages/weather-check-service run current-weather London\n",
        URACIL_VERSION,
        program_name, program_name, program_name, program_name,
        program_name, program_name, program_name, program_name,
        program_name, program_name, program_name, program_name,
        program_name, program_name, program_name, program_name
    );
}

static int ur_dispatch_permission_update(
    const ur_service_bundle *bundle,
    int argc,
    char **argv,
    int arg_offset,
    bool grant
)
{
    int perm_start = arg_offset + 1;
    int perm_count = argc - perm_start;

    if (perm_count < 1) {
        fprintf(stderr, "permission list is required\n");
        return 1;
    }

    return (grant
        ? ur_runtime_grant_permissions(stderr, bundle, perm_count, (const char *const *)&argv[perm_start])
        : ur_runtime_revoke_permissions(stderr, bundle, perm_count, (const char *const *)&argv[perm_start])) == UR_OK
        ? 0
        : 1;
}

static int ur_dispatch_run_external(
    const ur_loaded_bundle *loaded,
    const char *function_name,
    int argc,
    const char *const argv[]
)
{
    const ur_function_descriptor *descriptor;
    ur_permission_mask grants = UR_PERMISSION_MASK_NONE;
    ur_permission_mask missing;
    ur_service_context context;
    char missing_buffer[256];
    char state_directory[512];
    ur_status status;

    descriptor = ur_bundle_find_function(&loaded->bundle, function_name);
    if (descriptor == NULL) {
        fprintf(stderr, "unknown function: %s\n", function_name);
        return 1;
    }

    status = ur_permission_load_grants(
        loaded->bundle.service_name,
        loaded->bundle.requested_permissions,
        &grants);
    if (status != UR_OK) {
        fprintf(stderr, "failed to load grants: %s\n", ur_status_string(status));
        return 1;
    }

    missing = descriptor->required_permissions & ~grants;
    if (missing != UR_PERMISSION_MASK_NONE) {
        ur_permission_mask_format(missing, missing_buffer, sizeof(missing_buffer));
        fprintf(stderr,
            "refusing to run '%s'; missing permissions: %s\n",
            descriptor->name, missing_buffer);
        return 1;
    }

    if (ur_platform_state_dir(state_directory, sizeof(state_directory)) != UR_OK) {
        fprintf(stderr, "failed to resolve state directory\n");
        return 1;
    }

    context.bundle = &loaded->bundle;
    context.granted_permissions = grants;
    context.state_directory = state_directory;
    context.rootless = true;
    context.out = stdout;

    fprintf(stderr, "running '%s' inside rootless Uracil context\n", function_name);

    status = ur_process_execute_function(
        &context, loaded, function_name, argc, argv, stdout);

    return (status == UR_OK) ? 0 : 1;
}

static int ur_run_external_bundle(int argc, char **argv, const char *bundle_dir)
{
    ur_loaded_bundle *loaded = NULL;
    const char *command;
    ur_status status;
    int result = 1;

    status = ur_loader_load_bundle(bundle_dir, &loaded);
    if (status != UR_OK) {
        fprintf(stderr, "failed to load bundle from '%s': %s\n",
            bundle_dir, ur_status_string(status));
        return 1;
    }

    if (argc < 1) {
        fprintf(stderr, "command required after --bundle <dir>\n");
        ur_loader_free_bundle(loaded);
        return 1;
    }

    command = argv[0];

    if (strcmp(command, "describe") == 0) {
        result = ur_runtime_print_bundle(stdout, &loaded->bundle) == UR_OK ? 0 : 1;
    } else if (strcmp(command, "permissions") == 0) {
        result = ur_runtime_print_permissions(stdout, &loaded->bundle) == UR_OK ? 0 : 1;
    } else if (strcmp(command, "grant") == 0) {
        result = ur_dispatch_permission_update(
            &loaded->bundle, argc, argv, 0, true);
    } else if (strcmp(command, "revoke") == 0) {
        result = ur_dispatch_permission_update(
            &loaded->bundle, argc, argv, 0, false);
    } else if (strcmp(command, "run") == 0) {
        if (argc < 2) {
            fprintf(stderr, "function name is required\n");
            result = 1;
        } else {
            result = ur_dispatch_run_external(
                loaded, argv[1],
                argc - 2, (const char *const *)&argv[2]);
        }
    } else {
        fprintf(stderr, "unknown command: %s\n", command);
        result = 1;
    }

    ur_loader_free_bundle(loaded);
    return result;
}

int main(int argc, char **argv)
{
    const ur_service_bundle *bundle = ur_demo_bundle();
    const char *command;

    if (argc < 2) {
        ur_print_usage(stdout, argv[0]);
        return 0;
    }

    command = argv[1];

    if ((strcmp(command, "help") == 0) || (strcmp(command, "--help") == 0) || (strcmp(command, "-h") == 0)) {
        ur_print_usage(stdout, argv[0]);
        return 0;
    }

    if ((strcmp(command, "version") == 0) || (strcmp(command, "--version") == 0)) {
        printf("uracil %s\n", URACIL_VERSION);
        return 0;
    }

    /* External bundle mode */
    if (strcmp(command, "--bundle") == 0) {
        if (argc < 3) {
            fprintf(stderr, "--bundle requires a directory path\n");
            return 1;
        }
        return ur_run_external_bundle(argc - 3, argv + 3, argv[2]);
    }

    if ((strcmp(command, "describe") == 0) || (strcmp(command, "--describe") == 0)) {
        return ur_runtime_print_bundle(stdout, bundle) == UR_OK ? 0 : 1;
    }

    if ((strcmp(command, "permissions") == 0) || (strcmp(command, "--permissions") == 0)) {
        return ur_runtime_print_permissions(stdout, bundle) == UR_OK ? 0 : 1;
    }

    if ((strcmp(command, "grant") == 0) || (strcmp(command, "--grant") == 0)) {
        return ur_dispatch_permission_update(bundle, argc, argv, 1, true);
    }

    if ((strcmp(command, "revoke") == 0) || (strcmp(command, "--revoke") == 0)) {
        return ur_dispatch_permission_update(bundle, argc, argv, 1, false);
    }

    if ((strcmp(command, "run") == 0) || (strcmp(command, "--run") == 0)) {
        if (argc < 3) {
            fprintf(stderr, "function name is required\n");
            return 1;
        }

        return ur_runtime_run(stdout, bundle, argv[2]) == UR_OK ? 0 : 1;
    }

    if (strcmp(command, "hot-swap") == 0) {
        printf("hot-swap contract: group=%s revision=%s abi=%s\n",
            bundle->hotswap_group,
            bundle->payload_revision,
            bundle->runtime_abi
        );
        printf("binary payload replacement is not implemented in this bootstrap repo yet\n");
        return 0;
    }

    fprintf(stderr, "unknown command: %s\n", command);
    ur_print_usage(stderr, argv[0]);
    return 1;
}
