/*
 * ura_guest.h — Uracil guest SDK: the platform interface a .ura service
 * binary sees when it runs inside the microkernel (wasm32 target).
 *
 * The functions here mirror the kernel's platform abstraction
 * (include/uracil/platform.h). On wasm32 they are imports under the
 * "uracil" namespace and are serviced by the kernel; a guest compiled
 * against this header imports nothing else from the outside world.
 *
 * The URACIL_* runtime contract (URACIL_SERVICE_NAME, URACIL_FUNCTION_NAME,
 * URACIL_GRANTED_PERMISSIONS, URACIL_STATE_DIR, ...) is delivered through
 * ura_guest_get_env() — the same names the native subprocess contract uses.
 */

#ifndef URACIL_GUEST_H
#define URACIL_GUEST_H

#include <stddef.h>
#include <stdint.h>

#define URA_GUEST_OK 0
#define URA_GUEST_ERROR 1
#define URA_GUEST_NOT_FOUND 2
#define URA_GUEST_DENIED 3

#define URA_USE(value) ((void)(value))

#if defined(__EMSCRIPTEN__)
#define URA_IMPORT(name) __attribute__((import_module("uracil"), import_name(name)))
#else
#define URA_IMPORT(name)
#endif

/* ── environment contract ─────────────────────────────────────────────── */

URA_IMPORT("get_env")
int ura_guest_get_env(const char *name, char *buffer, int buffer_len);

/* ── filesystem (workdir = URACIL_SERVICE_DIR unless path is absolute) ── */

URA_IMPORT("read_file")
int ura_guest_read_file(const char *path, void *out, int capacity);

URA_IMPORT("write_file")
int ura_guest_write_file(const char *path, const void *data, int length);

URA_IMPORT("list_directory")
int ura_guest_list_directory(const char *path, char *buffer, int buffer_len);

URA_IMPORT("remove_file")
int ura_guest_remove_file(const char *path);

URA_IMPORT("create_directory")
int ura_guest_create_directory(const char *path);

/* ── time & randomness ────────────────────────────────────────────────── */

URA_IMPORT("get_time")
double ura_guest_get_time(void);

URA_IMPORT("sleep_ms")
void ura_guest_sleep_ms(int milliseconds);

URA_IMPORT("random_bytes")
int ura_guest_random_bytes(void *buffer, int length);

/* ── network (gated by net.client / net.server depending on direction) ── */

URA_IMPORT("network_request")
int ura_guest_network_request(
    const char *method,
    const char *url,
    const char *headers,
    const void *body,
    int body_len,
    char *response_buf,
    int response_buf_len);

/* ── console (stream 1 = stdout, 2 = stderr) ──────────────────────────── */

URA_IMPORT("write")
int ura_guest_write(int stream, const void *data, int length);

/* ── process end ──────────────────────────────────────────────────────── */

URA_IMPORT("exit")
void ura_guest_exit(int code);

#endif
