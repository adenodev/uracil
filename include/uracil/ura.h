/*
 * uracil — .ura service binary loader (kernel side).
 *
 * A .ura is the Uracil service container:
 *   [service binary][payload entries][uint64_le payload_size][8B magic]
 * with ".ura.meta" as the first payload entry — the line-oriented index the
 * builder produces from an APS manifest (name, version, abi, mode, platform,
 * entry, fn, fn-language, desc, req, perm, runner and cauth fields).
 *
 * Native targets execute the stub binary directly; on wasm32 the microkernel
 * performs the stub's role: it verifies the trailer, extracts the payload
 * into URACIL_SERVICE_DIR, and turns .ura.meta into the service description
 * the runtime dispatches against.
 */

#ifndef URACIL_URA_H
#define URACIL_URA_H

#include <stddef.h>

#include "uracil/common.h"
#include "uracil/permissions.h"

#define UR_URA_MAX_FUNCTIONS 16
#define UR_URA_MAX_REQUIRES 8
#define UR_URA_NAME 128
#define UR_URA_FIELD 64
#define UR_URA_DESCRIPTION 256

typedef struct ur_ura_function {
    char name[UR_URA_FIELD];
    char language[32];
    char description[UR_URA_DESCRIPTION];
    char requires[UR_URA_MAX_REQUIRES][32];
    int require_count;
} ur_ura_function;

typedef struct ur_ura_service {
    char name[UR_URA_NAME];
    char version[UR_URA_FIELD];
    char abi[UR_URA_FIELD];
    char mode[32];
    char platform[UR_URA_FIELD];
    char entry[UR_URA_FIELD];
    ur_ura_function functions[UR_URA_MAX_FUNCTIONS];
    int function_count;
    char requested[UR_URA_MAX_REQUIRES][32];
    int requested_count;
    unsigned char *module;      /* the service binary (stub bytes) */
    size_t module_length;
    unsigned char *payload;     /* raw payload entry stream */
    size_t payload_length;
} ur_ura_service;

/* Load and verify a .ura from the kernel filesystem. */
ur_status ur_ura_load(const char *path, ur_ura_service **out);

void ur_ura_free(ur_ura_service *service);

/* Write every payload entry into destination_dir (mkdir -p semantics). */
ur_status ur_ura_extract_payload(const ur_ura_service *service, const char *destination_dir);

const ur_ura_function *ur_ura_find_function(const ur_ura_service *service, const char *name);

/* Resolve a function's requires[] into a permission mask. */
ur_permission_mask ur_ura_function_mask(const ur_ura_function *function);

/* Human-readable requires list ("fs.read, clock.read" or "(none)"). */
void ur_ura_function_requires_format(const ur_ura_function *function, char *buffer, size_t buffer_size);

/*
 * Names in requires[]/perm that have no Uracil permission bit. Returns the
 * count and fills names[] with pointers into the service structure.
 */
int ur_ura_unknown_requirements(
    const ur_ura_service *service,
    const char **names,
    int capacity);

#endif
