/*
 * Uracil – WASM platform abstraction (Emscripten).
 *
 * Real browser backends for the wasm target:
 *   - filesystem: the Emscripten FS, with the kernel state directory
 *     (/urashell) mounted as IDBFS by the host page, so grant files and
 *     shell files persist in IndexedDB across reloads. Standard stdio and
 *     opendir() work against it.
 *   - network: real fetch() (suspends via Asyncify). The response is a
 *     line envelope: "<status> <ok|http-error>\n<raw body>" or "ERR <msg>\n"
 *   - clock: Date.now(), randomness: crypto.getRandomValues()
 *   - environment: a JS-side map (URACIL_* parity with the native platform)
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "uracil/platform.h"

#if defined(__EMSCRIPTEN__)

#include <emscripten.h>

EM_JS(void, js_sync_state, (), {
    try {
        if (typeof FS !== 'undefined' && FS.syncfs) FS.syncfs(false, function () {});
    } catch (e) { /* memory-only fallback */ }
});

EM_JS(double, js_clock_now, (), { return Date.now() / 1000.0; });

EM_JS(void, js_random_bytes, (char *buf, int len), {
    const array = new Uint8Array(len);
    if (globalThis.crypto && crypto.getRandomValues) crypto.getRandomValues(array);
    else for (let i = 0; i < len; i++) array[i] = Math.floor(Math.random() * 256);
    HEAPU8.set(array, buf);
});

EM_ASYNC_JS(int, js_fetch, (const char *verb, const char *url, const char *headers,
                            const char *body, int body_len,
                            char *resp_buf, int resp_buf_len, int *out_len), {
    const methodStr = UTF8ToString(verb) || "GET";
    const urlStr = UTF8ToString(url);
    const cap = resp_buf_len - 1;
    let head, text;
    try {
        const opts = { method: methodStr, headers: {} };
        const headersStr = headers ? UTF8ToString(headers) : "";
        if (headersStr && headersStr !== "{}") {
            try { Object.assign(opts.headers, JSON.parse(headersStr)); } catch (e) {}
        }
        if (body && body_len > 0) opts.body = UTF8ToString(body, body_len);
        const resp = await fetch(urlStr, opts);
        head = resp.status + (resp.ok ? " ok\n" : " http-error\n");
        text = await resp.text();
    } catch (e) {
        head = "ERR " + String(e) + "\n";
        text = "";
    }
    if (head.length > cap) head = head.slice(0, cap);
    if (text.length > cap - head.length) text = text.slice(0, cap - head.length);
    stringToUTF8(head + text, resp_buf, resp_buf_len);
    if (out_len) setValue(out_len, head.length + text.length, "i32");
    return 0;
});

EM_JS(int, js_env_get, (const char *name, char *buf, int buf_len), {
    const value = (globalThis.__uracilEnv || {})[UTF8ToString(name)];
    if (value === undefined) return 0;
    stringToUTF8(String(value), buf, buf_len);
    return 1;
});

EM_JS(int, js_env_set, (const char *name, const char *value), {
    globalThis.__uracilEnv = globalThis.__uracilEnv || {};
    globalThis.__uracilEnv[UTF8ToString(name)] = UTF8ToString(value);
    return 1;
});

#else
/* Non-Emscripten builds never include this file; keep link-safe stubs. */
static void js_sync_state(void) {}
static double js_clock_now(void) { return 0.0; }
static void js_random_bytes(char *buf, int len) { memset(buf, 0, (size_t)len); }
static int js_fetch(const char *v, const char *u, const char *h, const char *b, int bl,
                    char *o, int ol, int *out) {
    (void)v; (void)u; (void)h; (void)b; (void)bl; (void)o; (void)ol; (void)out;
    return -1;
}
static int js_env_get(const char *n, char *b, int l) { (void)n; (void)b; (void)l; return 0; }
static int js_env_set(const char *n, const char *v) { (void)n; (void)v; return 0; }
#endif

#define UR_WASM_STATE_DIR "/urashell"

static void ur_platform_sanitize_service_name(const char *service_name, char *buffer, size_t buffer_size)
{
    size_t index = 0U;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return;
    }

    while ((service_name != NULL) && (*service_name != '\0') && (index + 1U < buffer_size)) {
        const char current = *service_name++;

        if (((current >= 'a') && (current <= 'z')) ||
            ((current >= 'A') && (current <= 'Z')) ||
            ((current >= '0') && (current <= '9')) ||
            (current == '-') ||
            (current == '_')) {
            buffer[index++] = current;
        } else {
            buffer[index++] = '-';
        }
    }

    buffer[index] = '\0';
}

static ur_status ur_platform_make_directory(const char *path)
{
    if (mkdir(path, 0755) == 0) {
        return UR_OK;
    }

    if (errno == EEXIST) {
        return UR_OK;
    }

    return UR_IO_ERROR;
}

ur_status ur_platform_state_dir(char *buffer, size_t buffer_size)
{
    const int written = snprintf(buffer, buffer_size, "%s", UR_WASM_STATE_DIR);

    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

ur_status ur_platform_permission_manifest_path(
    const char *service_name,
    char *buffer,
    size_t buffer_size
)
{
    char state_dir[512];
    char sanitized_name[256];
    int written;

    if ((service_name == NULL) || (buffer == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    if (ur_platform_state_dir(state_dir, sizeof(state_dir)) != UR_OK) {
        return UR_IO_ERROR;
    }

    ur_platform_sanitize_service_name(service_name, sanitized_name, sizeof(sanitized_name));
    written = snprintf(buffer, buffer_size, "%s/grants/%s.perm", state_dir, sanitized_name);

    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return UR_IO_ERROR;
    }

    return UR_OK;
}

ur_status ur_platform_ensure_state_dirs(void)
{
    char grants_dir[512];
    int written;

    if (ur_platform_make_directory(UR_WASM_STATE_DIR) != UR_OK) {
        return UR_IO_ERROR;
    }

    written = snprintf(grants_dir, sizeof(grants_dir), "%s/grants", UR_WASM_STATE_DIR);
    if ((written < 0) || ((size_t)written >= sizeof(grants_dir))) {
        return UR_IO_ERROR;
    }

    return ur_platform_make_directory(grants_dir);
}

ur_status ur_platform_list_directory(const char *path, FILE *stream)
{
    DIR *directory;
    struct dirent *entry;

    if ((path == NULL) || (stream == NULL)) {
        return UR_INVALID_ARGUMENT;
    }

    directory = opendir(path);
    if (directory == NULL) {
        fprintf(stream, "(no such directory: %s)", path);
        return UR_IO_ERROR;
    }

    while ((entry = readdir(directory)) != NULL) {
        if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }

        fprintf(stream, "- %s\n", entry->d_name);
    }

    closedir(directory);
    return UR_OK;
}

ur_status ur_platform_read_file(const char *path, void **out_data, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long file_size;
    void *buffer;
    size_t bytes_read;

    if (file == NULL) {
        return UR_IO_ERROR;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return UR_IO_ERROR;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return UR_IO_ERROR;
    }

    rewind(file);

    buffer = malloc((size_t)file_size + 1);
    if (buffer == NULL) {
        fclose(file);
        return UR_OUT_OF_MEMORY;
    }

    bytes_read = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);

    if (bytes_read != (size_t)file_size) {
        free(buffer);
        return UR_IO_ERROR;
    }

    ((char *)buffer)[bytes_read] = '\0';
    *out_data = buffer;
    *out_size = bytes_read;
    return UR_OK;
}

ur_status ur_platform_write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    size_t written;

    if (file == NULL) {
        return UR_IO_ERROR;
    }

    written = fwrite(data, 1, size, file);
    fclose(file);

    return (written == size) ? UR_OK : UR_IO_ERROR;
}

ur_status ur_platform_remove_file(const char *path)
{
    return (remove(path) == 0) ? UR_OK : UR_IO_ERROR;
}

ur_status ur_platform_create_directory(const char *path)
{
    return ur_platform_make_directory(path);
}

ur_status ur_platform_get_time(double *out_timestamp)
{
    *out_timestamp = js_clock_now();
    return UR_OK;
}

ur_status ur_platform_sleep_ms(uint32_t ms)
{
#if defined(__EMSCRIPTEN__)
    emscripten_sleep(ms);
#endif
    return UR_OK;
}

ur_status ur_platform_network_request(
    const char *method,
    const char *url,
    const char *headers,
    const void *body,
    size_t body_len,
    char *response_buf,
    size_t response_buf_len,
    size_t *out_response_len
)
{
    int out_len = 0;
    int rc = js_fetch(method, url, headers, (const char *)body, (int)body_len,
                      response_buf, (int)response_buf_len, &out_len);
    if (rc != 0) {
        return UR_NOT_SUPPORTED;
    }
    if (out_response_len != NULL) {
        *out_response_len = (size_t)out_len;
    }
    return UR_OK;
}

ur_status ur_platform_random_bytes(void *buf, size_t len)
{
    js_random_bytes((char *)buf, (int)len);
    return UR_OK;
}

ur_status ur_platform_get_env(const char *name, char *buf, size_t buf_len)
{
    if (!js_env_get(name, buf, (int)buf_len)) {
        return UR_NOT_FOUND;
    }
    return UR_OK;
}

ur_status ur_platform_set_env(const char *name, const char *value)
{
    return js_env_set(name, value) ? UR_OK : UR_ERROR;
}

ur_status ur_platform_spawn_process(const char *command, char *const argv[], const char *envp[], int *out_pid)
{
    (void)command;
    (void)argv;
    (void)envp;
    (void)out_pid;
    return UR_NOT_SUPPORTED;
}

ur_status ur_platform_wait_process(int pid, int *out_status)
{
    (void)pid;
    (void)out_status;
    return UR_NOT_SUPPORTED;
}

ur_status ur_platform_kill_process(int pid, int signal)
{
    (void)pid;
    (void)signal;
    return UR_NOT_SUPPORTED;
}

const char *ur_platform_get_temp_dir(void)
{
    return "/tmp";
}

const char *ur_platform_get_cwd(void)
{
    return "/";
}

ur_status ur_platform_set_cwd(const char *path)
{
    return (chdir(path) == 0) ? UR_OK : UR_IO_ERROR;
}

ur_status ur_platform_init(void)
{
    /* The host page mounts /urashell (IDBFS) before calling into the kernel;
     * this hook just guarantees the directory skeleton exists. */
    return ur_platform_ensure_state_dirs();
}

void ur_platform_cleanup(void)
{
    js_sync_state();
}
