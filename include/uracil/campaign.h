#ifndef URACIL_CAMPAIGN_H
#define URACIL_CAMPAIGN_H

#include <stdio.h>

#include "uracil/common.h"

#define UR_CAMPAIGN_KERNEL_ABI "uracil.campaign.v1"
#define UR_CAMPAIGN_MAX_ID 128
#define UR_CAMPAIGN_MAX_SERVICE 128
#define UR_CAMPAIGN_MAX_ENV 64
#define UR_CAMPAIGN_MAX_SECURITY 64
#define UR_CAMPAIGN_MAX_PATH 512
#define UR_CAMPAIGN_MAX_ARTIFACTS 64

typedef enum ur_campaign_phase {
    UR_CAMPAIGN_PHASE_BOOTSTRAP = 0,
    UR_CAMPAIGN_PHASE_RUNNING = 1,
    UR_CAMPAIGN_PHASE_COMPLETED = 2,
    UR_CAMPAIGN_PHASE_FAILED = 3,
    UR_CAMPAIGN_PHASE_COUNT = 4
} ur_campaign_phase;

typedef struct ur_campaign_kernel {
    char campaign_id[UR_CAMPAIGN_MAX_ID];
    char owner_service[UR_CAMPAIGN_MAX_SERVICE];
    char target_env[UR_CAMPAIGN_MAX_ENV];
    char runtime_abi[UR_CAMPAIGN_MAX_ENV];
    char security_profile[UR_CAMPAIGN_MAX_SECURITY];
    ur_campaign_phase phase;
    uint64_t created_at;
    uint64_t updated_at;
    uint64_t completed_at;
    size_t artifact_count;
    char artifacts[UR_CAMPAIGN_MAX_ARTIFACTS][UR_CAMPAIGN_MAX_PATH];
} ur_campaign_kernel;

const char *ur_campaign_phase_name(ur_campaign_phase phase);
ur_campaign_phase ur_campaign_phase_from_name(const char *name);

ur_status ur_campaign_kernel_root_path(
    const char *campaign_id,
    char *buffer,
    size_t buffer_size
);

ur_status ur_campaign_kernel_artifacts_path(
    const char *campaign_id,
    char *buffer,
    size_t buffer_size
);

ur_status ur_campaign_kernel_boot(
    const char *campaign_id,
    const char *owner_service,
    const char *target_env,
    const char *runtime_abi,
    const char *security_profile,
    ur_campaign_kernel *out_campaign
);

ur_status ur_campaign_kernel_load(
    const char *campaign_id,
    ur_campaign_kernel *out_campaign
);

ur_status ur_campaign_kernel_store(const ur_campaign_kernel *campaign);

ur_status ur_campaign_kernel_transition(
    ur_campaign_kernel *campaign,
    ur_campaign_phase phase
);

ur_status ur_campaign_kernel_attach_artifact(
    ur_campaign_kernel *campaign,
    const char *artifact_path
);

ur_status ur_campaign_kernel_cleanup_artifacts(
    ur_campaign_kernel *campaign,
    FILE *stream,
    size_t *out_removed
);

ur_status ur_campaign_kernel_complete(
    ur_campaign_kernel *campaign,
    FILE *stream,
    size_t *out_removed
);

ur_status ur_campaign_kernel_destroy(
    ur_campaign_kernel *campaign,
    bool cleanup_artifacts,
    FILE *stream,
    size_t *out_removed
);

ur_status ur_campaign_kernel_print(FILE *stream, const ur_campaign_kernel *campaign);

#endif