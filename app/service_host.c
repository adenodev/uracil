/*
 * Uracil — generic hosted-service surface for host pages.
 *
 * Lets a page (the Kairos web app, the iris shell, …) run any `.ura` service
 * without the kernel knowing anything service-specific:
 *
 *   iris_wasm_service_open(name, url)      acquire + stage + load + spawn
 *   iris_wasm_service_call(name, fn, args) per-function gate → dispatch
 *   iris_wasm_service_close(name)
 *   iris_wasm_grant / iris_wasm_revoke / iris_wasm_perms   (page-level UX)
 *
 * Function names, descriptions, and per-function permission requirements come
 * from the `.ura.meta` — the kernel refuses a call before it reaches the
 * service when the function's `req` line isn't covered by the grants, with
 * the same one-line remedy the shell shows.
 *
 * Storage: /urashell/services/<name>/ holds the payload (URACIL_SERVICE_DIR)
 * and the service's data files; it lives on the IDBFS mount, so everything
 * survives reloads. (Revision staging for hot-swap lands in M4.)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "uracil/bundle.h"
#include "uracil/json.h"
#include "uracil/permissions.h"
#include "uracil/platform.h"
#include "uracil/runtime.h"
#include "uracil/ura.h"

#include "host.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define HOST_KEEP EMSCRIPTEN_KEEPALIVE
#else
#define HOST_KEEP
#endif

/* defined in iris_bundle.c */
extern const char *iris_glue_perm_update(const char *name, int grant, unsigned long long *out_mask);
extern const char *iris_glue_perm_list(void);
int js_fetch_blob(const char *url, char **out_ptr, int *out_len);
extern void js_sync_state(void);

#define SERVICE_HOST_MAX 4
#define SERVICE_RESPONSE_CAP (256 * 1024)
#define SERVICE_STATE_ROOT "/urashell/services"

typedef struct service_entry {
    int used;
    int slot;
    char name[64];
    char directory[512];
    ur_ura_service *service;
} service_entry;

static service_entry service_entries[SERVICE_HOST_MAX];
static char service_response[SERVICE_RESPONSE_CAP];

/* ── small helpers ─────────────────────────────────────────────────────── */

static service_entry *service_find(const char *name)
{
    int index;

    for (index = 0; index < SERVICE_HOST_MAX; ++index) {
        if (service_entries[index].used && (strcmp(service_entries[index].name, name) == 0)) {
            return &service_entries[index];
        }
    }
    return NULL;
}

static service_entry *service_alloc(const char *name)
{
    int index;

    for (index = 0; index < SERVICE_HOST_MAX; ++index) {
        if (!service_entries[index].used) {
            service_entry *entry = &service_entries[index];
            memset(entry, 0, sizeof(*entry));
            entry->used = 1;
            snprintf(entry->name, sizeof(entry->name), "%s", name);
            return entry;
        }
    }
    return NULL;
}

static void service_release(service_entry *entry)
{
    if (entry->slot != 0) {
        ur_ura_kill(entry->slot);
    }
    if (entry->service != NULL) {
        ur_ura_free(entry->service);
    }
    memset(entry, 0, sizeof(*entry));
}

static int service_mkdir_soft(const char *path)
{
    if ((mkdir(path, 0755) == 0) || (errno == EEXIST)) {
        return 0;
    }
    return -1;
}

static int service_file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static size_t service_write_bytes(const char *path, const void *data, size_t length)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return 0U;
    }
    if ((length > 0U) && (fwrite(data, 1U, length, file) != length)) {
        fclose(file);
        return 0U;
    }
    fclose(file);
    return length;
}

static ur_permission_mask service_grants(void)
{
    ur_permission_mask mask = UR_PERMISSION_MASK_NONE;
    ur_permission_mask requested =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_FS_WRITE) |
        UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT) |
        UR_PERMISSION_MASK(UR_PERMISSION_NET_SERVER) |
        UR_PERMISSION_MASK(UR_PERMISSION_PROC_SPAWN) |
        UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ);

    ur_permission_load_grants("iris.adeno.ltd", requested, &mask);
    return mask;
}

static const char *service_refuse(const char *action, ur_permission_mask missing)
{
    char list[256];
    char grants[256];
    size_t index;

    ur_permission_mask_format(missing, list, sizeof(list));
    snprintf(grants, sizeof(grants), "%s", list);
    for (index = 0U; grants[index] != '\0'; ++index) {
        if ((grants[index] == ',') && (grants[index + 1U] == ' ')) {
            grants[index] = ' ';
        }
    }

    snprintf(service_response, sizeof(service_response),
        "{\"ok\":false,\"error\":\"permission\",\"missing\":\"%s\","
        "\"message\":\"iris: refusing %s; missing permissions: %s\\n"
        "       grant with: perm grant %s\"}",
        list, action, list, grants);
    return service_response;
}

static void service_error(const char *message)
{
    snprintf(service_response, sizeof(service_response), "{\"ok\":false,\"error\":\"%s\"}", message);
}

/* ── page exports ──────────────────────────────────────────────────────── */

HOST_KEEP const char *iris_wasm_service_open(const char *name, const char *url)
{
    char ura_path[600];
    const char *basename;
    service_entry *entry;
    ur_permission_mask grants;
    ur_permission_mask needed = UR_PERMISSION_MASK_NONE;
    ur_permission_mask missing;
    ur_status status;
    int index;

    if ((name == NULL) || (name[0] == '\0') || (url == NULL)) {
        service_error("service_open requires name and url");
        return service_response;
    }

    entry = service_find(name);
    if ((entry != NULL) && (ur_ura_state(entry->slot) == UR_URA_STATE_READY)) {
        snprintf(service_response, sizeof(service_response),
            "{\"ok\":true,\"already_running\":true,\"name\":\"%s\"}", name);
        return service_response;
    }
    if (entry != NULL) {
        service_release(entry);
    }

    entry = service_alloc(name);
    if (entry == NULL) {
        service_error("no free service slot");
        return service_response;
    }

    (void)ur_platform_ensure_state_dirs();
    (void)service_mkdir_soft(SERVICE_STATE_ROOT);
    snprintf(entry->directory, sizeof(entry->directory), "%s/%s", SERVICE_STATE_ROOT, name);
    (void)service_mkdir_soft(entry->directory);

    basename = strrchr(url, '/');
    basename = (basename != NULL) ? basename + 1 : url;
    if (snprintf(ura_path, sizeof(ura_path), "%s/%s", entry->directory, basename) >= (int)sizeof(ura_path)) {
        service_error("path too long");
        return service_response;
    }

    if (!service_file_exists(ura_path)) {
        char *bytes = NULL;
        int length = 0;
        int rc;

        grants = service_grants();
        if (!ur_permission_mask_contains(grants, UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT))) {
            snprintf(service_response, sizeof(service_response),
                "{\"ok\":false,\"error\":\"permission\",\"missing\":\"net.client\","
                "\"message\":\"iris: refusing to fetch '%s'; missing permissions: net.client\\n"
                "       grant with: perm grant net.client\"}", url);
            return service_response;
        }

        rc = js_fetch_blob(url, &bytes, &length);
        if ((rc != 0) || (bytes == NULL)) {
            service_error("download failed");
            return service_response;
        }
        if (service_write_bytes(ura_path, bytes, (size_t)length) == 0U) {
            free(bytes);
            service_error("staging failed");
            return service_response;
        }
        free(bytes);
    }

    /* load the container: trailer + .ura.meta → service description */
    status = ur_ura_load(ura_path, &entry->service);
    if ((status != UR_OK) || (entry->service == NULL)) {
        snprintf(service_response, sizeof(service_response),
            "{\"ok\":false,\"error\":\"ura load failed (%s)\"}",
            ur_status_string(status));
        return service_response;
    }

    status = ur_ura_extract_payload(entry->service, entry->directory);
    if (status != UR_OK) {
        service_error("payload extraction failed");
        ur_ura_free(entry->service);
        entry->service = NULL;
        return service_response;
    }

    /* the union of the functions' requires must be covered by the grants */
    for (index = 0; index < entry->service->function_count; ++index) {
        needed |= ur_ura_function_mask(&entry->service->functions[index]);
    }
    grants = service_grants();
    missing = needed & ~grants;
    if (missing != UR_PERMISSION_MASK_NONE) {
        char action[96];
        snprintf(action, sizeof(action), "to run '%s'", name);
        ur_ura_free(entry->service);
        entry->service = NULL;
        return service_refuse(action, missing);
    }

    {
        char requested_text[160];
        char granted_text[160];
        size_t used = 0U;

        requested_text[0] = '\0';
        for (index = 0; index < entry->service->requested_count; ++index) {
            int written = snprintf(requested_text + used, sizeof(requested_text) - used,
                "%s%s", (index == 0) ? "" : ",", entry->service->requested[index]);
            if ((written < 0) || ((size_t)written >= (sizeof(requested_text) - used))) {
                break;
            }
            used += (size_t)written;
        }
        ur_permission_mask_format(grants, granted_text, sizeof(granted_text));

        entry->slot = ur_ura_spawn(entry->service->module, (int)entry->service->module_length,
            entry->service->name, entry->service->version,
            entry->service->abi, entry->service->mode,
            entry->directory, "/urashell", requested_text, granted_text,
            (unsigned long long)grants);
    }
    if (entry->slot == 0) {
        service_error("spawn failed");
        ur_ura_free(entry->service);
        entry->service = NULL;
        return service_response;
    }

    /* structured report for the host page */
    {
        char *buf = NULL;
        size_t size = 0;
        FILE *stream = open_memstream(&buf, &size);

        if (stream != NULL) {
            fprintf(stream, "{\"ok\":true,\"name\":\"%s\",\"version\":\"%s\",\"abi\":\"%s\","
                            "\"slot\":%d,\"functions\":[",
                entry->service->name, entry->service->version, entry->service->abi, entry->slot);
            for (index = 0; index < entry->service->function_count; ++index) {
                char requires[160];

                ur_ura_function_requires_format(&entry->service->functions[index], requires, sizeof(requires));
                fprintf(stream, "%s{\"name\":\"%s\",\"requires\":\"%s\"}",
                    (index == 0) ? "" : ",", entry->service->functions[index].name, requires);
            }
            fprintf(stream, "]}");
            fclose(stream);
        }
        if (buf != NULL) {
            snprintf(service_response, sizeof(service_response), "%s", buf);
            free(buf);
        } else {
            service_error("report failed");
        }
    }

    js_sync_state();
    return service_response;
}

HOST_KEEP const char *iris_wasm_service_call(const char *name, const char *function_name, const char *args_json)
{
    service_entry *entry;
    const ur_ura_function *function;
    ur_permission_mask grants;
    ur_permission_mask missing;
    ur_json_node *root = NULL;
    const char *first = NULL;
    const char *second = NULL;
    int arg_count = 0;
    int rc = -1;

    if ((name == NULL) || (function_name == NULL)) {
        service_error("service_call requires name and function");
        return service_response;
    }

    entry = service_find(name);
    if (entry == NULL) {
        service_error("service not open");
        return service_response;
    }
    if ((ur_ura_state(entry->slot) != UR_URA_STATE_READY) || (entry->service == NULL)) {
        service_error("service exited");
        return service_response;
    }

    /* per-function requirement gate, straight from the .ura.meta */
    function = ur_ura_find_function(entry->service, function_name);
    if (function == NULL) {
        snprintf(service_response, sizeof(service_response),
            "{\"ok\":false,\"error\":\"unknown function: %s\"}", function_name);
        return service_response;
    }
    grants = service_grants();
    missing = ur_ura_function_mask(function) & ~grants;
    if (missing != UR_PERMISSION_MASK_NONE) {
        char action[128];

        snprintf(action, sizeof(action), "to run '%s'", function_name);
        return service_refuse(action, missing);
    }

    /* arguments: JSON array of up to two strings */
    if ((args_json != NULL) && (args_json[0] != '\0')) {
        size_t length = strlen(args_json);

        root = ur_json_parse(args_json, length);
        if (root == NULL) {
            service_error("bad args JSON");
            return service_response;
        }
        arg_count = (int)ur_json_array_length(root);
        if (arg_count > 2) {
            ur_json_free(root);
            service_error("at most two string arguments are supported");
            return service_response;
        }
        if (arg_count >= 1) {
            first = ur_json_string(ur_json_array_get(root, 0));
        }
        if (arg_count >= 2) {
            second = ur_json_string(ur_json_array_get(root, 1));
        }
        first = (first != NULL) ? first : "";
        second = (second != NULL) ? second : "";
    }

    service_response[0] = '\0';
    if (arg_count == 2) {
        rc = ur_ura_call_str2(entry->slot, function_name, first, second,
            service_response, sizeof(service_response));
    } else if (arg_count == 1) {
        rc = ur_ura_call_str(entry->slot, function_name, first,
            service_response, sizeof(service_response));
    } else {
        rc = ur_ura_export_str(entry->slot, function_name, service_response, sizeof(service_response));
    }
    {
        /* surface the service's console output (platform write) on stderr */
        static char service_log[4096];

        service_log[0] = '\0';
        if (ur_ura_take_output(entry->slot, service_log, sizeof(service_log)) > 0) {
            fprintf(stderr, "[svc] %s", service_log);
        }
    }

    if (root != NULL) {
        ur_json_free(root);
    }

    if (rc < 0) {
        service_error("service fault");
    } else if (service_response[0] == '\0') {
        service_error("empty response");
    } else {
        js_sync_state();
    }
    return service_response;
}

HOST_KEEP void iris_wasm_service_close(const char *name)
{
    service_entry *entry = (name != NULL) ? service_find(name) : NULL;

    if (entry != NULL) {
        service_release(entry);
        js_sync_state();
    }
}

/* ── hot-swap: replace the running revision, custody intact ────────────── */
/*
 * M4: the enclave module itself is patchable in situ. Flow:
 *   1. the replacement .ura is loaded and its payload extracted (data files —
 *      vault.json, events.jsonl — are preserved; only the payload/manifest
 *      files are rewritten)
 *   2. the new revision's requirements are gated against the grants
 *   3. the kernel mints a one-time transfer secret and asks the running
 *      instance to export custody wrapped under it (exportTransfer)
 *   4. the old instance is killed; the new revision is spawned on the same
 *      service directory (same sealed store)
 *   5. the new instance imports custody (importTransfer) — unlocked, no
 *      re-prompt; the secret is dropped with the swap
 */

static void service_hex_secret(unsigned char secret[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    int index;

    for (index = 0; index < 32; ++index) {
        out[index * 2] = digits[secret[index] >> 4];
        out[index * 2 + 1] = digits[secret[index] & 0x0F];
    }
    out[64] = '\0';
}

static const char *service_swap_with(service_entry *entry, const char *ura_path)
{
    ur_ura_service *next = NULL;
    ur_permission_mask grants;
    ur_permission_mask needed = UR_PERMISSION_MASK_NONE;
    ur_permission_mask missing;
    unsigned char secret[32];
    char secret_hex[65];
    char transfer_payload[4096];
    char transfer_uid[128];
    char old_version[64];
    char requested_text[160];
    char granted_text[160];
    int had_transfer = 0;
    int unlocked = 0;
    int index;

    if (ur_ura_load(ura_path, &next) != UR_OK || next == NULL) {
        service_error("revision load failed");
        return service_response;
    }
    if (ur_ura_extract_payload(next, entry->directory) != UR_OK) {
        ur_ura_free(next);
        service_error("revision extract failed");
        return service_response;
    }

    for (index = 0; index < next->function_count; ++index) {
        needed |= ur_ura_function_mask(&next->functions[index]);
    }
    grants = service_grants();
    missing = needed & ~grants;
    if (missing != UR_PERMISSION_MASK_NONE) {
        char action[128];

        snprintf(action, sizeof(action), "to run '%s' revision %s", entry->name, next->version);
        ur_ura_free(next);
        return service_refuse(action, missing);
    }

    ur_platform_random_bytes(secret, 32);
    service_hex_secret(secret, secret_hex);
    transfer_payload[0] = '\0';
    transfer_uid[0] = '\0';

    if ((entry->slot != 0) && (ur_ura_state(entry->slot) == UR_URA_STATE_READY)) {
        char args[160];
        char response[4608];
        int rc;

        snprintf(args, sizeof(args), "{\"secret\":\"%s\"}", secret_hex);
        response[0] = '\0';
        rc = ur_ura_call_str(entry->slot, "exportTransfer", args, response, sizeof(response));
        if ((rc > 0) && (response[0] != '\0')) {
            ur_json_node *root = ur_json_parse(response, strlen(response));

            if (root != NULL) {
                const char *payload_field = ur_json_string(ur_json_get(root, "payload"));
                const char *uid_field = ur_json_string(ur_json_get(root, "uid"));

                if ((payload_field != NULL) && (uid_field != NULL)) {
                    snprintf(transfer_payload, sizeof(transfer_payload), "%s", payload_field);
                    snprintf(transfer_uid, sizeof(transfer_uid), "%s", uid_field);
                    had_transfer = 1;
                }
                ur_json_free(root);
            }
        }
    }

    snprintf(old_version, sizeof(old_version), "%s",
        (entry->service != NULL) ? entry->service->version : "(none)");

    if (entry->slot != 0) {
        ur_ura_kill(entry->slot);
        entry->slot = 0;
    }
    if (entry->service != NULL) {
        ur_ura_free(entry->service);
    }
    entry->service = next;

    requested_text[0] = '\0';
    {
        size_t used = 0U;

        for (index = 0; index < next->requested_count; ++index) {
            int written = snprintf(requested_text + used, sizeof(requested_text) - used,
                "%s%s", (index == 0) ? "" : ",", next->requested[index]);
            if ((written < 0) || ((size_t)written >= (sizeof(requested_text) - used))) {
                break;
            }
            used += (size_t)written;
        }
    }
    ur_permission_mask_format(grants, granted_text, sizeof(granted_text));

    entry->slot = ur_ura_spawn(next->module, (int)next->module_length,
        next->name, next->version, next->abi, next->mode,
        entry->directory, "/urashell", requested_text, granted_text,
        (unsigned long long)grants);
    if (entry->slot == 0) {
        service_error("revision respawn failed");
        return service_response;
    }

    if (had_transfer) {
        static char transfer_response[1024];
        char args[8192];

        snprintf(args, sizeof(args),
            "{\"secret\":\"%s\",\"payload\":\"%s\",\"uid\":\"%s\"}",
            secret_hex, transfer_payload, transfer_uid);
        transfer_response[0] = '\0';
        (void)ur_ura_call_str(entry->slot, "importTransfer", args,
            transfer_response, sizeof(transfer_response));
        if (strstr(transfer_response, "\"ok\":true") != NULL) {
            unlocked = 1;
        }
    }

    snprintf(service_response, sizeof(service_response),
        "{\"ok\":true,\"name\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
        "\"custody_transferred\":%s,\"unlocked\":%s,\"slot\":%d}",
        entry->name, old_version, next->version,
        had_transfer ? "true" : "false",
        unlocked ? "true" : "false",
        entry->slot);

    js_sync_state();
    return service_response;
}

HOST_KEEP const char *iris_wasm_service_swap(const char *name, const char *url)
{
    service_entry *entry;
    char revisions[600];
    char revision_path[700];
    const char *basename;

    if ((name == NULL) || (url == NULL)) {
        service_error("service_swap requires name and url");
        return service_response;
    }
    entry = service_find(name);
    if (entry == NULL) {
        service_error("service not open");
        return service_response;
    }

    snprintf(revisions, sizeof(revisions), "%s/revisions", entry->directory);
    (void)service_mkdir_soft(revisions);
    basename = strrchr(url, '/');
    basename = (basename != NULL) ? basename + 1 : url;
    if (snprintf(revision_path, sizeof(revision_path), "%s/%s", revisions, basename) >= (int)sizeof(revision_path)) {
        service_error("revision path too long");
        return service_response;
    }

    if (!service_file_exists(revision_path)) {
        ur_permission_mask grants = service_grants();
        char *bytes = NULL;
        int length = 0;
        int rc;

        if (!ur_permission_mask_contains(grants, UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT))) {
            snprintf(service_response, sizeof(service_response),
                "{\"ok\":false,\"error\":\"permission\",\"missing\":\"net.client\","
                "\"message\":\"iris: refusing to fetch '%s'; missing permissions: net.client\\n"
                "       grant with: perm grant net.client\"}", url);
            return service_response;
        }
        rc = js_fetch_blob(url, &bytes, &length);
        if ((rc != 0) || (bytes == NULL)) {
            service_error("revision download failed");
            return service_response;
        }
        if (service_write_bytes(revision_path, bytes, (size_t)length) == 0U) {
            free(bytes);
            service_error("revision staging failed");
            return service_response;
        }
        free(bytes);
    }

    return service_swap_with(entry, revision_path);
}

/* strict, padded base64 (the transport the OTA bridge patches arrive in) */
static int service_base64_decode(const char *text, unsigned char *out, int out_cap)
{
    static const signed char table[256] = {
        /* 0-43 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1
    };
    int value = 0;
    int bits = 0;
    int written = 0;

    for (; *text != '\0'; ++text) {
        signed char digit;

        if ((*text == '=') || (*text == '\n') || (*text == '\r')) {
            continue;
        }
        digit = table[(unsigned char)*text];
        if (digit < 0) {
            return -1;
        }
        value = (value << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= out_cap) {
                return -1;
            }
            out[written++] = (unsigned char)((value >> bits) & 0xFF);
        }
    }
    return written;
}

HOST_KEEP const char *iris_wasm_service_swap_bytes(const char *name, const char *base64)
{
    service_entry *entry;
    unsigned char *bytes;
    int length;
    char revisions[600];
    char revision_path[700];

    if ((name == NULL) || (base64 == NULL)) {
        service_error("service_swap_bytes requires name and payload");
        return service_response;
    }
    entry = service_find(name);
    if (entry == NULL) {
        service_error("service not open");
        return service_response;
    }

    length = (int)(strlen(base64) / 4 * 3 + 3);
    bytes = malloc((size_t)length);
    if (bytes == NULL) {
        service_error("out of memory");
        return service_response;
    }
    length = service_base64_decode(base64, bytes, length);
    if (length <= 0) {
        free(bytes);
        service_error("bad base64 revision payload");
        return service_response;
    }

    snprintf(revisions, sizeof(revisions), "%s/revisions", entry->directory);
    (void)service_mkdir_soft(revisions);
    {
        double now = 0.0;
        (void)ur_platform_get_time(&now);
        snprintf(revision_path, sizeof(revision_path), "%s/embedded-%.0f.ura", revisions, now);
    }
    if (service_write_bytes(revision_path, bytes, (size_t)length) == 0U) {
        free(bytes);
        service_error("revision staging failed");
        return service_response;
    }
    free(bytes);

    return service_swap_with(entry, revision_path);
}

HOST_KEEP const char *iris_wasm_grant(const char *names)
{
    const char *message = NULL;
    char buffer[256];
    char *token;

    if ((names == NULL) || (names[0] == '\0')) {
        return iris_glue_perm_list();
    }
    snprintf(buffer, sizeof(buffer), "%s", names);
    token = strtok(buffer, " ,");
    while (token != NULL) {
        message = iris_glue_perm_update(token, 1, NULL);
        token = strtok(NULL, " ,");
    }
    return (message != NULL) ? message : iris_glue_perm_list();
}

HOST_KEEP const char *iris_wasm_revoke(const char *names)
{
    const char *message = NULL;
    char buffer[256];
    char *token;

    if ((names == NULL) || (names[0] == '\0')) {
        return iris_glue_perm_list();
    }
    snprintf(buffer, sizeof(buffer), "%s", names);
    token = strtok(buffer, " ,");
    while (token != NULL) {
        message = iris_glue_perm_update(token, 0, NULL);
        token = strtok(NULL, " ,");
    }
    return (message != NULL) ? message : iris_glue_perm_list();
}

HOST_KEEP const char *iris_wasm_perms(void)
{
    return iris_glue_perm_list();
}
