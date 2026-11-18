/*
 * iris bundle — the Nim shell hosted as an in-process Uracil service.
 *
 * The shell (examples/iris) is compiled from Nim to C and linked into this
 * wasm module. This glue layer connects it to the runtime:
 *   - the ur_service_bundle descriptor the kernel inspects (describe,
 *     permissions, requested grants)
 *   - capability envelopes routed through the real permission functions
 *     (ur_runtime_grant/revoke_permissions, ur_permission_load_grants)
 *   - the *_wasm_* exports the host page calls, returning pointers into
 *     stable shell-owned buffers that JS reads immediately
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "uracil/bundle.h"
#include "uracil/campaign.h"
#include "uracil/json.h"
#include "uracil/loader.h"
#include "uracil/permissions.h"
#include "uracil/platform.h"
#include "uracil/runtime.h"
#include "uracil/ura.h"

#include "host.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define IRIS_KEEP EMSCRIPTEN_KEEPALIVE
#else
#define IRIS_KEEP
#endif

/* Nim exports (see examples/iris/iris.nim) */
extern const char *iris_boot(void);
extern const char *iris_exec(const char *line);
extern const char *iris_key(int key);
extern const char *iris_screen(void);
extern const char *iris_tick(void);
extern void iris_doom_key(int key, int pressed);
extern void iris_set_grants(unsigned long long mask);
extern void iris_set_state(const char *dir);
extern void iris_set_size(int w, int h);

/* lives in arch/wasm/platform.c */
extern void js_sync_state(void);

static const char *const IRIS_SERVICE = "iris.adeno.ltd";

/* ── bundle descriptor ─────────────────────────────────────────────────── */

static int iris_fn_boot(const ur_service_context *context)
{
    fputs(iris_boot(), context->out);
    return 0;
}

static int iris_fn_meta(const ur_service_context *context)
{
    fprintf(context->out,
        "service=%s state=%s rootless=%s\n",
        context->bundle->service_name,
        context->state_directory,
        context->rootless ? "true" : "false");
    return 0;
}

static const ur_function_descriptor iris_functions[] = {
    {
        .name = "boot",
        .description = "Boot the shell: runtime manifest + tetra banner.",
        .required_permissions = UR_PERMISSION_MASK_NONE,
        .entry = iris_fn_boot
    },
    {
        .name = "exec",
        .description = "Execute one command line (capability-gated per command).",
        .required_permissions = UR_PERMISSION_MASK_NONE,
        .entry = iris_fn_meta
    },
    {
        .name = "key",
        .description = "Feed one key to the open TUI app.",
        .required_permissions = UR_PERMISSION_MASK_NONE,
        .entry = iris_fn_meta
    },
    {
        .name = "screen",
        .description = "Read the current TUI frame as JSON.",
        .required_permissions = UR_PERMISSION_MASK_NONE,
        .entry = iris_fn_meta
    },
};

static const ur_service_bundle iris_bundle = {
    .service_name = "iris.adeno.ltd",
    .service_version = "0.1.0",
    .description = "Iris — capability shell for the Hermes enclave (Nim).",
    .runtime_abi = "uracil.service.v1",
    .hotswap_group = "iris.adeno.ltd",
    .payload_revision = "iris-001",
    .requested_permissions =
        UR_PERMISSION_MASK(UR_PERMISSION_FS_READ) |
        UR_PERMISSION_MASK(UR_PERMISSION_FS_WRITE) |
        UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT) |
        UR_PERMISSION_MASK(UR_PERMISSION_CLOCK_READ),
    .function_count = UR_ARRAY_LEN(iris_functions),
    .functions = iris_functions
};

const ur_service_bundle *ur_iris_bundle(void)
{
    return &iris_bundle;
}

/* ── glue helpers called from Nim ──────────────────────────────────────── */

/* All glue returns share one reused buffer: the caller (Nim) copies the
 * string immediately, so nothing outlives the next call. */
static char *glue_buffer = NULL;

static const char *glue_take(char *malloced)
{
    if (malloced == NULL) {
        return "";
    }
    free(glue_buffer);
    glue_buffer = malloced;
    return glue_buffer;
}

IRIS_KEEP const char *iris_glue_list_dir(const char *path)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);

    if (stream != NULL) {
        ur_platform_list_directory(path, stream);
        fclose(stream);
    }
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_perm_update(const char *name, int grant, unsigned long long *out_mask)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);
    const char *names[1];
    ur_permission_mask mask = UR_PERMISSION_MASK_NONE;

    names[0] = name;

    if (stream != NULL) {
        if (grant) {
            ur_runtime_grant_permissions(stream, &iris_bundle, 1, names);
        } else {
            ur_runtime_revoke_permissions(stream, &iris_bundle, 1, names);
        }
        fclose(stream);
    }

    js_sync_state();
    ur_permission_load_grants(iris_bundle.service_name, iris_bundle.requested_permissions, &mask);
    if (out_mask != NULL) {
        *out_mask = (unsigned long long)mask;
    }
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_perm_list(void)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);

    if (stream != NULL) {
        ur_runtime_print_permissions(stream, &iris_bundle);
        fclose(stream);
    }
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_describe(void)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);

    if (stream != NULL) {
        ur_runtime_print_bundle(stream, &iris_bundle);
        fclose(stream);
    }
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_state_dir(void)
{
    static char state[512];

    if (ur_platform_state_dir(state, sizeof(state)) != UR_OK) {
        snprintf(state, sizeof(state), "/urashell");
    }
    return state;
}

IRIS_KEEP const char *iris_glue_kernel_log(void)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);

    if (stream != NULL) {
        fprintf(stream, "grant manifest %s/grants/%s.perm", iris_glue_state_dir(), IRIS_SERVICE);
        fclose(stream);
    }
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_campaign(void)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);

    if (stream != NULL) {
        ur_campaign_kernel campaign;
        ur_status status = ur_campaign_kernel_load("iris-demo", &campaign);

        if (status != UR_OK) {
            status = ur_campaign_kernel_boot(
                "iris-demo", IRIS_SERVICE, "wasm32", UR_CAMPAIGN_KERNEL_ABI, "none", &campaign);
        }
        if (status == UR_OK) {
            ur_campaign_kernel_print(stream, &campaign);
        } else {
            fprintf(stream, "campaign kernel unavailable: %s", ur_status_string(status));
        }
        fclose(stream);
    }
    return glue_take(buf);
}

/* ── campaign lifecycle: hold a service bundle (Kairos dry run) ────────── */
/*
 * Exercises the real campaign machinery end to end:
 *   campaign stage     — boot/load the campaign kernel, write the Kairos
 *                        enclave bundle manifest into its artifacts dir,
 *                        attach the artifact, transition to RUNNING
 *   campaign load      — run the kernel's bundle loader over the staged
 *                        directory and report exactly what the kernel sees,
 *                        including declared capabilities that have no Uracil
 *                        equivalent (these are silently dropped by the loader)
 *   campaign complete  — phase COMPLETED + artifact cleanup
 *   campaign destroy   — remove the campaign state entirely
 */

#define IRIS_CAMPAIGN_ID "iris-demo"
#define IRIS_CAMPAIGN_BUNDLE "kairos-enclave"

static const char iris_campaign_manifest[] =
    "{\n"
    "  \"name\": \"ai.adeno.kairos-enclave\",\n"
    "  \"version\": \"0.3.0\",\n"
    "  \"description\": \"Client-side encrypted enclave for Kairos events: AES-256-GCM payloads, CAuth JWT key derivation, monotonic sync. Staged into a Uracil campaign as a dry run.\",\n"
    "  \"runtime\": {\n"
    "    \"name\": \"uracil\",\n"
    "    \"abi\": \"uracil.service.v1\",\n"
    "    \"mode\": \"rootless\",\n"
    "    \"hotswap_group\": \"ai.adeno.kairos-enclave\",\n"
    "    \"payload_revision\": \"dry-run\"\n"
    "  },\n"
    "  \"service\": {\n"
    "    \"entry_function\": \"initialize\",\n"
    "    \"functions\": [\n"
    "      { \"name\": \"initialize\", \"description\": \"Initialize with a CAuth JWT; derive the AES-256-GCM key via HKDF.\", \"required_permissions\": [\"storage.indexeddb\", \"crypto.subtle\"] },\n"
    "      { \"name\": \"saveEvent\", \"description\": \"Encrypt an event payload and store the sealed blob.\", \"required_permissions\": [\"storage.indexeddb\", \"crypto.subtle\"] },\n"
    "      { \"name\": \"getEvents\", \"description\": \"Decrypt and return stored events.\", \"required_permissions\": [\"storage.indexeddb\", \"crypto.subtle\"] },\n"
    "      { \"name\": \"sync\", \"description\": \"Push and pull sealed events, merging with conflict resolution.\", \"required_permissions\": [\"storage.indexeddb\", \"crypto.subtle\", \"network.fetch\"] },\n"
    "      { \"name\": \"signOut\", \"description\": \"Clear local sealed data and end the session.\", \"required_permissions\": [\"storage.indexeddb\"] }\n"
    "    ]\n"
    "  },\n"
    "  \"permissions\": {\n"
    "    \"requested\": [\"storage.indexeddb\", \"crypto.subtle\", \"network.fetch\", \"worker.self\"]\n"
    "  }\n"
    "}\n";

static ur_status iris_campaign_ensure(ur_campaign_kernel *campaign)
{
    ur_status status;

    status = ur_campaign_kernel_load(IRIS_CAMPAIGN_ID, campaign);
    if ((status == UR_OK) &&
        (campaign->phase != UR_CAMPAIGN_PHASE_COMPLETED) &&
        (campaign->phase != UR_CAMPAIGN_PHASE_FAILED)) {
        return UR_OK;
    }

    return ur_campaign_kernel_boot(
        IRIS_CAMPAIGN_ID, IRIS_SERVICE, "wasm32", UR_CAMPAIGN_KERNEL_ABI, "none", campaign);
}

static int iris_mkdir_soft(const char *path)
{
    if ((mkdir(path, 0755) == 0) || (errno == EEXIST)) {
        return 0;
    }
    return -1;
}

static size_t iris_write_file_bytes(const char *path, const void *data, size_t length)
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

static size_t iris_campaign_write_file(const char *path, const char *data)
{
    return iris_write_file_bytes(path, data, strlen(data));
}

static char *iris_campaign_read_file(const char *path, size_t *out_length)
{
    FILE *file = fopen(path, "rb");
    long file_size;
    char *buffer;

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
    if (fread(buffer, 1U, (size_t)file_size, file) != (size_t)file_size) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);

    buffer[file_size] = '\0';
    if (out_length != NULL) {
        *out_length = (size_t)file_size;
    }
    return buffer;
}

static void iris_campaign_collect_unknown(
    const ur_json_node *array,
    const char **names,
    size_t *count,
    size_t max)
{
    size_t index;
    size_t length;

    if (array == NULL) {
        return;
    }

    length = ur_json_array_length(array);
    for (index = 0U; index < length; ++index) {
        const char *name = ur_json_string(ur_json_array_get(array, index));
        size_t seen;

        if (name == NULL) {
            continue;
        }
        if (ur_permission_from_name(name) < UR_PERMISSION_COUNT) {
            continue;
        }

        for (seen = 0U; seen < *count; ++seen) {
            if (strcmp(names[seen], name) == 0) {
                break;
            }
        }
        if ((seen == *count) && (*count < max)) {
            names[(*count)++] = name;
        }
    }
}

static void iris_campaign_report_unknown(FILE *stream, const char *manifest_path)
{
    const ur_json_node *root;
    const ur_json_node *permissions;
    const ur_json_node *service;
    const ur_json_node *functions;
    const char *names[16];
    size_t count = 0U;
    size_t index;
    size_t length = 0U;
    char *data = iris_campaign_read_file(manifest_path, &length);

    if (data == NULL) {
        fprintf(stream, "  declared capabilities: manifest unreadable\n");
        return;
    }

    root = ur_json_parse(data, length);
    free(data);
    if (root == NULL) {
        fprintf(stream, "  declared capabilities: manifest unparsable\n");
        return;
    }

    permissions = ur_json_get(root, "permissions");
    if (permissions != NULL) {
        iris_campaign_collect_unknown(
            ur_json_get(permissions, "requested"), names, &count, 16U);
    }

    service = ur_json_get(root, "service");
    if (service != NULL) {
        functions = ur_json_get(service, "functions");
        length = ur_json_array_length(functions);
        for (index = 0U; index < length; ++index) {
            iris_campaign_collect_unknown(
                ur_json_get(ur_json_array_get(functions, index), "required_permissions"),
                names, &count, 16U);
        }
    }

    if (count == 0U) {
        fprintf(stream, "  unresolved declarations: (none)\n");
    } else {
        fprintf(stream, "  unresolved declarations (%zu — silently dropped by the loader):\n", count);
        for (index = 0U; index < count; ++index) {
            fprintf(stream, "    - %s — no Uracil equivalent\n", names[index]);
        }
    }

    ur_json_free(root);
}

IRIS_KEEP const char *iris_glue_campaign_stage(void)
{
    char bundle_dir[UR_CAMPAIGN_MAX_PATH];
    char manifest_path[UR_CAMPAIGN_MAX_PATH];
    char artifacts[UR_CAMPAIGN_MAX_PATH];
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);
    ur_campaign_kernel campaign;
    ur_status status;
    size_t written;

    if (stream == NULL) {
        return NULL;
    }

    status = iris_campaign_ensure(&campaign);
    if (status != UR_OK) {
        fprintf(stream, "campaign kernel unavailable: %s\n", ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }

    if (ur_campaign_kernel_artifacts_path(IRIS_CAMPAIGN_ID, artifacts, sizeof(artifacts)) != UR_OK) {
        fprintf(stream, "campaign artifacts path unavailable\n");
        fclose(stream);
        return glue_take(buf);
    }

    written = (size_t)snprintf(bundle_dir, sizeof(bundle_dir), "%s/%s", artifacts, IRIS_CAMPAIGN_BUNDLE);
    if ((written == 0U) || (written >= sizeof(bundle_dir))) {
        fprintf(stream, "campaign bundle path too long\n");
        fclose(stream);
        return glue_take(buf);
    }
    (void)iris_mkdir_soft(bundle_dir);

    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", bundle_dir);
    written = iris_campaign_write_file(manifest_path, iris_campaign_manifest);
    if (written == 0U) {
        fprintf(stream, "failed to write %s\n", manifest_path);
        fclose(stream);
        return glue_take(buf);
    }

    status = ur_campaign_kernel_attach_artifact(&campaign, bundle_dir);
    if (status == UR_OK) {
        status = ur_campaign_kernel_transition(&campaign, UR_CAMPAIGN_PHASE_RUNNING);
    }
    if (status != UR_OK) {
        fprintf(stream, "campaign transition failed: %s\n", ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }

    fprintf(stream, "campaign:  %s · phase %s\n", campaign.campaign_id, ur_campaign_phase_name(campaign.phase));
    fprintf(stream, "staged:    %s\n", bundle_dir);
    fprintf(stream, "           manifest.json (%zu bytes) — ai.adeno.kairos-enclave 0.3.0\n", written);
    fprintf(stream, "artifacts: %zu attached · state persists under the IDBFS mount\n", campaign.artifact_count);
    fprintf(stream, "next:      campaign load\n");

    js_sync_state();
    fclose(stream);
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_campaign_load(void)
{
    char artifacts[UR_CAMPAIGN_MAX_PATH];
    char bundle_dir[UR_CAMPAIGN_MAX_PATH];
    char manifest_path[UR_CAMPAIGN_MAX_PATH];
    char pending[256];
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);
    ur_loaded_bundle *loaded = NULL;
    ur_status status;
    size_t index;

    if (stream == NULL) {
        return NULL;
    }

    if (ur_campaign_kernel_artifacts_path(IRIS_CAMPAIGN_ID, artifacts, sizeof(artifacts)) != UR_OK) {
        fprintf(stream, "campaign artifacts path unavailable\n");
        fclose(stream);
        return glue_take(buf);
    }

    snprintf(bundle_dir, sizeof(bundle_dir), "%s/%s", artifacts, IRIS_CAMPAIGN_BUNDLE);
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", bundle_dir);

    status = ur_loader_load_bundle(bundle_dir, &loaded);
    if (status != UR_OK || loaded == NULL) {
        fprintf(stream, "no staged bundle at %s (%s)\n", bundle_dir, ur_status_string(status));
        fprintf(stream, "run: campaign stage\n");
        fclose(stream);
        return glue_take(buf);
    }

    fprintf(stream, "held service:  %s %s\n",
        loaded->bundle.service_name, loaded->bundle.service_version);
    fprintf(stream, "  runtime:     %s · hotswap %s · revision %s\n",
        loaded->bundle.runtime_abi, loaded->bundle.hotswap_group, loaded->bundle.payload_revision);
    fprintf(stream, "  description: %s\n",
        loaded->bundle.description != NULL ? loaded->bundle.description : "");
    fprintf(stream, "  functions:   %zu\n", loaded->bundle.function_count);
    for (index = 0U; index < loaded->bundle.function_count; ++index) {
        const ur_function_descriptor *fn = &loaded->bundle.functions[index];
        ur_permission_mask_format(fn->required_permissions, pending, sizeof(pending));
        fprintf(stream, "    - %-10s [requires: %s]\n",
            fn->name != NULL ? fn->name : "(unnamed)", pending);
    }
    ur_permission_mask_format(loaded->bundle.requested_permissions, pending, sizeof(pending));
    fprintf(stream, "  resolved mask: %s\n", pending);

    iris_campaign_report_unknown(stream, manifest_path);

    ur_loader_free_bundle(loaded);
    fclose(stream);
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_campaign_complete(void)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);
    ur_campaign_kernel campaign;
    ur_status status;
    size_t removed = 0U;

    if (stream == NULL) {
        return NULL;
    }

    status = ur_campaign_kernel_load(IRIS_CAMPAIGN_ID, &campaign);
    if (status != UR_OK) {
        fprintf(stream, "no campaign state (%s)\n", ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }

    status = ur_campaign_kernel_complete(&campaign, stream, &removed);
    if (status != UR_OK) {
        fprintf(stream, "complete failed: %s\n", ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }

    fprintf(stream, "campaign:  %s · phase %s\n", campaign.campaign_id, ur_campaign_phase_name(campaign.phase));
    fprintf(stream, "cleaned:   %zu artifact(s) removed from the sandbox\n", removed);
    fprintf(stream, "next:      campaign stage (fresh) or campaign destroy\n");

    js_sync_state();
    fclose(stream);
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_campaign_destroy(void)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);
    ur_campaign_kernel campaign;
    ur_status status;

    if (stream == NULL) {
        return NULL;
    }

    status = ur_campaign_kernel_load(IRIS_CAMPAIGN_ID, &campaign);
    if (status != UR_OK) {
        fprintf(stream, "no campaign state (%s)\n", ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }

    status = ur_campaign_kernel_destroy(&campaign, true, stream, NULL);
    if (status != UR_OK) {
        fprintf(stream, "destroy failed: %s\n", ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }

    fprintf(stream, "campaign state for '%s' removed (state file + artifacts dir)\n", IRIS_CAMPAIGN_ID);

    js_sync_state();
    fclose(stream);
    return glue_take(buf);
}

/* ── hosted doom: a .ura service binary, run by the microkernel ────────── */
/*
 * `tui doom` runs the real engine as a Uracil service:
 *   1. `net.client` gates the download: doom-engine-1.9.0.wasm32.ura comes
 *      through the platform network and is staged as a campaign artifact
 *   2. ur_ura_load() verifies the container (trailer + .ura.meta) and the
 *      kernel resolves the service's per-function requirements from the meta
 *   3. the payload (doom1.wad, manifest.json) is extracted into
 *      URACIL_SERVICE_DIR; the entry function runs once its requires are
 *      granted, and every platform call the engine makes is serviced by the
 *      kernel under the service's grant mask
 *   4. ticks and frames flow through exports named by the meta — nothing in
 *      the executor is doom-specific
 */

#define IRIS_DOOM_FRAME_CAP 131072
#define IRIS_DOOM_OUT_CAP 16384

#define IRIS_DOOM_URA_URL "/doom/doom-engine-1.9.0.wasm32.ura"
#define IRIS_DOOM_URA_FILE "doom-engine-1.9.0.wasm32.ura"
#define IRIS_DOOM_SERVICE_DIR "service"

static int doom_slot = 0;
static char doom_frame_buffer[IRIS_DOOM_FRAME_CAP];
static char doom_output_buffer[IRIS_DOOM_OUT_CAP];

/* defined in the gated-download section below */
int js_fetch_blob(const char *url, char **out_ptr, int *out_len);

static void iris_perms_join(const char names[][32], int count, char *buffer, size_t buffer_size)
{
    int index;
    size_t used = 0U;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }
    buffer[0] = '\0';

    for (index = 0; index < count; ++index) {
        int written = snprintf(buffer + used, buffer_size - used, "%s%s",
            (index == 0) ? "" : ",", names[index]);

        if ((written < 0) || ((size_t)written >= (buffer_size - used))) {
            break;
        }
        used += (size_t)written;
    }
}

static void iris_perms_join_mask(ur_permission_mask mask, char *buffer, size_t buffer_size)
{
    size_t index;
    size_t used = 0U;
    int first = 1;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }
    buffer[0] = '\0';

    for (index = 0U; index < UR_PERMISSION_COUNT; ++index) {
        const char *name;
        int written;

        if ((mask & UR_PERMISSION_MASK(index)) == 0U) {
            continue;
        }
        name = ur_permission_name((ur_permission_id)index);
        written = snprintf(buffer + used, buffer_size - used, "%s%s", first ? "" : ",", name);
        if ((written < 0) || ((size_t)written >= (buffer_size - used))) {
            break;
        }
        used += (size_t)written;
        first = 0;
    }
}

static int iris_doom_file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static int iris_doom_download(const char *url, const char *fs_path, FILE *log)
{
    char *bytes = NULL;
    int length = 0;
    int rc;
    size_t written;

    if (iris_doom_file_exists(fs_path)) {
        long size;
        FILE *file = fopen(fs_path, "rb");

        if (file == NULL) {
            fprintf(log, "[doom] cached payload unreadable: %s\n", fs_path);
            return -1;
        }
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fclose(file);
        fprintf(log, "[doom] cached: %s (%ld bytes)\n", fs_path, size);
        return 0;
    }

    rc = js_fetch_blob(url, &bytes, &length);
    if ((rc != 0) || (bytes == NULL)) {
        fprintf(log, "[doom] download failed for %s (rc=%d)\n", url, rc);
        return -1;
    }

    written = iris_write_file_bytes(fs_path, bytes, (size_t)length);
    free(bytes);
    if (written == 0U) {
        fprintf(log, "[doom] staging failed for %s\n", fs_path);
        return -1;
    }

    fprintf(log, "[doom] downloaded %s · %.1f KB via the gated net (net.client)\n",
        url, (double)length / 1024.0);
    return 0;
}

IRIS_KEEP const char *iris_glue_doom_open(void)
{
    char artifacts[UR_CAMPAIGN_MAX_PATH];
    char stage_dir[UR_CAMPAIGN_MAX_PATH];
    char service_dir[UR_CAMPAIGN_MAX_PATH];
    char ura_path[UR_CAMPAIGN_MAX_PATH];
    char pending[256];
    char missing[160];
    char requires_text[160];
    char requested_text[160];
    char granted_text[160];
    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);
    ur_campaign_kernel campaign;
    ur_loaded_bundle *loaded = NULL;
    ur_ura_service *service = NULL;
    ur_permission_mask mask = UR_PERMISSION_MASK_NONE;
    ur_permission_mask needed = UR_PERMISSION_MASK_NONE;
    ur_permission_mask missing_mask;
    ur_status status;
    size_t index;
    int rc;

    if (stream == NULL) {
        return NULL;
    }

    if ((doom_slot != 0) && (ur_ura_state(doom_slot) == UR_URA_STATE_READY)) {
        fprintf(stream, "[doom] service already running on slot %d\n", doom_slot);
        fclose(stream);
        return glue_take(buf);
    }

    /* 1 — the download is gated by net.client */
    ur_permission_load_grants(iris_bundle.service_name, iris_bundle.requested_permissions, &mask);
    if (!ur_permission_mask_contains(mask, UR_PERMISSION_MASK(UR_PERMISSION_NET_CLIENT))) {
        fprintf(stream, "iris: refusing to run 'tui doom'; missing permissions: net.client\n"
                        "       grant with: perm grant net.client\n");
        fclose(stream);
        return glue_take(buf);
    }

    /* 2 — stage the .ura into the campaign's artifacts directory */
    (void)ur_platform_ensure_state_dirs();

    status = iris_campaign_ensure(&campaign);
    if (status != UR_OK) {
        fprintf(stream, "[doom] campaign kernel unavailable: %s\n", ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }
    if ((ur_campaign_kernel_artifacts_path(IRIS_CAMPAIGN_ID, artifacts, sizeof(artifacts)) != UR_OK) ||
        (snprintf(stage_dir, sizeof(stage_dir), "%s/doom", artifacts) >= (int)sizeof(stage_dir))) {
        fprintf(stream, "[doom] campaign path unavailable\n");
        fclose(stream);
        return glue_take(buf);
    }
    (void)iris_mkdir_soft(stage_dir);

    snprintf(ura_path, sizeof(ura_path), "%s/%s", stage_dir, IRIS_DOOM_URA_FILE);
    snprintf(service_dir, sizeof(service_dir), "%s/%s", stage_dir, IRIS_DOOM_SERVICE_DIR);

    if (iris_doom_download(IRIS_DOOM_URA_URL, ura_path, stream) != 0) {
        fclose(stream);
        return glue_take(buf);
    }

    (void)ur_campaign_kernel_attach_artifact(&campaign, ura_path);
    status = ur_campaign_kernel_transition(&campaign, UR_CAMPAIGN_PHASE_RUNNING);
    if (status != UR_OK) {
        fprintf(stream, "[doom] campaign transition failed: %s\n", ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }

    /* 3 — load the .ura: the kernel inspects the service it is asked to run */
    status = ur_ura_load(ura_path, &service);
    if ((status != UR_OK) || (service == NULL)) {
        fprintf(stream, "[doom] .ura load failed at %s (%s)\n", ura_path, ur_status_string(status));
        fclose(stream);
        return glue_take(buf);
    }

    fprintf(stream, "[doom] ura: %s %s · %s · platform %s\n",
        service->name, service->version, service->abi, service->platform);
    for (index = 0U; index < (size_t)service->function_count; ++index) {
        ur_ura_function_requires_format(&service->functions[index], requires_text, sizeof(requires_text));
        fprintf(stream, "  fn %-11s requires: %s\n",
            service->functions[index].name, requires_text);
    }
    {
        const char *unknown[8];
        int unknown_count = ur_ura_unknown_requirements(service, unknown, 8);
        int scan;

        for (scan = 0; scan < unknown_count; ++scan) {
            fprintf(stream, "  note: '%s' has no Uracil equivalent (ignored)\n", unknown[scan]);
        }
    }

    /* the requirements of the functions iris drives */
    {
        static const char *const driven[] = { "doom_start", "doom_tick", "doom_key", "doom_frame" };
        size_t driven_index;

        for (driven_index = 0U; driven_index < sizeof(driven) / sizeof(driven[0]); ++driven_index) {
            const ur_ura_function *function = ur_ura_find_function(service, driven[driven_index]);

            if (function != NULL) {
                needed |= ur_ura_function_mask(function);
            }
        }
    }
    missing_mask = needed & ~mask;
    if (missing_mask != UR_PERMISSION_MASK_NONE) {
        char grant_line[160];
        char *token;

        ur_permission_mask_format(missing_mask, missing, sizeof(missing));
        ur_permission_mask_format(missing_mask, grant_line, sizeof(grant_line));
        fprintf(stream, "iris: refusing to run 'tui doom'; missing permissions: %s\n", missing);
        fprintf(stream, "       grant with: perm grant");
        token = strtok(grant_line, ", ");
        while (token != NULL) {
            fprintf(stream, " %s", token);
            token = strtok(NULL, ", ");
        }
        fprintf(stream, "\n");
        ur_ura_free(service);
        fclose(stream);
        return glue_take(buf);
    }

    /* 4 — extract the payload into URACIL_SERVICE_DIR */
    status = ur_ura_extract_payload(service, service_dir);
    if (status != UR_OK) {
        fprintf(stream, "[doom] payload extraction failed (%s)\n", ur_status_string(status));
        ur_ura_free(service);
        fclose(stream);
        return glue_take(buf);
    }

    /* the extracted manifest still loads through the bundle loader */
    status = ur_loader_load_bundle(service_dir, &loaded);
    if ((status == UR_OK) && (loaded != NULL)) {
        ur_permission_mask_format(loaded->bundle.requested_permissions, pending, sizeof(pending));
        fprintf(stream, "[doom] bundle: %s %s · %zu functions · declared: %s · campaign %s (%s)\n",
            loaded->bundle.service_name, loaded->bundle.service_version,
            loaded->bundle.function_count, pending,
            campaign.campaign_id, ur_campaign_phase_name(campaign.phase));
        ur_loader_free_bundle(loaded);
    }

    /* 5 — spawn the module; the kernel services its platform calls */
    iris_perms_join(service->requested, service->requested_count, requested_text, sizeof(requested_text));
    iris_perms_join_mask(mask, granted_text, sizeof(granted_text));

    doom_slot = ur_ura_spawn(service->module, (int)service->module_length,
        service->name, service->version, service->abi, service->mode,
        service_dir, iris_glue_state_dir(), requested_text, granted_text,
        (unsigned long long)mask);
    if (doom_slot == 0) {
        fprintf(stream, "[doom] spawn failed (module rejected by the kernel)\n");
        ur_ura_free(service);
        fclose(stream);
        return glue_take(buf);
    }

    rc = ur_ura_call(doom_slot, (service->entry[0] != '\0') ? service->entry : "doom_start");
    if (rc != 0) {
        fprintf(stream, "[doom] entry function failed (rc=%d)\n", rc);
        ur_ura_kill(doom_slot);
        doom_slot = 0;
        ur_ura_free(service);
        fclose(stream);
        return glue_take(buf);
    }

    fprintf(stream, "[doom] running on slot %d · %s %s · syscalls serviced by the kernel\n",
        doom_slot, service->name, service->version);

    doom_output_buffer[0] = '\0';
    if (ur_ura_take_output(doom_slot, doom_output_buffer, sizeof(doom_output_buffer)) > 0) {
        fprintf(stream, "%s", doom_output_buffer);
    }
    (void)ur_ura_export_str(doom_slot, "doom_frame", doom_frame_buffer, sizeof(doom_frame_buffer));

    js_sync_state();
    ur_ura_free(service);
    fclose(stream);
    return glue_take(buf);
}

IRIS_KEEP const char *iris_glue_doom_tick(void)
{
    if (doom_slot == 0) {
        return "";
    }
    if (ur_ura_state(doom_slot) == UR_URA_STATE_READY) {
        (void)ur_ura_call(doom_slot, "doom_tick");
        (void)ur_ura_export_str(doom_slot, "doom_frame", doom_frame_buffer, sizeof(doom_frame_buffer));
    }
    return doom_frame_buffer;
}

IRIS_KEEP const char *iris_glue_doom_frame(void)
{
    if (doom_slot == 0) {
        return "";
    }
    (void)ur_ura_export_str(doom_slot, "doom_frame", doom_frame_buffer, sizeof(doom_frame_buffer));
    return doom_frame_buffer;
}

IRIS_KEEP const char *iris_glue_doom_take_output(void)
{
    doom_output_buffer[0] = '\0';
    if (doom_slot != 0) {
        (void)ur_ura_take_output(doom_slot, doom_output_buffer, sizeof(doom_output_buffer));
    }
    return doom_output_buffer;
}

IRIS_KEEP int iris_glue_doom_state(void)
{
    return (doom_slot == 0) ? UR_URA_STATE_NONE : ur_ura_state(doom_slot);
}

IRIS_KEEP int iris_glue_doom_active(void)
{
    return ((doom_slot != 0) && (ur_ura_state(doom_slot) == UR_URA_STATE_READY)) ? 1 : 0;
}

IRIS_KEEP void iris_glue_doom_key(int key, int pressed)
{
    if (doom_slot != 0) {
        (void)ur_ura_call_ints(doom_slot, "doom_key", key, pressed);
    }
}

IRIS_KEEP void iris_glue_doom_close(void)
{
    if (doom_slot != 0) {
        ur_ura_kill(doom_slot);
        doom_slot = 0;
        js_sync_state();
    }
}

/* ── gated byte download (kernel platform network, net.client gated) ───── */

#if defined(__EMSCRIPTEN__)
EM_ASYNC_JS(int, js_fetch_blob, (const char *url, char **out_ptr, int *out_len), {
    try {
        const resp = await fetch(UTF8ToString(url));
        if (!resp.ok) return resp.status;
        const buf = new Uint8Array(await resp.arrayBuffer());
        const ptr = _malloc(buf.length);
        HEAPU8.set(buf, ptr);
        setValue(out_ptr, ptr, '*');
        setValue(out_len, buf.length, 'i32');
        return 0;
    } catch (e) {
        return -1;
    }
});
#else
int js_fetch_blob(const char *url, char **out_ptr, int *out_len)
{
    (void)url; (void)out_ptr; (void)out_len;
    return -1;
}
#endif

/* ── host-page exports ─────────────────────────────────────────────────── */

IRIS_KEEP const char *iris_wasm_boot(void)
{
    static int booted = 0;
    ur_permission_mask mask = UR_PERMISSION_MASK_NONE;

    if (!booted) {
        ur_platform_init();
        booted = 1;
    }

    ur_permission_load_grants(iris_bundle.service_name, iris_bundle.requested_permissions, &mask);
    iris_set_grants((unsigned long long)mask);
    iris_set_state(iris_glue_state_dir());
    return iris_boot();
}

IRIS_KEEP const char *iris_wasm_exec(const char *line)
{
    return iris_exec(line);
}

IRIS_KEEP const char *iris_wasm_key(int key)
{
    return iris_key(key);
}

IRIS_KEEP const char *iris_wasm_screen(void)
{
    return iris_screen();
}

IRIS_KEEP const char *iris_wasm_tick(void)
{
    return iris_tick();
}

IRIS_KEEP void iris_wasm_doom_key(int key, int pressed)
{
    iris_doom_key(key, pressed);
}

IRIS_KEEP void iris_wasm_set_size(int w, int h)
{
    iris_set_size(w, h);
}
