#ifndef URACIL_PROCESS_H
#define URACIL_PROCESS_H

#include "uracil/loader.h"
#include "uracil/runtime.h"

/**
 * Execute a function from an externally loaded bundle.
 *
 * Locates the function's snippet file under <service_directory>/functions/,
 * sets URACIL_* environment variables, and runs the snippet as a subprocess
 * using Python 3.
 *
 * @param context  The runtime context (must reference a loaded bundle).
 * @param loaded   The loaded bundle (provides the service directory).
 * @param function_name  Which function to execute.
 * @param argc     Argument count for the function (0 is valid).
 * @param argv     Arguments to pass to the function snippet.
 * @return UR_OK on success (exit code 0), UR_ERROR otherwise.
 */
ur_status ur_process_execute_function(
    const ur_service_context *context,
    const ur_loaded_bundle *loaded,
    const char *function_name,
    int argc,
    const char *const argv[],
    FILE *out
);

#endif
