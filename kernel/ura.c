/*
 * uracil — .ura service binary loader (kernel side).
 *
 * See include/uracil/ura.h for the container layout. Parsing mirrors
 * stub/ura_stub.c so native and wasm targets consume the same format.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "uracil/ura.h"

#define UR_URA_MAGIC_LEN 8
#define UR_URA_TRAILER_LEN 16
#define UR_URA_MAX_FILE (32U * 1024U * 1024U)

static const unsigned char ur_ura_magic[UR_URA_MAGIC_LEN] = {
    'U', 'R', 'A', 0x01, 'S', 'V', 'C', 0x00
};

static void ur_ura_copy(char *destination, const char *source, size_t size)
{
    size_t length;

    if ((destination == NULL) || (size == 0U)) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    length = strlen(source);
    if (length >= size) {
        length = size - 1U;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static uint16_t ur_ura_read_u16(const unsigned char *data)
{
    return (uint16_t)(data[0] | (data[1] << 8));
}

static uint32_t ur_ura_read_u32(const unsigned char *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t ur_ura_read_u64(const unsigned char *data)
{
    uint64_t value = 0U;
    int index;

    for (index = 7; index >= 0; --index) {
        value = (value << 8) | data[index];
    }
    return value;
}

static unsigned char *ur_ura_read_whole_file(const char *path, size_t *out_length)
{
    FILE *file = fopen(path, "rb");
    long size;
    unsigned char *data;

    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if ((size < (long)(UR_URA_TRAILER_LEN + 1)) || ((size_t)size > UR_URA_MAX_FILE)) {
        fclose(file);
        return NULL;
    }
    rewind(file);

    data = malloc((size_t)size);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1U, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);

    *out_length = (size_t)size;
    return data;
}

/* ── .ura.meta parsing (mirrors the stub) ─────────────────────────────── */

static ur_ura_function *ur_ura_meta_function(ur_ura_service *service, const char *name)
{
    int index;

    for (index = 0; index < service->function_count; ++index) {
        if (strcmp(service->functions[index].name, name) == 0) {
            return &service->functions[index];
        }
    }
    return NULL;
}

static void ur_ura_parse_meta(ur_ura_service *service, const char *meta, size_t length)
{
    char line[1024];
    size_t position = 0U;

    while (position < length) {
        size_t line_length = 0U;
        char *key;
        char *value;

        while ((position + line_length < length) && (meta[position + line_length] != '\n')) {
            ++line_length;
        }
        if (line_length >= sizeof(line)) {
            line_length = sizeof(line) - 1U;
        }
        memcpy(line, meta + position, line_length);
        line[line_length] = '\0';
        position += line_length + 1U;

        line[strcspn(line, "\r\n")] = '\0';
        if ((line[0] == '\0') || (line[0] == '#')) {
            continue;
        }

        key = line;
        value = strchr(line, ' ');
        if (value != NULL) {
            *value = '\0';
            ++value;
        } else {
            value = line + strlen(line);
        }

        if (strcmp(key, "name") == 0) {
            ur_ura_copy(service->name, value, sizeof(service->name));
        } else if (strcmp(key, "version") == 0) {
            ur_ura_copy(service->version, value, sizeof(service->version));
        } else if (strcmp(key, "abi") == 0) {
            ur_ura_copy(service->abi, value, sizeof(service->abi));
        } else if (strcmp(key, "mode") == 0) {
            ur_ura_copy(service->mode, value, sizeof(service->mode));
        } else if (strcmp(key, "platform") == 0) {
            ur_ura_copy(service->platform, value, sizeof(service->platform));
        } else if (strcmp(key, "entry") == 0) {
            ur_ura_copy(service->entry, value, sizeof(service->entry));
        } else if (strcmp(key, "fn") == 0) {
            if (service->function_count < UR_URA_MAX_FUNCTIONS) {
                ur_ura_function *function = &service->functions[service->function_count];
                char *space = strchr(value, ' ');

                memset(function, 0, sizeof(*function));
                if (space != NULL) {
                    *space = '\0';
                }
                ur_ura_copy(function->name, value, sizeof(function->name));
                ur_ura_copy(function->language, "python", sizeof(function->language));
                service->function_count++;
            }
        } else if (strcmp(key, "fn-language") == 0) {
            char *space = strchr(value, ' ');
            ur_ura_function *function = NULL;

            if (space != NULL) {
                *space = '\0';
                function = ur_ura_meta_function(service, value);
                if (function != NULL) {
                    ur_ura_copy(function->language, space + 1, sizeof(function->language));
                }
            }
        } else if (strcmp(key, "desc") == 0) {
            char *space = strchr(value, ' ');
            ur_ura_function *function = NULL;

            if (space != NULL) {
                *space = '\0';
                function = ur_ura_meta_function(service, value);
                if (function != NULL) {
                    ur_ura_copy(function->description, space + 1, sizeof(function->description));
                }
            }
        } else if (strcmp(key, "req") == 0) {
            char *space = strchr(value, ' ');
            char *token;
            ur_ura_function *function = NULL;

            if (space != NULL) {
                *space = '\0';
            }
            function = ur_ura_meta_function(service, value);
            if (function != NULL) {
                token = (space != NULL) ? strtok(space + 1, " ") : NULL;
                while ((token != NULL) && (function->require_count < UR_URA_MAX_REQUIRES)) {
                    ur_ura_copy(function->requires[function->require_count], token, 32);
                    function->require_count++;
                    token = strtok(NULL, " ");
                }
            }
        } else if (strcmp(key, "perm") == 0) {
            char *token = strtok(value, " ");

            while ((token != NULL) && (service->requested_count < UR_URA_MAX_REQUIRES)) {
                ur_ura_copy(service->requested[service->requested_count], token, 32);
                service->requested_count++;
                token = strtok(NULL, " ");
            }
        }
        /* runner and cauth lines are part of the format; ignored here */
    }
}

/* ── payload extraction ───────────────────────────────────────────────── */

static ur_status ur_ura_make_directory(const char *path)
{
    if ((mkdir(path, 0755) == 0) || (errno == EEXIST)) {
        return UR_OK;
    }
    return UR_IO_ERROR;
}

static ur_status ur_ura_make_directories(const char *path)
{
    char buffer[1024];
    size_t index;

    ur_ura_copy(buffer, path, sizeof(buffer));
    for (index = 1U; buffer[index] != '\0'; ++index) {
        if (buffer[index] == '/') {
            buffer[index] = '\0';
            if (ur_ura_make_directory(buffer) != UR_OK) {
                return UR_IO_ERROR;
            }
            buffer[index] = '/';
        }
    }
    return ur_ura_make_directory(buffer);
}

ur_status ur_ura_extract_payload(const ur_ura_service *service, const char *destination_dir)
{
    size_t position = 0U;

    if ((service == NULL) || (destination_dir == NULL)) {
        return UR_INVALID_ARGUMENT;
    }
    if (ur_ura_make_directories(destination_dir) != UR_OK) {
        return UR_IO_ERROR;
    }

    while (position + 2U <= service->payload_length) {
        uint16_t name_length = ur_ura_read_u16(service->payload + position);
        const unsigned char *name;
        char entry_path[1024];
        char out_path[1024];
        uint32_t data_length;
        FILE *file;
        int written;

        position += 2U;
        if ((name_length == 0U) || (name_length > 512U) ||
            (position + name_length + 4U > service->payload_length)) {
            return UR_ERROR;
        }

        name = service->payload + position;
        position += name_length;
        memcpy(entry_path, name, name_length);
        entry_path[name_length] = '\0';

        data_length = ur_ura_read_u32(service->payload + position);
        position += 4U;
        if (position + data_length > service->payload_length) {
            return UR_ERROR;
        }

        /* refuse directory traversal, like the native stub */
        if ((strstr(entry_path, "..") != NULL) || (entry_path[0] == '/') || (entry_path[0] == '\\')) {
            return UR_ERROR;
        }

        written = snprintf(out_path, sizeof(out_path), "%s/%s", destination_dir, entry_path);
        if ((written < 0) || ((size_t)written >= sizeof(out_path))) {
            return UR_IO_ERROR;
        }

        {
            char parent[1024];
            char *slash;

            ur_ura_copy(parent, out_path, sizeof(parent));
            slash = strrchr(parent, '/');
            if (slash != NULL) {
                *slash = '\0';
                if (ur_ura_make_directories(parent) != UR_OK) {
                    return UR_IO_ERROR;
                }
            }
        }

        file = fopen(out_path, "wb");
        if (file == NULL) {
            return UR_IO_ERROR;
        }
        if (fwrite(service->payload + position, 1U, data_length, file) != data_length) {
            fclose(file);
            return UR_IO_ERROR;
        }
        fclose(file);

        position += data_length;
    }

    return UR_OK;
}

/* ── load / free / queries ────────────────────────────────────────────── */

ur_status ur_ura_load(const char *path, ur_ura_service **out)
{
    ur_ura_service *service;
    unsigned char *file_data = NULL;
    size_t file_length = 0U;
    const unsigned char *trailer;
    uint64_t payload_size;
    size_t payload_start;
    size_t meta_size = 0U;

    if ((path == NULL) || (out == NULL)) {
        return UR_INVALID_ARGUMENT;
    }
    *out = NULL;

    file_data = ur_ura_read_whole_file(path, &file_length);
    if (file_data == NULL) {
        return UR_IO_ERROR;
    }

    trailer = file_data + file_length - UR_URA_TRAILER_LEN;
    if (memcmp(trailer + 8, ur_ura_magic, UR_URA_MAGIC_LEN) != 0) {
        free(file_data);
        return UR_ERROR;
    }

    payload_size = ur_ura_read_u64(trailer);
    if ((payload_size + UR_URA_TRAILER_LEN) > file_length) {
        free(file_data);
        return UR_ERROR;
    }
    payload_start = file_length - UR_URA_TRAILER_LEN - (size_t)payload_size;

    service = calloc(1U, sizeof(*service));
    if (service == NULL) {
        free(file_data);
        return UR_OUT_OF_MEMORY;
    }

    service->module = file_data;
    service->module_length = payload_start;
    service->payload = file_data + payload_start;
    service->payload_length = (size_t)payload_size;

    /* locate .ura.meta (always the first entry) */
    if (service->payload_length >= 2U) {
        uint16_t name_length = ur_ura_read_u16(service->payload);

        if ((name_length == 9U) && (memcmp(service->payload + 2U, ".ura.meta", 9U) == 0)) {
            uint32_t meta_length;

            meta_size = 2U + name_length;
            meta_length = ur_ura_read_u32(service->payload + meta_size);
            meta_size += 4U;
            if (meta_size + meta_length <= service->payload_length) {
                ur_ura_parse_meta(service, (const char *)(service->payload + meta_size), meta_length);
            }
        }
    }

    if ((service->name[0] == '\0') || (service->version[0] == '\0')) {
        ur_ura_free(service);
        return UR_ERROR;
    }

    *out = service;
    return UR_OK;
}

void ur_ura_free(ur_ura_service *service)
{
    if (service == NULL) {
        return;
    }
    free(service->module); /* module + payload share one allocation */
    free(service);
}

const ur_ura_function *ur_ura_find_function(const ur_ura_service *service, const char *name)
{
    int index;

    if ((service == NULL) || (name == NULL)) {
        return NULL;
    }

    for (index = 0; index < service->function_count; ++index) {
        if (strcmp(service->functions[index].name, name) == 0) {
            return &service->functions[index];
        }
    }
    return NULL;
}

ur_permission_mask ur_ura_function_mask(const ur_ura_function *function)
{
    ur_permission_mask mask = UR_PERMISSION_MASK_NONE;
    int index;

    if (function == NULL) {
        return mask;
    }

    for (index = 0; index < function->require_count; ++index) {
        ur_permission_id permission = ur_permission_from_name(function->requires[index]);

        if (permission < UR_PERMISSION_COUNT) {
            mask |= UR_PERMISSION_MASK(permission);
        }
    }
    return mask;
}

void ur_ura_function_requires_format(const ur_ura_function *function, char *buffer, size_t buffer_size)
{
    int index;
    size_t used = 0U;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }
    buffer[0] = '\0';

    if ((function == NULL) || (function->require_count == 0)) {
        snprintf(buffer, buffer_size, "(none)");
        return;
    }

    for (index = 0; index < function->require_count; ++index) {
        int written = snprintf(
            buffer + used, buffer_size - used, "%s%s",
            (index == 0) ? "" : ", ", function->requires[index]);

        if ((written < 0) || ((size_t)written >= (buffer_size - used))) {
            break;
        }
        used += (size_t)written;
    }
}

int ur_ura_unknown_requirements(const ur_ura_service *service, const char **names, int capacity)
{
    int count = 0;
    int index;

    if ((service == NULL) || (names == NULL) || (capacity <= 0)) {
        return 0;
    }

    for (index = 0; index < service->requested_count; ++index) {
        if ((ur_permission_from_name(service->requested[index]) >= UR_PERMISSION_COUNT) &&
            (count < capacity)) {
            names[count++] = service->requested[index];
        }
    }

    for (index = 0; index < service->function_count; ++index) {
        const ur_ura_function *function = &service->functions[index];
        int require_index;

        for (require_index = 0; require_index < function->require_count; ++require_index) {
            int seen = 0;
            int scan;

            if (ur_permission_from_name(function->requires[require_index]) < UR_PERMISSION_COUNT) {
                continue;
            }
            for (scan = 0; scan < count; ++scan) {
                if (strcmp(names[scan], function->requires[require_index]) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (!seen && (count < capacity)) {
                names[count++] = function->requires[require_index];
            }
        }
    }

    return count;
}
