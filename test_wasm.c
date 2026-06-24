/*
 * Simple test for Uracil WASM module.
 * This can be compiled with emcc to test the WASM build.
 */

#include <stdio.h>
#include <string.h>

#include "uracil/runtime.h"
#include "uracil/loader.h"
#include "uracil/permissions.h"
#include "uracil/campaign.h"
#include "uracil/json.h"

int main(void)
{
    printf("Uracil WASM test starting...\n");

    // Test runtime init
    ur_status status = ur_runtime_init();
    if (status != UR_OK) {
        printf("Runtime init failed: %d\n", status);
        return 1;
    }
    printf("Runtime initialized\n");

    // Test JSON parsing
    const char *json_str = "{\"name\":\"test\",\"value\":42}";
    ur_json_node *node = ur_json_parse(json_str, strlen(json_str));
    if (node != NULL) {
        printf("JSON parsed successfully\n");
        ur_json_free(node);
    } else {
        printf("JSON parse failed\n");
    }

    // Test permissions
    ur_permission_mask mask = UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) | UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ);
    printf("Permission mask: 0x%lx\n", (unsigned long)mask);

    // Test campaign kernel boot
    ur_campaign_kernel campaign;
    status = ur_campaign_kernel_boot("test-campaign", "test-service", "generic", UR_CAMPAIGN_KERNEL_ABI, "none", &campaign);
    if (status == UR_OK) {
        printf("Campaign kernel booted\n");
        ur_campaign_kernel_print(stdout, &campaign);
    } else {
        printf("Campaign kernel boot failed: %d\n", status);
    }

    ur_runtime_cleanup();
    printf("Uracil WASM test complete\n");
    return 0;
}