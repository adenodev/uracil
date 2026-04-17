/*
 * Uracil – Subprocess function executor.
 *
 * Executes function snippets from externally loaded bundles as
 * child processes.  Sets URACIL_* environment variables so the
 * snippet can discover its runtime context.
 *
 * The executor looks for Python files under:
 *   <service_directory>/functions/<function_name>.py
 *
 * And runs:
 *   python3 functions/<name>.py [args...]
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "uracil/permissions.h"
#include "uracil/platform.h"
#include "uracil/process.h"

#define UR_PROCESS_MAX_ARGS 64

static int ur_process_setenv_safe(const char *name, const char *value)
{
    if ((name == NULL) || (value == NULL)) {
        return 0;
    }
    return setenv(name, value, 1);
}

static void ur_process_set_runtime_env(
    const ur_service_context *context,
    const ur_loaded_bundle *loaded
)
{
    char permissions_buf[256];
    char grants_buf[256];
    char state_dir[512];

    ur_process_setenv_safe("URACIL_SERVICE_NAME", context->bundle->service_name);
    ur_process_setenv_safe("URACIL_SERVICE_VERSION", context->bundle->service_version);
    ur_process_setenv_safe("URACIL_RUNTIME_ABI", context->bundle->runtime_abi);
    ur_process_setenv_safe("URACIL_HOTSWAP_GROUP", context->bundle->hotswap_group);
    ur_process_setenv_safe("URACIL_PAYLOAD_REVISION", context->bundle->payload_revision);
    ur_process_setenv_safe("URACIL_SERVICE_DIR", loaded->service_directory);
    ur_process_setenv_safe("URACIL_ROOTLESS", context->rootless ? "true" : "false");

    ur_permission_mask_format(
        context->bundle->requested_permissions,
        permissions_buf,
        sizeof(permissions_buf));
    ur_process_setenv_safe("URACIL_REQUESTED_PERMISSIONS", permissions_buf);

    ur_permission_mask_format(
        context->granted_permissions,
        grants_buf,
        sizeof(grants_buf));
    ur_process_setenv_safe("URACIL_GRANTED_PERMISSIONS", grants_buf);

    if (ur_platform_state_dir(state_dir, sizeof(state_dir)) == UR_OK) {
        ur_process_setenv_safe("URACIL_STATE_DIR", state_dir);
    }
}

static bool ur_process_find_snippet(
    const ur_loaded_bundle *loaded,
    const char *function_name,
    char *snippet_path,
    size_t path_size
)
{
    int written;

    /* Try functions/<name>.py first */
    written = snprintf(snippet_path, path_size,
        "%s/functions/%s.py", loaded->service_directory, function_name);
    if ((written > 0) && ((size_t)written < path_size)) {
        if (access(snippet_path, F_OK) == 0) {
            return true;
        }
    }

    /* Try with hyphens replaced by underscores */
    {
        char normalized[256];
        size_t index = 0U;
        const char *src = function_name;

        while ((*src != '\0') && (index + 1U < sizeof(normalized))) {
            normalized[index++] = (*src == '-') ? '_' : *src;
            ++src;
        }
        normalized[index] = '\0';

        written = snprintf(snippet_path, path_size,
            "%s/functions/%s.py", loaded->service_directory, normalized);
        if ((written > 0) && ((size_t)written < path_size)) {
            if (access(snippet_path, F_OK) == 0) {
                return true;
            }
        }
    }

    return false;
}

ur_status ur_process_execute_function(
    const ur_service_context *context,
    const ur_loaded_bundle *loaded,
    const char *function_name,
    int argc,
    const char *const argv[],
    FILE *out
)
{
    char snippet_path[1024];
    pid_t child;
    int status;
    int pipe_fd[2];

    if ((context == NULL) || (loaded == NULL) || (function_name == NULL) || (out == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (!ur_process_find_snippet(loaded, function_name, snippet_path, sizeof(snippet_path))) {
        fprintf(out, "snippet not found: functions/%s.py\n", function_name);
        return UR_NOT_FOUND;
    }

    if (argc > UR_PROCESS_MAX_ARGS) {
        fprintf(out, "too many arguments (max %d)\n", UR_PROCESS_MAX_ARGS);
        return UR_INVALID_ARGUMENT;
    }

    /* Create a pipe to capture child output */
    if (pipe(pipe_fd) != 0) {
        fprintf(out, "failed to create pipe: %s\n", strerror(errno));
        return UR_IO_ERROR;
    }

    child = fork();
    if (child < 0) {
        fprintf(out, "fork failed: %s\n", strerror(errno));
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return UR_IO_ERROR;
    }

    if (child == 0) {
        /* ── Child process ─────────────────────────────────────────── */
        char *exec_argv[UR_PROCESS_MAX_ARGS + 4];
        int exec_index = 0;
        int arg_index;

        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        /* Change to service directory */
        if (chdir(loaded->service_directory) != 0) {
            _exit(127);
        }

        ur_process_set_runtime_env(context, loaded);
        ur_process_setenv_safe("URACIL_FUNCTION_NAME", function_name);

        exec_argv[exec_index++] = "python3";
        exec_argv[exec_index++] = snippet_path;

        for (arg_index = 0; arg_index < argc; ++arg_index) {
            exec_argv[exec_index++] = (char *)argv[arg_index];
        }
        exec_argv[exec_index] = NULL;

        execvp("python3", exec_argv);
        _exit(127);
    }

    /* ── Parent process ──────────────────────────────────────────── */
    {
        char buffer[4096];
        ssize_t bytes_read;

        close(pipe_fd[1]);

        while ((bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1U)) > 0) {
            buffer[bytes_read] = '\0';
            fputs(buffer, out);
        }

        close(pipe_fd[0]);
    }

    if (waitpid(child, &status, 0) < 0) {
        fprintf(out, "waitpid failed: %s\n", strerror(errno));
        return UR_IO_ERROR;
    }

    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            return UR_OK;
        }
        if (exit_code == 127) {
            fprintf(out, "python3 not found or snippet failed to execute\n");
        }
        return UR_ERROR;
    }

    return UR_ERROR;
}
