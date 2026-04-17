#include "demo_bundle.h"

#include <stdio.h>
#include <time.h>

#include "uracil/platform.h"
#include "uracil/runtime.h"

static int ur_demo_health(const ur_service_context *context)
{
    fprintf(context->out,
        "service=%s status=ready rootless=%s abi=%s\n",
        context->bundle->service_name,
        context->rootless ? "true" : "false",
        context->bundle->runtime_abi
    );
    return 0;
}

static int ur_demo_inspect_fs(const ur_service_context *context)
{
    fprintf(context->out, "listing current working directory through host adapter:\n");
    return ur_platform_list_directory(".", context->out) == UR_OK ? 0 : 1;
}

static int ur_demo_net_probe(const ur_service_context *context)
{
    fprintf(context->out,
        "network client capability granted; virtual environment would open an outbound socket here\n"
    );
    fprintf(context->out, "payload_revision=%s hot_swap_group=%s\n",
        context->bundle->payload_revision,
        context->bundle->hotswap_group
    );
    return 0;
}

static int ur_demo_clock_now(const ur_service_context *context)
{
    time_t now = time(NULL);
    const struct tm *tm_ptr;
    struct tm tm_now;
    char buffer[64];

    tm_ptr = localtime(&now);
    if (tm_ptr == NULL) {
        fprintf(context->out, "failed to read local clock\n");
        return 1;
    }

    tm_now = *tm_ptr;

    if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S %Z", &tm_now) == 0U) {
        fprintf(context->out, "failed to format local clock\n");
        return 1;
    }

    fprintf(context->out, "virtual clock reading: %s\n", buffer);
    return 0;
}

static const ur_function_descriptor ur_demo_functions[] = {
    {
        .name = "health",
        .description = "Report runtime health and service metadata.",
        .required_permissions = UR_PERMISSION_MASK_NONE,
        .entry = ur_demo_health
    },
    {
        .name = "inspect-fs",
        .description = "List the current working directory through the filesystem adapter.",
        .required_permissions = UR_PERMISSION_MASK(UR_PERMISSION_FS_READ),
        .entry = ur_demo_inspect_fs
    },
    {
        .name = "net-probe",
        .description = "Demonstrate an outbound network client capability.",
        .required_permissions = UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT),
        .entry = ur_demo_net_probe
    },
    {
        .name = "clock-now",
        .description = "Read the virtual clock exposed by the host adapter.",
        .required_permissions = UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ),
        .entry = ur_demo_clock_now
    }
};

static const ur_service_bundle ur_demo_service_bundle = {
    .service_name = "aps.sample.echo-service",
    .service_version = "0.1.0",
    .description = "Sample APS-native service bundle running inside Uracil.",
    .runtime_abi = "uracil.service.v1",
    .hotswap_group = "aps.sample.echo-service",
    .payload_revision = "demo-001",
    .requested_permissions =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT) |
        UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ),
    .function_count = UR_ARRAY_LEN(ur_demo_functions),
    .functions = ur_demo_functions
};

const ur_service_bundle *ur_demo_bundle(void)
{
    return &ur_demo_service_bundle;
}
