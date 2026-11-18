/*
 * Uracil — .ura wasm32 payload executor (kernel side).
 *
 * The kernel cannot instantiate wasm itself; the platform layer does it on
 * the kernel's behalf (host-mediated spawn), then the kernel drives the
 * module. The module's entire interface is the uracil.* platform namespace
 * implemented in host.c, and the URACIL_* contract from the .ura.meta.
 */

#ifndef URACIL_WASM_HOST_H
#define URACIL_WASM_HOST_H

#define UR_URA_STATE_NONE 0
#define UR_URA_STATE_READY 1
#define UR_URA_STATE_EXITED 2

/* Spawn a service module. Returns a slot id, 0 on failure. */
int ur_ura_spawn(const void *module, int module_length,
                 const char *service_name, const char *service_version,
                 const char *runtime_abi, const char *mode,
                 const char *service_dir, const char *state_dir,
                 const char *requested_permissions, const char *granted_permissions,
                 unsigned long long grants);

/* Call an exported function by name (resolved from .ura.meta by the caller). */
int ur_ura_call(int slot, const char *function_name);

/* Call an exported function with two int arguments (key events). */
int ur_ura_call_ints(int slot, const char *function_name, int first, int second);

/*
 * Call an exported function with string arguments. The kernel allocates the
 * argument buffers in guest memory, writes, calls, and frees; the guest
 * treats them as borrowed. The export must return a NUL-terminated string
 * pointer, which is copied into `out` (kernel memory). Returns the copied
 * length, or a negative code on fault/no-slot.
 */
int ur_ura_call_str(int slot, const char *function_name, const char *argument,
                    char *out, int capacity);
int ur_ura_call_str2(int slot, const char *function_name,
                     const char *first, const char *second,
                     char *out, int capacity);

/* Copy a C string returned by an export (e.g. doom_frame) into kernel memory. */
int ur_ura_export_str(int slot, const char *function_name, char *destination, int capacity);

/* 0 = no such slot, 1 = running, 2 = exited. */
int ur_ura_state(int slot);

/* Drain the service's console output (uracil.write) into kernel memory. */
int ur_ura_take_output(int slot, char *destination, int capacity);

/* Drop the slot and flush its files. */
void ur_ura_kill(int slot);

#endif
