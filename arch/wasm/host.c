/*
 * Uracil — .ura wasm32 payload executor (kernel side).
 *
 * For wasm32 service binaries the microkernel performs the role the native
 * stub plays: it instantiates the module from a loaded .ura and services the
 * service's operating-system calls. The module sees exactly one namespace,
 * "uracil.*", mirroring include/uracil/platform.h:
 *
 *   read_file / write_file / list_directory / remove_file / create_directory
 *   get_time / sleep_ms / random_bytes / network_request
 *   write (console) / get_env (the URACIL_* contract) / exit
 *
 * Per-call capability gates are enforced here against the grant mask the
 * kernel hands over at spawn: fs.read, fs.write, clock.read, net.client.
 * Relative and absolute paths resolve into URACIL_SERVICE_DIR, matching the
 * native stub's chdir-into-payload behaviour.
 */

#include <emscripten.h>

#include "host.h"

EM_JS(int, js_ura_spawn,
      (const void *module, int module_length,
       const char *service_name, const char *service_version,
       const char *runtime_abi, const char *mode,
       const char *service_dir, const char *state_dir,
       const char *requested_permissions, const char *granted_permissions,
       unsigned long long grants),
{
    if (!Module.__uraServices) {
        Module.__uraServices = { next: 1, slots: {} };
    }
    var registry = Module.__uraServices;
    var slot = registry.next++;
    var bytes = HEAPU8.slice(module, module + module_length);

    var inst = null;
    var outText = "";
    var exited = false;
    var exitCode = 0;

    var env = {
        URACIL_SERVICE_NAME: UTF8ToString(service_name),
        URACIL_SERVICE_VERSION: UTF8ToString(service_version),
        URACIL_RUNTIME_ABI: UTF8ToString(runtime_abi),
        URACIL_MODE: UTF8ToString(mode),
        URACIL_SERVICE_DIR: UTF8ToString(service_dir),
        URACIL_STATE_DIR: UTF8ToString(state_dir),
        URACIL_ROOTLESS: UTF8ToString(mode) === "rootless" ? "true" : "false",
        URACIL_REQUESTED_PERMISSIONS: UTF8ToString(requested_permissions),
        URACIL_GRANTED_PERMISSIONS: UTF8ToString(granted_permissions)
    };

    var grantBits = BigInt(grants);
    function granted(bit) { return (grantBits & (1n << BigInt(bit))) !== 0n; }
    var PERM_FS_READ = 0, PERM_FS_WRITE = 1, PERM_NET_CLIENT = 2, PERM_CLOCK_READ = 5;

    function mem8() { return new Uint8Array(inst.exports.memory.buffer); }
    function readCStr(ptr) {
        var m = mem8();
        var end = ptr;
        while (m[end]) end++;
        return new TextDecoder().decode(m.subarray(ptr, end));
    }
    function resolvePath(p) {
        p = String(p);
        if (p.indexOf("..") !== -1) return null;
        while (p.length && p[0] === "/") p = p.slice(1);
        return env.URACIL_SERVICE_DIR + "/" + p;
    }
    function writeMem(ptr, data, cap) {
        var m = mem8();
        var count = Math.min(data.length, cap);
        m.set(data.subarray(0, count), ptr);
        return count;
    }
    function writeStr(ptr, cap, text) {
        var encoded = new TextEncoder().encode(text);
        var count = Math.min(encoded.length, cap - 1);
        var m = mem8();
        m.set(encoded.subarray(0, count), ptr);
        m[ptr + count] = 0;
        return count;
    }

    var imports = { uracil: {
        read_file: function (pathPtr, outPtr, cap) {
            if (!granted(PERM_FS_READ)) return 3;
            var path = resolvePath(readCStr(pathPtr));
            if (path === null) return -1;
            var data;
            try { data = Module.FS.readFile(path); } catch (e) { return -1; }
            if (outPtr !== 0 && cap > 0) writeMem(outPtr, data, cap);
            return data.length;
        },
        write_file: function (pathPtr, dataPtr, len) {
            if (!granted(PERM_FS_WRITE)) return 3;
            var path = resolvePath(readCStr(pathPtr));
            if (path === null) return -1;
            try { Module.FS.writeFile(path, mem8().slice(dataPtr, dataPtr + len)); }
            catch (e) { return -1; }
            return len;
        },
        list_directory: function (pathPtr, outPtr, cap) {
            if (!granted(PERM_FS_READ)) return 3;
            var path = resolvePath(readCStr(pathPtr));
            if (path === null) return -1;
            var names;
            try { names = Module.FS.readdir(path); } catch (e) { return -1; }
            return writeStr(outPtr, cap, names.filter(function (n) {
                return n !== "." && n !== "..";
            }).join("\n"));
        },
        remove_file: function (pathPtr) {
            if (!granted(PERM_FS_WRITE)) return 3;
            var path = resolvePath(readCStr(pathPtr));
            if (path === null) return -1;
            try { Module.FS.unlink(path); } catch (e) { return -1; }
            return 0;
        },
        create_directory: function (pathPtr) {
            if (!granted(PERM_FS_WRITE)) return 3;
            var path = resolvePath(readCStr(pathPtr));
            if (path === null) return -1;
            try { Module.FS.mkdirTree(path); } catch (e) { return -1; }
            return 0;
        },
        get_time: function () {
            if (!granted(PERM_CLOCK_READ)) return -1;
            /* Epoch seconds — event ids and LWW timestamps depend on this
             * being wall-clock, not page-relative (a real bug once). */
            return Date.now() / 1000;
        },
        sleep_ms: function (ms) {
            if (!granted(PERM_CLOCK_READ)) return;
            var until = performance.now() + ms;
            while (performance.now() < until) { /* the host paces ticks */ }
        },
        random_bytes: function (outPtr, len) {
            var m = mem8();
            if (typeof crypto !== "undefined" && crypto.getRandomValues) {
                var chunk = new Uint8Array(len);
                crypto.getRandomValues(chunk);
                m.set(chunk, outPtr);
            } else {
                for (var i = 0; i < len; i++) m[outPtr + i] = Math.floor(Math.random() * 256);
            }
            return 0;
        },
        network_request: function (method, url, headers, body, bodyLen, out, cap) {
            if (!granted(PERM_NET_CLIENT)) return 3;
            return -1; /* synchronous network is not offered to services yet */
        },
        write: function (stream, dataPtr, len) {
            outText += new TextDecoder().decode(mem8().subarray(dataPtr, dataPtr + len));
            if (outText.length > 32768) outText = outText.slice(-32768);
            return len;
        },
        get_env: function (namePtr, outPtr, cap) {
            var value = env[readCStr(namePtr)];
            if (value === undefined) return -1;
            return writeStr(outPtr, cap, value);
        },
        exit: function (code) {
            exited = true;
            exitCode = code | 0;
        }
    } };

    try {
        var compiled = new WebAssembly.Module(bytes);
        inst = new WebAssembly.Instance(compiled, imports);
    } catch (e) {
        if (Module.printErr) Module.printErr("[ura] instantiate failed: " + (e && e.message ? e.message : e));
        return 0;
    }

    if (inst.exports._initialize) inst.exports._initialize();

    registry.slots[slot] = {
        inst: inst,
        peekExited: function () { return exited; },
        takeOutput: function () { var t = outText; outText = ""; return t; },
        call: function (name) {
            if (exited || !inst.exports[name]) return -1;
            try { return inst.exports[name]() | 0; } catch (e) {
                exited = true;
                if (Module.printErr) Module.printErr("[ura] fault in " + name + ": " + (e && e.message ? e.message : e));
                return -2;
            }
        },
        callInts: function (name, a, b) {
            if (exited || !inst.exports[name]) return -1;
            try { inst.exports[name](a, b); return 0; } catch (e) {
                exited = true;
                if (Module.printErr) Module.printErr("[ura] fault in " + name + ": " + (e && e.message ? e.message : e));
                return -2;
            }
        },
        copyResponse: function (ptr, destination, capacity) {
            if (!ptr) return 0;
            var m = mem8();
            var end = ptr;
            while (m[end]) end++;
            var len = end - ptr;
            if (len > capacity - 1) len = capacity - 1;
            if (!destination) return len;
            HEAPU8.set(m.subarray(ptr, ptr + len), destination);
            HEAPU8[destination + len] = 0;
            return len;
        },
        exportString: function (name, destination, capacity) {
            if (exited || !inst.exports[name]) return 0;
            try {
                return this.copyResponse(inst.exports[name](), destination, capacity);
            } catch (e) { return 0; }
        }
    };
    return slot;
});

EM_JS(int, js_ura_call, (int slot, const char *name), {
    var slots = Module.__uraServices ? Module.__uraServices.slots : null;
    var rec = slots ? slots[slot] : null;
    if (!rec) return -1;
    return rec.call(UTF8ToString(name));
});

EM_JS(int, js_ura_call_str, (int slot, const char *name, const char *argument, char *out, int capacity), {
    var slots = Module.__uraServices ? Module.__uraServices.slots : null;
    var rec = slots ? slots[slot] : null;
    if (!rec || rec.peekExited()) return -1;
    var inst = rec.inst;
    var fnName = UTF8ToString(name);
    if (!inst.exports[fnName]) return -3;
    try {
        /* NOTE: C `const char *` params arrive as POINTER NUMBERS — decode
         * them first. Argument buffers then live in the GUEST's memory:
         * encode into the guest view, not the kernel's HEAPU8. */
        var argumentText = UTF8ToString(argument);
        var encoded = new TextEncoder().encode(argumentText);
        var ptr = inst.exports.ura_alloc(encoded.length + 1);
        var guest = new Uint8Array(inst.exports.memory.buffer);
        guest.set(encoded, ptr);
        guest[ptr + encoded.length] = 0;
        var response = inst.exports[fnName](ptr, encoded.length);
        inst.exports.ura_free(ptr, encoded.length + 1);
        return rec.copyResponse(response, out, capacity);
    } catch (e) {
        rec.exited = true;
        if (Module.printErr) Module.printErr("[ura] fault in " + fnName + ": " + (e && e.message ? e.message : e));
        return -2;
    }
});

EM_JS(int, js_ura_call_str2, (int slot, const char *name, const char *first, const char *second, char *out, int capacity), {
    var slots = Module.__uraServices ? Module.__uraServices.slots : null;
    var rec = slots ? slots[slot] : null;
    if (!rec || rec.peekExited()) return -1;
    var inst = rec.inst;
    var fnName = UTF8ToString(name);
    if (!inst.exports[fnName]) return -3;
    try {
        var firstText = UTF8ToString(first);
        var secondText = UTF8ToString(second);
        var firstEncoded = new TextEncoder().encode(firstText);
        var secondEncoded = new TextEncoder().encode(secondText);
        var firstPtr = inst.exports.ura_alloc(firstEncoded.length + 1);
        var secondPtr = inst.exports.ura_alloc(secondEncoded.length + 1);
        var guest = new Uint8Array(inst.exports.memory.buffer);
        guest.set(firstEncoded, firstPtr);
        guest[firstPtr + firstEncoded.length] = 0;
        guest.set(secondEncoded, secondPtr);
        guest[secondPtr + secondEncoded.length] = 0;
        var response = inst.exports[fnName](firstPtr, firstEncoded.length, secondPtr, secondEncoded.length);
        inst.exports.ura_free(firstPtr, firstEncoded.length + 1);
        inst.exports.ura_free(secondPtr, secondEncoded.length + 1);
        return rec.copyResponse(response, out, capacity);
    } catch (e) {
        rec.exited = true;
        if (Module.printErr) Module.printErr("[ura] fault in " + fnName + ": " + (e && e.message ? e.message : e));
        return -2;
    }
});

EM_JS(int, js_ura_call_ints, (int slot, const char *name, int a, int b), {
    var slots = Module.__uraServices ? Module.__uraServices.slots : null;
    var rec = slots ? slots[slot] : null;
    if (!rec) return -1;
    return rec.callInts(UTF8ToString(name), a, b);
});

EM_JS(int, js_ura_export_str, (int slot, const char *name, char *destination, int capacity), {
    var slots = Module.__uraServices ? Module.__uraServices.slots : null;
    var rec = slots ? slots[slot] : null;
    if (!rec) return 0;
    return rec.exportString(UTF8ToString(name), destination, capacity);
});

EM_JS(int, js_ura_state, (int slot), {
    var slots = Module.__uraServices ? Module.__uraServices.slots : null;
    var rec = slots ? slots[slot] : null;
    if (!rec) return 0;
    return rec.peekExited() ? 2 : 1;
});

EM_JS(int, js_ura_take_output, (int slot, char *destination, int capacity), {
    var slots = Module.__uraServices ? Module.__uraServices.slots : null;
    var rec = slots ? slots[slot] : null;
    if (!rec) return 0;
    var text = rec.takeOutput();
    if (!text) return 0;
    stringToUTF8(text, destination, capacity);
    var size = lengthBytesUTF8(text);
    return size < capacity ? size : capacity - 1;
});

EM_JS(void, js_ura_kill, (int slot), {
    var registry = Module.__uraServices;
    if (!registry || !registry.slots[slot]) return;
    delete registry.slots[slot];
    try { Module.FS.syncfs(false, function () {}); } catch (e) {}
});

int ur_ura_spawn(const void *module, int module_length,
                 const char *service_name, const char *service_version,
                 const char *runtime_abi, const char *mode,
                 const char *service_dir, const char *state_dir,
                 const char *requested_permissions, const char *granted_permissions,
                 unsigned long long grants)
{
    return js_ura_spawn(module, module_length, service_name, service_version,
                        runtime_abi, mode, service_dir, state_dir,
                        requested_permissions, granted_permissions, grants);
}

int ur_ura_call(int slot, const char *function_name)
{
    return js_ura_call(slot, function_name);
}

int ur_ura_call_str(int slot, const char *function_name, const char *argument,
                    char *out, int capacity)
{
    return js_ura_call_str(slot, function_name, argument, out, capacity);
}

int ur_ura_call_str2(int slot, const char *function_name,
                     const char *first, const char *second,
                     char *out, int capacity)
{
    return js_ura_call_str2(slot, function_name, first, second, out, capacity);
}

int ur_ura_call_ints(int slot, const char *function_name, int first, int second)
{
    return js_ura_call_ints(slot, function_name, first, second);
}

int ur_ura_export_str(int slot, const char *function_name, char *destination, int capacity)
{
    return js_ura_export_str(slot, function_name, destination, capacity);
}

int ur_ura_state(int slot)
{
    return js_ura_state(slot);
}

int ur_ura_take_output(int slot, char *destination, int capacity)
{
    return js_ura_take_output(slot, destination, capacity);
}

void ur_ura_kill(int slot)
{
    js_ura_kill(slot);
}
