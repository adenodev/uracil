#ifndef URACIL_PERMISSIONS_H
#define URACIL_PERMISSIONS_H

#include "uracil/common.h"

typedef enum ur_permission_id {
    UR_PERMISSION_FS_READ = 0,
    UR_PERMISSION_FS_WRITE,
    UR_PERMISSION_NET_CLIENT,
    UR_PERMISSION_NET_SERVER,
    UR_PERMISSION_PROC_SPAWN,
    UR_PERMISSION_CLOCK_READ,
    UR_PERMISSION_COUNT
} ur_permission_id;

typedef uint64_t ur_permission_mask;

#define UR_PERMISSION_MASK(permission_id) (1ULL << (permission_id))
#define UR_PERMISSION_MASK_NONE ((ur_permission_mask)0)

const char *ur_permission_name(ur_permission_id permission_id);
ur_permission_id ur_permission_from_name(const char *name);
bool ur_permission_mask_contains(ur_permission_mask granted, ur_permission_mask required);
void ur_permission_mask_format(ur_permission_mask mask, char *buffer, size_t buffer_size);

ur_status ur_permission_load_grants(
    const char *service_name,
    ur_permission_mask requested,
    ur_permission_mask *out_grants
);

ur_status ur_permission_save_grants(
    const char *service_name,
    ur_permission_mask requested,
    ur_permission_mask grants
);

#endif
