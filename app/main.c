#include <stdio.h>
#include <string.h>

#include "demo_bundle.h"
#include "uracil/runtime.h"

static void ur_print_usage(FILE *stream, const char *program_name)
{
    fprintf(stream,
        "Uracil host runtime scaffold\n\n"
        "usage:\n"
        "  %s describe\n"
        "  %s permissions\n"
        "  %s grant <permission> [permission...]\n"
        "  %s revoke <permission> [permission...]\n"
        "  %s run <function>\n"
        "  %s hot-swap\n"
        "\nflags:\n"
        "  %s --grant <permission> [permission...]\n"
        "  %s --revoke <permission> [permission...]\n"
        "  %s --version\n"
        "\nexamples:\n"
        "  %s describe\n"
        "  %s --grant fs.read clock.read\n"
        "  %s run inspect-fs\n",
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name,
        program_name
    );
}

static int ur_dispatch_permission_update(
    const ur_service_bundle *bundle,
    int argc,
    char **argv,
    bool grant
)
{
    if (argc < 3) {
        fprintf(stderr, "permission list is required\n");
        return 1;
    }

    return (grant
        ? ur_runtime_grant_permissions(stderr, bundle, argc - 2, (const char *const *)&argv[2])
        : ur_runtime_revoke_permissions(stderr, bundle, argc - 2, (const char *const *)&argv[2])) == UR_OK
        ? 0
        : 1;
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
        printf("%s %s (%s)\n", bundle->service_name, bundle->service_version, bundle->runtime_abi);
        return 0;
    }

    if ((strcmp(command, "describe") == 0) || (strcmp(command, "--describe") == 0)) {
        return ur_runtime_print_bundle(stdout, bundle) == UR_OK ? 0 : 1;
    }

    if ((strcmp(command, "permissions") == 0) || (strcmp(command, "--permissions") == 0)) {
        return ur_runtime_print_permissions(stdout, bundle) == UR_OK ? 0 : 1;
    }

    if ((strcmp(command, "grant") == 0) || (strcmp(command, "--grant") == 0)) {
        return ur_dispatch_permission_update(bundle, argc, argv, true);
    }

    if ((strcmp(command, "revoke") == 0) || (strcmp(command, "--revoke") == 0)) {
        return ur_dispatch_permission_update(bundle, argc, argv, false);
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
