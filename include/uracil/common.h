#ifndef URACIL_COMMON_H
#define URACIL_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UR_ARRAY_LEN(values) (sizeof(values) / sizeof((values)[0]))
#define UR_UNUSED(value) ((void)(value))

typedef enum ur_status {
    UR_OK = 0,
    UR_ERROR = 1,
    UR_NOT_FOUND = 2,
    UR_PERMISSION_DENIED = 3,
    UR_INVALID_ARGUMENT = 4,
    UR_IO_ERROR = 5,
    UR_OUT_OF_MEMORY = 6,
    UR_NOT_SUPPORTED = 7
} ur_status;

#endif
