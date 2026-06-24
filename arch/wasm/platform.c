/*
 * Uracil – WASM platform abstraction layer.
 *
 * Provides host adapter implementations for the WASM target.
 * Uses JavaScript imports for filesystem, clock, and network operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "uracil/platform.h"

/* ── JavaScript imports ───────────────────────────────────────── */

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

/* Filesystem operations */
EM_JS(int, js_fs_list_dir, (const char *path, char *buf, int buf_len), {
    const pathStr = UTF8ToString(path);
    // In a real implementation, this would use IndexedDB or the File System Access API
    // For now, return a mock directory listing
    const mockEntries = [
        { name: ".", isDir: true },
        { name: "..", isDir: true },
        { name: "manifest.json", isDir: false, size: 1024 },
        { name: "main.py", isDir: false, size: 2048 },
    ];
    let result = "";
    for (const entry of mockEntries) {
        result += entry.name + "\t" + (entry.isDir ? "dir" : "file") + "\t" + (entry.size || 0) + "\n";
    }
    stringToUTF8(result, buf, buf_len);
    return 0;
});

EM_JS(int, js_fs_read_file, (const char *path, char *buf, int buf_len, int *out_len), {
    const pathStr = UTF8ToString(path);
    // Mock file reading - in reality would read from IndexedDB
    const mockContent = "mock file content for " + pathStr;
    stringToUTF8(mockContent, buf, buf_len);
    if (out_len) setValue(out_len, mockContent.length, 'i32');
    return 0;
});

EM_JS(int, js_fs_write_file, (const char *path, const char *data, int data_len), {
    const pathStr = UTF8ToString(path);
    const dataStr = UTF8ToString(data, data_len);
    // Mock file writing - in reality would write to IndexedDB
    console.log("Writing to " + pathStr + ": " + dataStr.substring(0, 100));
    return 0;
});

EM_JS(int, js_fs_remove, (const char *path), {
    const pathStr = UTF8ToString(path);
    console.log("Removing " + pathStr);
    return 0;
});

EM_JS(int, js_fs_mkdir, (const char *path), {
    const pathStr = UTF8ToString(path);
    console.log("Creating directory " + pathStr);
    return 0;
});

/* Clock operations */
EM_JS(double, js_clock_now, (), {
    return Date.now() / 1000.0;
});

/* Network operations */
EM_JS(int, js_net_request, (const char *method, const char *url, const char *headers, const char *body, int body_len, char *response_buf, int response_buf_len, int *out_len), {
    const methodStr = UTF8ToString(method);
    const urlStr = UTF8ToString(url);
    console.log("Network request: " + methodStr + " " + urlStr);
    const mockResponse = '{"status": "ok", "mock": true}';
    stringToUTF8(mockResponse, response_buf, response_buf_len);
    return 0;
});

/* Random number generation */
EM_JS(void, js_random_bytes, (char *buf, int len), {
    const array = new Uint8Array(len);
    crypto.getRandomValues(array);
    HEAPU8.set(array, buf);
});

#else
/* Stubs for non-Emscripten builds */
static int js_fs_list_dir(const char *path, char *buf, int buf_len) { return -1; }
static int js_fs_read_file(const char *path, char *buf, int buf_len, int *out_len) { return -1; }
static int js_fs_write_file(const char *path, const char *data, int data_len) { return -1; }
static int js_fs_remove(const char *path) { return -1; }
static int js_fs_mkdir(const char *path) { return -1; }
static double js_clock_now(void) { return 0; }
static int js_net_request(const char *method, const char *url, const char *headers, const char *body, int body_len, char *response_buf, int response_buf_len, int *out_len) { return -1; }
static void js_random_bytes(char *buf, int len) { memset(buf, 0, len); }
#endif

/* ── Platform implementation ──────────────────────────────────── */

ur_status ur_platform_init(void) {
    return UR_OK;
}

void ur_platform_cleanup(void) {
    /* Nothing to clean up */
}

ur_status ur_platform_list_directory(const char *path, FILE *out) {
    char buf[4096];
    int result = js_fs_list_dir(path, buf, sizeof(buf));
    if (result == 0) {
        fprintf(out, "%s", buf);
    }
    return result == 0 ? UR_OK : UR_IO_ERROR;
}

ur_status ur_platform_read_file(const char *path, void **out_data, size_t *out_size) {
    char buf[8192];
    int len = 0;
    int result = js_fs_read_file(path, buf, sizeof(buf), &len);
    if (result == 0 && len > 0) {
        *out_data = malloc(len + 1);
        if (*out_data == NULL) {
            return UR_OUT_OF_MEMORY;
        }
        memcpy(*out_data, buf, len);
        ((char *)*out_data)[len] = '\0';
        *out_size = len;
        return UR_OK;
    }
    return UR_IO_ERROR;
}

ur_status ur_platform_write_file(const char *path, const void *data, size_t size) {
    int result = js_fs_write_file(path, (const char *)data, (int)size);
    return result == 0 ? UR_OK : UR_IO_ERROR;
}

ur_status ur_platform_remove_file(const char *path) {
    int result = js_fs_remove(path);
    return result == 0 ? UR_OK : UR_IO_ERROR;
}

ur_status ur_platform_create_directory(const char *path) {
    int result = js_fs_mkdir(path);
    return result == 0 ? UR_OK : UR_OK; /* Ignore errors for mkdir */
}

ur_status ur_platform_get_time(double *out_timestamp) {
    *out_timestamp = js_clock_now();
    return UR_OK;
}

ur_status ur_platform_sleep_ms(uint32_t ms) {
    /* In WASM, we can't really sleep synchronously.
     * This would need to be async in a real implementation. */
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
) {
    int out_len = 0;
    int result = js_net_request(method, url, headers, (const char *)body, (int)body_len, response_buf, (int)response_buf_len, &out_len);
    if (result == 0 && out_len > 0) {
        *out_response_len = out_len;
        return UR_OK;
    }
    return UR_IO_ERROR;
}

ur_status ur_platform_random_bytes(void *buf, size_t len) {
    js_random_bytes((char *)buf, (int)len);
    return UR_OK;
}

ur_status ur_platform_get_env(const char *name, char *buf, size_t buf_len) {
    /* Environment variables not available in WASM by default */
    (void)name;
    (void)buf;
    (void)buf_len;
    return UR_NOT_SUPPORTED;
}

ur_status ur_platform_set_env(const char *name, const char *value) {
    (void)name;
    (void)value;
    return UR_NOT_SUPPORTED;
}

ur_status ur_platform_spawn_process(const char *command, char *const argv[], const char *envp[], int *out_pid) {
    (void)command;
    (void)argv;
    (void)envp;
    (void)out_pid;
    return UR_NOT_SUPPORTED;
}

ur_status ur_platform_wait_process(int pid, int *out_status) {
    (void)pid;
    (void)out_status;
    return UR_NOT_SUPPORTED;
}

ur_status ur_platform_kill_process(int pid, int signal) {
    (void)pid;
    (void)signal;
    return UR_NOT_SUPPORTED;
}

const char *ur_platform_get_temp_dir(void) {
    return "/tmp";
}

const char *ur_platform_get_cwd(void) {
    return "/";
}

ur_status ur_platform_set_cwd(const char *path) {
    (void)path;
    return UR_NOT_SUPPORTED;
}
