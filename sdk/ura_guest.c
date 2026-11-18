/*
 * ura_guest.c — Uracil guest SDK: the C-runtime bridge.
 *
 * A .ura wasm32 service brings its own libc for pure computation, but every
 * operating-system call must reach the microkernel as a uracil.* platform
 * import. This file owns the POSIX surface the C library would otherwise
 * resolve to wasi_snapshot_preview1 / env.* stubs:
 *
 *   open/close/read/write/lseek   → platform file operations, serviced by
 *                                   the kernel against URACIL_SERVICE_DIR
 *   unlink/rmdir/mkdir            → platform file operations
 *   clock_gettime/time/nanosleep  → platform clock
 *   _exit                         → platform exit
 *   isatty/system/chdir/getpid    → inert
 *
 * Linking this in leaves the module's import list as exactly "uracil.*".
 */

#if defined(__EMSCRIPTEN__)

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ura_guest.h"

/* emscripten's memory-growth notifier would otherwise be an env.* import */
void emscripten_notify_memory_growth(void)
{
}

/* ── guest file table ─────────────────────────────────────────────────── */

#define URA_GUEST_FDS 16
#define URA_GUEST_FD_BASE 64
#define URA_GUEST_PATH_MAX 512

typedef struct ura_guest_file {
    int used;
    int writable;
    int dirty;
    char path[URA_GUEST_PATH_MAX];
    unsigned char *data;
    size_t length;
    size_t position;
} ura_guest_file;

static ura_guest_file guest_files[URA_GUEST_FDS];

static ura_guest_file *ura_guest_slot(int fd)
{
    int index = fd - URA_GUEST_FD_BASE;

    if ((index < 0) || (index >= URA_GUEST_FDS) || !guest_files[index].used) {
        return NULL;
    }
    return &guest_files[index];
}

static ura_guest_file *ura_guest_alloc(const char *path, int writable)
{
    int index;

    for (index = 0; index < URA_GUEST_FDS; ++index) {
        if (!guest_files[index].used) {
            ura_guest_file *file = &guest_files[index];
            size_t written = 0U;

            memset(file, 0, sizeof(*file));
            while ((path[written] != '\0') && (written < URA_GUEST_PATH_MAX - 1U)) {
                file->path[written] = path[written];
                written++;
            }
            file->writable = writable;
            file->used = 1;
            return file;
        }
    }
    return NULL;
}

/* ── open / close / read / write / seek ───────────────────────────────── */

int open(const char *path, int flags, ...)
{
    ura_guest_file *file;
    int writable;
    int size;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    writable = ((flags & O_WRONLY) != 0) || ((flags & O_RDWR) != 0) ||
               ((flags & O_CREAT) != 0) || ((flags & O_TRUNC) != 0);

    file = ura_guest_alloc(path, writable ? 1 : 0);
    if (file == NULL) {
        errno = EMFILE;
        return -1;
    }

    size = ura_guest_read_file(path, NULL, 0);
    if (size < 0) {
        if (!writable) {
            memset(file, 0, sizeof(*file));
            errno = ENOENT;
            return -1;
        }
        size = 0;
    }

    if (size > 0) {
        file->data = malloc((size_t)size);
        if (file->data == NULL) {
            memset(file, 0, sizeof(*file));
            errno = ENOMEM;
            return -1;
        }
        if (ura_guest_read_file(path, file->data, size) < 0) {
            free(file->data);
            memset(file, 0, sizeof(*file));
            errno = EIO;
            return -1;
        }
        file->length = (size_t)size;
    }

    if (writable) {
        if ((flags & O_TRUNC) != 0) {
            file->length = 0U;
        } else if ((flags & O_APPEND) != 0) {
            file->position = file->length;
        }
    }

    return URA_GUEST_FD_BASE + (int)(file - guest_files);
}

int close(int fd)
{
    ura_guest_file *file = ura_guest_slot(fd);

    if (file == NULL) {
        errno = EBADF;
        return -1;
    }

    if (file->writable && file->dirty) {
        if (ura_guest_write_file(file->path, file->data, (int)file->length) < 0) {
            errno = EIO;
            return -1;
        }
    }

    free(file->data);
    memset(file, 0, sizeof(*file));
    return 0;
}

ssize_t read(int fd, void *buffer, size_t count)
{
    ura_guest_file *file = ura_guest_slot(fd);
    size_t available;
    size_t amount;

    if (file == NULL) {
        errno = EBADF;
        return -1;
    }

    available = file->length - file->position;
    amount = (count < available) ? count : available;
    if (amount > 0U) {
        memcpy(buffer, file->data + file->position, amount);
        file->position += amount;
    }
    return (ssize_t)amount;
}

ssize_t write(int fd, const void *buffer, size_t count)
{
    ura_guest_file *file;

    if ((fd == 1) || (fd == 2)) {
        int written = ura_guest_write(fd, buffer, (int)count);
        if (written < 0) {
            errno = EIO;
            return -1;
        }
        return (ssize_t)written;
    }

    file = ura_guest_slot(fd);
    if (file == NULL) {
        errno = EBADF;
        return -1;
    }
    if (!file->writable) {
        errno = EBADF;
        return -1;
    }

    if (file->position + count > file->length) {
        size_t grown = file->position + count;
        unsigned char *data = realloc(file->data, grown);

        if (data == NULL) {
            errno = ENOMEM;
            return -1;
        }
        file->data = data;
        if (grown > file->length) {
            memset(file->data + file->length, 0, grown - file->length);
        }
        file->length = grown;
    }

    memcpy(file->data + file->position, buffer, count);
    file->position += count;
    file->dirty = 1;
    return (ssize_t)count;
}

off_t lseek(int fd, off_t offset, int whence)
{
    ura_guest_file *file = ura_guest_slot(fd);

    if (file == NULL) {
        errno = EBADF;
        return -1;
    }

    if (whence == SEEK_SET) {
        file->position = (size_t)((long)offset < 0 ? 0 : (long)offset);
    } else if (whence == SEEK_CUR) {
        long target = (long)file->position + (long)offset;
        file->position = (size_t)(target < 0 ? 0 : target);
    } else if (whence == SEEK_END) {
        long target = (long)file->length + (long)offset;
        file->position = (size_t)(target < 0 ? 0 : target);
    }

    return (off_t)file->position;
}

/* ── file management ──────────────────────────────────────────────────── */

int unlink(const char *path)
{
    if (ura_guest_remove_file(path) != URA_GUEST_OK) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int rmdir(const char *path)
{
    return unlink(path);
}

int mkdir(const char *path, mode_t mode)
{
    URA_USE(mode);
    if (ura_guest_create_directory(path) != URA_GUEST_OK) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int rename(const char *from, const char *to)
{
    URA_USE(from);
    URA_USE(to);
    errno = ENOSYS;
    return -1;
}

/* ── clock ────────────────────────────────────────────────────────────── */

int clock_gettime(clockid_t clock_id, struct timespec *result)
{
    double now;

    URA_USE(clock_id);
    now = ura_guest_get_time();

    if (result != NULL) {
        long seconds = (long)now;
        result->tv_sec = seconds;
        result->tv_nsec = (long)((now - (double)seconds) * 1000000000.0);
    }
    return 0;
}

time_t time(time_t *result)
{
    time_t value = (time_t)ura_guest_get_time();

    if (result != NULL) {
        *result = value;
    }
    return value;
}

int nanosleep(const struct timespec *request, struct timespec *remaining)
{
    URA_USE(remaining);
    if (request != NULL) {
        ura_guest_sleep_ms((int)((request->tv_sec * 1000) + (request->tv_nsec / 1000000)));
    }
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    ura_guest_sleep_ms((int)(seconds * 1000U));
    return 0;
}

/* ── inert surface ────────────────────────────────────────────────────── */

int isatty(int fd)
{
    URA_USE(fd);
    return 0;
}

int system(const char *command)
{
    URA_USE(command);
    return -1;
}

int chdir(const char *path)
{
    URA_USE(path);
    return 0; /* relative paths resolve to URACIL_SERVICE_DIR kernel-side */
}

int getpid(void)
{
    return 1;
}

char *getcwd(char *buffer, size_t size)
{
    if ((buffer == NULL) || (size == 0U)) {
        errno = EINVAL;
        return NULL;
    }
    if (size < 2U) {
        errno = ERANGE;
        return NULL;
    }
    buffer[0] = '/';
    buffer[1] = '\0';
    return buffer;
}

void _exit(int code)
{
    ura_guest_exit(code);
    for (;;) {
    }
}

/* ── fopen over the platform file surface ─────────────────────────────── */
/*
 * musl's internal open path is ENOSYS-stubbed in emscripten's no-filesystem
 * standalone mode, so fopen never reaches the bridge's open(). fopen/freopen
 * are therefore defined here in terms of the platform-backed open() + musl's
 * fdopen(); buffered stdio then flows through the overridden read/write/lseek
 * below.
 */

static int ura_guest_mode_flags(const char *mode)
{
    int flags = 0;
    int plus = 0;
    const char *cursor;

    for (cursor = mode; *cursor != '\0'; ++cursor) {
        if (*cursor == '+') {
            plus = 1;
        }
    }

    if (mode[0] == 'w') {
        flags = plus ? O_RDWR : O_WRONLY;
        flags |= O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags = plus ? O_RDWR : O_WRONLY;
        flags |= O_CREAT | O_APPEND;
    } else {
        flags = plus ? O_RDWR : O_RDONLY;
    }
    return flags;
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *file;
    int fd;

    if ((path == NULL) || (mode == NULL)) {
        errno = EINVAL;
        return NULL;
    }

    fd = open(path, ura_guest_mode_flags(mode), 0666);
    if (fd < 0) {
        return NULL;
    }

    file = fdopen(fd, mode);
    if (file == NULL) {
        close(fd);
        return NULL;
    }
    return file;
}

FILE *freopen(const char *path, const char *mode, FILE *stream)
{
    URA_USE(stream);
    return fopen(path, mode);
}

int fcntl(int fd, int command, ...)
{
    URA_USE(fd);
    URA_USE(command);
    return 0;
}

int __syscall_fcntl(int fd, int command, ...)
{
    URA_USE(fd);
    URA_USE(command);
    return 0;
}

/* ── standalone syscall surface ───────────────────────────────────────── */
/*
 * emscripten's wasmfs/no_fs.c provides the __wasi_* wrappers as WEAK symbols
 * that forward to wasi_snapshot_preview1. Strong definitions here replace
 * the entire surface, and the __syscall_* stubs (which would otherwise be
 * imported as env.__syscall_* due to --import-undefined) resolve internally.
 * The result: no wasi_* or env.* imports remain in the linked module.
 *
 * Note: emscripten's musl stdio (fread/fwrite/fseek/fclose) calls these
 * __wasi_fd_* functions directly, so they route into the guest file table
 * defined above rather than stubbing anything out.
 */

#include <wasi/api.h>

__wasi_errno_t __wasi_fd_write(__wasi_fd_t fd,
                               const __wasi_ciovec_t *iovs,
                               size_t iovs_len,
                               __wasi_size_t *nwritten)
{
    size_t total = 0U;
    size_t index;

    for (index = 0U; index < iovs_len; ++index) {
        const void *buffer = iovs[index].buf;
        size_t length = iovs[index].buf_len;

        if (length == 0U) {
            continue;
        }
        if (write((int)fd, buffer, length) < 0) {
            return ((fd == 1) || (fd == 2)) ? __WASI_ERRNO_IO : __WASI_ERRNO_BADF;
        }
        total += length;
    }

    if (nwritten != NULL) {
        *nwritten = (__wasi_size_t)total;
    }
    return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t __wasi_fd_read(__wasi_fd_t fd,
                              const __wasi_iovec_t *iovs,
                              size_t iovs_len,
                              __wasi_size_t *nread)
{
    size_t total = 0U;
    size_t index;

    for (index = 0U; index < iovs_len; ++index) {
        void *buffer = iovs[index].buf;
        size_t length = iovs[index].buf_len;
        ssize_t taken;

        if (length == 0U) {
            continue;
        }
        taken = read((int)fd, buffer, length);
        if (taken < 0) {
            return __WASI_ERRNO_BADF;
        }
        total += (size_t)taken;
        if ((size_t)taken < length) {
            break; /* EOF reached — stdio sees a short read */
        }
    }

    if (nread != NULL) {
        *nread = (__wasi_size_t)total;
    }
    return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t __wasi_fd_close(__wasi_fd_t fd)
{
    if (close((int)fd) < 0) {
        return __WASI_ERRNO_BADF;
    }
    return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t __wasi_fd_seek(__wasi_fd_t fd,
                              __wasi_filedelta_t offset,
                              __wasi_whence_t whence,
                              __wasi_filesize_t *newoffset)
{
    off_t position = lseek((int)fd, (off_t)offset, (int)whence);

    if (position < 0) {
        return __WASI_ERRNO_INVAL;
    }
    if (newoffset != NULL) {
        *newoffset = (__wasi_filesize_t)position;
    }
    return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t __wasi_fd_fdstat_get(__wasi_fd_t fd, __wasi_fdstat_t *stat)
{
    URA_USE(fd);
    URA_USE(stat);
    return __WASI_ERRNO_NOSYS;
}

_Noreturn void __wasi_proc_exit(__wasi_exitcode_t code)
{
    ura_guest_exit((int)code);
    for (;;) {
    }
}

__wasi_errno_t __wasi_clock_time_get(__wasi_clockid_t clock_id,
                                     __wasi_timestamp_t precision,
                                     __wasi_timestamp_t *result)
{
    URA_USE(clock_id);
    URA_USE(precision);
    if (result != NULL) {
        *result = (__wasi_timestamp_t)(ura_guest_get_time() * 1000000000.0);
    }
    return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t __wasi_random_get(uint8_t *buffer, __wasi_size_t length)
{
    if (ura_guest_random_bytes(buffer, (int)length) != URA_GUEST_OK) {
        return __WASI_ERRNO_IO;
    }
    return __WASI_ERRNO_SUCCESS;
}

int __syscall_unlinkat(int dirfd, const char *path, int flags)
{
    URA_USE(dirfd);
    URA_USE(flags);
    if (ura_guest_remove_file(path) != URA_GUEST_OK) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int __syscall_rmdir(const char *path)
{
    return __syscall_unlinkat(-100, path, 0);
}

#endif /* __EMSCRIPTEN__ */
