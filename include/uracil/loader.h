#ifndef URACIL_LOADER_H
#define URACIL_LOADER_H

#include "uracil/bundle.h"

/**
 * A loaded bundle owns all the memory for the bundle descriptor,
 * function descriptors, and all associated strings.  It is produced
 * by ur_loader_load_bundle() and freed with ur_loader_free_bundle().
 */
typedef struct ur_loaded_bundle {
    ur_service_bundle bundle;
    char *service_directory;
    /* private storage — do not touch */
    ur_function_descriptor *functions_storage;
    char **strings;
    size_t strings_count;
} ur_loaded_bundle;

/**
 * Load an external service bundle from a directory that contains
 * a manifest.json file.
 *
 * The returned bundle can be used with any ur_runtime_* function.
 * The caller must free it with ur_loader_free_bundle().
 */
ur_status ur_loader_load_bundle(
    const char *directory,
    ur_loaded_bundle **out
);

/**
 * Free a loaded bundle and all associated memory.
 */
void ur_loader_free_bundle(ur_loaded_bundle *loaded);

#endif
