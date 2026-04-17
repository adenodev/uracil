#include <string.h>

#include "uracil/bundle.h"

const ur_function_descriptor *ur_bundle_find_function(
    const ur_service_bundle *bundle,
    const char *function_name
)
{
    size_t index;

    if ((bundle == NULL) || (function_name == NULL)) {
        return NULL;
    }

    for (index = 0U; index < bundle->function_count; ++index) {
        const ur_function_descriptor *descriptor = &bundle->functions[index];

        if ((descriptor->name != NULL) && (strcmp(descriptor->name, function_name) == 0)) {
            return descriptor;
        }
    }

    return NULL;
}
