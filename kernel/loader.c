/*
 * Uracil – External bundle loader.
 *
 * Reads manifest.json from a service directory and constructs a
 * ur_service_bundle in memory.  The loaded bundle can then be used
 * with the same runtime functions as compiled-in bundles.
 *
 * For external bundles, function entry points are NULL — they are
 * executed through ur_process_execute_function() instead.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uracil/json.h"
#include "uracil/loader.h"
#include "uracil/permissions.h"

#define UR_LOADER_MAX_STRINGS 256

static char *ur_loader_read_file(const char *path, size_t *out_length)
{
    FILE *file;
    long file_size;
    char *buffer;
    size_t bytes_read;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return NULL;
    }

    rewind(file);

    buffer = malloc((size_t)file_size + 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    bytes_read = fread(buffer, 1U, (size_t)file_size, file);
    fclose(file);

    if ((long)bytes_read != file_size) {
        free(buffer);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    if (out_length != NULL) {
        *out_length = bytes_read;
    }
    return buffer;
}

static char *ur_loader_own_string(ur_loaded_bundle *loaded, const char *src)
{
    char *copy;

    if (src == NULL) {
        return NULL;
    }
    if (loaded->strings_count >= UR_LOADER_MAX_STRINGS) {
        return NULL;
    }

    copy = strdup(src);
    if (copy == NULL) {
        return NULL;
    }

    loaded->strings[loaded->strings_count++] = copy;
    return copy;
}

static ur_permission_mask ur_loader_parse_permissions_array(const ur_json_node *array)
{
    ur_permission_mask mask = UR_PERMISSION_MASK_NONE;
    size_t index;
    size_t count;

    if (array == NULL) {
        return mask;
    }

    count = ur_json_array_length(array);
    for (index = 0U; index < count; ++index) {
        const char *name = ur_json_string(ur_json_array_get(array, index));
        if (name != NULL) {
            ur_permission_id perm = ur_permission_from_name(name);
            if (perm < UR_PERMISSION_COUNT) {
                mask |= UR_PERMISSION_MASK(perm);
            }
        }
    }
    return mask;
}

ur_status ur_loader_load_bundle(
    const char *directory,
    ur_loaded_bundle **out
)
{
    char manifest_path[1024];
    char *file_data = NULL;
    size_t file_length = 0;
    ur_json_node *root = NULL;
    ur_loaded_bundle *loaded = NULL;
    const ur_json_node *runtime_node;
    const ur_json_node *service_node;
    const ur_json_node *permissions_node;
    const ur_json_node *functions_node;
    size_t func_count;
    size_t index;
    int written;

    if ((directory == NULL) || (out == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    *out = NULL;

    written = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", directory);
    if ((written < 0) || ((size_t)written >= sizeof(manifest_path))) {
        return UR_INVALID_ARGUMENT;
    }

    file_data = ur_loader_read_file(manifest_path, &file_length);
    if (file_data == NULL) {
        return UR_IO_ERROR;
    }

    root = ur_json_parse(file_data, file_length);
    free(file_data);
    if (root == NULL) {
        return UR_ERROR;
    }

    loaded = calloc(1U, sizeof(*loaded));
    if (loaded == NULL) {
        ur_json_free(root);
        return UR_ERROR;
    }

    loaded->strings = calloc(UR_LOADER_MAX_STRINGS, sizeof(char *));
    if (loaded->strings == NULL) {
        free(loaded);
        ur_json_free(root);
        return UR_ERROR;
    }

    loaded->service_directory = strdup(directory);
    if (loaded->service_directory == NULL) {
        free(loaded->strings);
        free(loaded);
        ur_json_free(root);
        return UR_ERROR;
    }

    /* Top-level fields */
    loaded->bundle.service_name = ur_loader_own_string(
        loaded, ur_json_string(ur_json_get(root, "name")));
    loaded->bundle.service_version = ur_loader_own_string(
        loaded, ur_json_string(ur_json_get(root, "version")));
    loaded->bundle.description = ur_loader_own_string(
        loaded, ur_json_string(ur_json_get(root, "description")));

    if ((loaded->bundle.service_name == NULL) || (loaded->bundle.service_version == NULL)) {
        ur_loader_free_bundle(loaded);
        ur_json_free(root);
        return UR_ERROR;
    }

    /* Runtime section */
    runtime_node = ur_json_get(root, "runtime");
    if (runtime_node != NULL) {
        loaded->bundle.runtime_abi = ur_loader_own_string(
            loaded, ur_json_string(ur_json_get(runtime_node, "abi")));
        loaded->bundle.hotswap_group = ur_loader_own_string(
            loaded, ur_json_string(ur_json_get(runtime_node, "hotswap_group")));
        loaded->bundle.payload_revision = ur_loader_own_string(
            loaded, ur_json_string(ur_json_get(runtime_node, "payload_revision")));
    }

    /* Fallback defaults */
    if (loaded->bundle.runtime_abi == NULL) {
        loaded->bundle.runtime_abi = ur_loader_own_string(loaded, "uracil.service.v1");
    }
    if (loaded->bundle.hotswap_group == NULL) {
        loaded->bundle.hotswap_group = ur_loader_own_string(
            loaded, loaded->bundle.service_name);
    }
    if (loaded->bundle.payload_revision == NULL) {
        loaded->bundle.payload_revision = ur_loader_own_string(loaded, "unknown");
    }
    if (loaded->bundle.description == NULL) {
        loaded->bundle.description = ur_loader_own_string(loaded, "");
    }

    /* Permissions section */
    permissions_node = ur_json_get(root, "permissions");
    if (permissions_node != NULL) {
        loaded->bundle.requested_permissions = ur_loader_parse_permissions_array(
            ur_json_get(permissions_node, "requested"));
    }

    /* Service / functions section */
    service_node = ur_json_get(root, "service");
    functions_node = (service_node != NULL)
        ? ur_json_get(service_node, "functions")
        : NULL;

    func_count = ur_json_array_length(functions_node);
    if (func_count > 0U) {
        loaded->functions_storage = calloc(func_count, sizeof(ur_function_descriptor));
        if (loaded->functions_storage == NULL) {
            ur_loader_free_bundle(loaded);
            ur_json_free(root);
            return UR_ERROR;
        }

        for (index = 0U; index < func_count; ++index) {
            const ur_json_node *func = ur_json_array_get(functions_node, index);
            ur_function_descriptor *desc = &loaded->functions_storage[index];

            desc->name = ur_loader_own_string(
                loaded, ur_json_string(ur_json_get(func, "name")));
            desc->description = ur_loader_own_string(
                loaded, ur_json_string(ur_json_get(func, "description")));
            desc->required_permissions = ur_loader_parse_permissions_array(
                ur_json_get(func, "required_permissions"));
            desc->entry = NULL; /* external functions — executed via subprocess */
        }

        loaded->bundle.function_count = func_count;
        loaded->bundle.functions = loaded->functions_storage;
    }

    ur_json_free(root);
    *out = loaded;
    return UR_OK;
}

void ur_loader_free_bundle(ur_loaded_bundle *loaded)
{
    size_t index;

    if (loaded == NULL) {
        return;
    }

    free(loaded->service_directory);
    free(loaded->functions_storage);

    for (index = 0U; index < loaded->strings_count; ++index) {
        free(loaded->strings[index]);
    }
    free(loaded->strings);
    free(loaded);
}
