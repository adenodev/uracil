/*
 * ura_stub.c — Uracil Service Binary Stub
 *
 * Compiled into a native executable for a target platform.  At build time
 * the APS package payload is appended after the stub binary together with
 * a 16-byte trailer that stores the payload size and a magic signature.
 *
 * Binary layout:
 *   [compiled stub] [payload entries] [uint64_le payload_size] [8B magic]
 *
 * Payload entries (flat archive, no compression):
 *   For each file:
 *     [uint16_le path_len] [path bytes] [uint32_le data_len] [data bytes]
 *   The first entry is always ".ura.meta" — a line-oriented index produced
 *   by the builder from the APS manifest.
 *
 * Usage:
 *   ./service                          Run the default (entry) function
 *   ./service <function> [args...]     Run a named function
 *   ./service --describe               Show service metadata
 *   ./service --permissions            Show permission grants
 *   ./service --grant <perm> [...]     Grant permissions
 *   ./service --revoke <perm> [...]    Revoke permissions
 *   ./service --version                Show version
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #include <process.h>
  #define PATH_SEP '\\'
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <dirent.h>
  #include <limits.h>
  #define PATH_SEP '/'
#endif

/* ── Limits & magic ───────────────────────────────────────────── */

#define MAX_PATH_LEN  4096
#define MAX_FUNCTIONS 64
#define MAX_PERMS     32
#define MAX_LINE      2048
#define MAX_NAME      256

static const uint8_t URA_MAGIC[8] = {'U','R','A',0x01,'S','V','C',0x00};
#define TRAILER_LEN 16  /* 8 B payload size + 8 B magic */

/* ── Types ────────────────────────────────────────────────────── */

typedef struct {
    char name[MAX_NAME];
    char snippet[MAX_NAME];
    char description[MAX_LINE];
    char requires[MAX_PERMS][64];
    int  num_requires;
} ura_func_t;

typedef struct {
    char       name[MAX_NAME];
    char       version[64];
    char       abi[64];
    char       mode[32];
    char       platform[64];
    char       entry[MAX_NAME];
    ura_func_t funcs[MAX_FUNCTIONS];
    int        num_funcs;
    char       perm_req[MAX_PERMS][64];
    int        num_perm_req;
} ura_meta_t;

/* ── Globals ──────────────────────────────────────────────────── */

static char g_tmpdir[MAX_PATH_LEN];

/* ── Platform helpers ─────────────────────────────────────────── */

static int self_path(char *buf, size_t len) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)len);
    return (n > 0 && n < len) ? 0 : -1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, len - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)len;
    extern int _NSGetExecutablePath(char *, uint32_t *);
    if (_NSGetExecutablePath(buf, &sz) != 0) return -1;
    buf[sz] = '\0';
    return 0;
#else
    (void)buf; (void)len;
    return -1;
#endif
}

static int make_tmpdir(char *buf, size_t len) {
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    snprintf(buf, len, "%sura_%lu", tmp, (unsigned long)GetCurrentProcessId());
    return _mkdir(buf);
#else
    snprintf(buf, len, "/tmp/ura-XXXXXX");
    return mkdtemp(buf) ? 0 : -1;
#endif
}

static void mkdirs(const char *path) {
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = saved;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}

/* Recursive directory removal */
#ifdef _WIN32
static void rmdir_r(const char *path) {
    char pattern[MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryA(path); return; }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char child[MAX_PATH_LEN];
        snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            rmdir_r(child);
        else
            DeleteFileA(child);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(path);
}
#else
static void rmdir_r(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[MAX_PATH_LEN];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rmdir_r(child);
        else
            unlink(child);
    }
    closedir(d);
    rmdir(path);
}
#endif

static void cleanup(void) {
    if (!g_tmpdir[0]) return;
    rmdir_r(g_tmpdir);
    g_tmpdir[0] = '\0';
}

static void sig_handler(int sig) {
    cleanup();
    _exit(128 + sig);
}

/* ── Little-endian readers ────────────────────────────────────── */

static uint16_t read_u16(FILE *f) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return 0;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static uint32_t read_u32(FILE *f) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t read_u64(FILE *f) {
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return 0;
    return (uint64_t)b[0] | ((uint64_t)b[1] << 8) |
           ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24) |
           ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) |
           ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
}

/* ── Payload extraction ───────────────────────────────────────── */

static int extract_payload(const char *exe, const char *tmpdir) {
    FILE *f = fopen(exe, "rb");
    if (!f) { perror("ura: open self"); return -1; }

    /* Read trailer from end of file */
    if (fseek(f, -(long)TRAILER_LEN, SEEK_END) != 0) {
        fclose(f); return -1;
    }
    long file_end = ftell(f) + TRAILER_LEN;
    uint64_t payload_size = read_u64(f);
    uint8_t magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, URA_MAGIC, 8) != 0) {
        fprintf(stderr, "ura: invalid binary — no embedded payload found\n");
        fclose(f); return -1;
    }

    /* Seek to payload start */
    long payload_start = file_end - TRAILER_LEN - (long)payload_size;
    if (payload_start < 0) { fclose(f); return -1; }
    fseek(f, payload_start, SEEK_SET);

    /* Extract entries */
    long consumed = 0;
    while (consumed < (long)payload_size) {
        uint16_t path_len = read_u16(f); consumed += 2;
        if (path_len == 0 || path_len >= MAX_PATH_LEN) break;

        char entry_path[MAX_PATH_LEN];
        if (fread(entry_path, 1, path_len, f) != path_len) break;
        consumed += path_len;
        entry_path[path_len] = '\0';

        /* Reject directory traversal */
        if (strstr(entry_path, "..") || entry_path[0] == '/' || entry_path[0] == '\\') {
            fprintf(stderr, "ura: unsafe path in payload: %s\n", entry_path);
            fclose(f); return -1;
        }

        uint32_t data_len = read_u32(f); consumed += 4;

        /* Build full output path */
        char out[MAX_PATH_LEN];
        snprintf(out, sizeof(out), "%s%c%s", tmpdir, PATH_SEP, entry_path);

        /* Ensure parent directories */
        char parent[MAX_PATH_LEN];
        snprintf(parent, sizeof(parent), "%s", out);
        char *slash = strrchr(parent, PATH_SEP);
        if (slash) { *slash = '\0'; mkdirs(parent); }

        /* Write file */
        FILE *of = fopen(out, "wb");
        if (!of) { perror(out); fclose(f); return -1; }
        char buf[8192];
        uint32_t left = data_len;
        while (left > 0) {
            size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
            size_t got = fread(buf, 1, chunk, f);
            if (got == 0) break;
            fwrite(buf, 1, got, of);
            left -= (uint32_t)got;
            consumed += (long)got;
        }
        fclose(of);
    }

    fclose(f);
    return 0;
}

/* ── Meta parsing (.ura.meta — line-oriented) ─────────────────── */

static void safe_copy(char *dst, const char *src, size_t n) {
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int parse_meta(const char *tmpdir, ura_meta_t *meta) {
    memset(meta, 0, sizeof(*meta));

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%c.ura.meta", tmpdir, PATH_SEP);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ura: missing .ura.meta in payload\n");
        return -1;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;

        char *key = line;
        char *val = strchr(line, ' ');
        if (val) { *val = '\0'; val++; } else { val = (char*)""; }

        if (strcmp(key, "name") == 0) {
            safe_copy(meta->name, val, sizeof(meta->name));
        } else if (strcmp(key, "version") == 0) {
            safe_copy(meta->version, val, sizeof(meta->version));
        } else if (strcmp(key, "abi") == 0) {
            safe_copy(meta->abi, val, sizeof(meta->abi));
        } else if (strcmp(key, "mode") == 0) {
            safe_copy(meta->mode, val, sizeof(meta->mode));
        } else if (strcmp(key, "platform") == 0) {
            safe_copy(meta->platform, val, sizeof(meta->platform));
        } else if (strcmp(key, "entry") == 0) {
            safe_copy(meta->entry, val, sizeof(meta->entry));
        } else if (strcmp(key, "fn") == 0 && meta->num_funcs < MAX_FUNCTIONS) {
            ura_func_t *fn = &meta->funcs[meta->num_funcs];
            char *sp = strchr(val, ' ');
            if (sp) {
                *sp = '\0';
                safe_copy(fn->name, val, sizeof(fn->name));
                safe_copy(fn->snippet, sp + 1, sizeof(fn->snippet));
            } else {
                safe_copy(fn->name, val, sizeof(fn->name));
            }
            meta->num_funcs++;
        } else if (strcmp(key, "desc") == 0) {
            char *sp = strchr(val, ' ');
            char fname[MAX_NAME];
            const char *desc = "";
            if (sp) { *sp = '\0'; safe_copy(fname, val, sizeof(fname)); desc = sp + 1; }
            else { safe_copy(fname, val, sizeof(fname)); }
            for (int i = 0; i < meta->num_funcs; i++) {
                if (strcmp(meta->funcs[i].name, fname) == 0) {
                    safe_copy(meta->funcs[i].description, desc,
                              sizeof(meta->funcs[i].description));
                    break;
                }
            }
        } else if (strcmp(key, "req") == 0) {
            /* "req <funcname> <perm1> <perm2> ..." */
            char *sp = strchr(val, ' ');
            char fname[MAX_NAME];
            if (sp) { *sp = '\0'; safe_copy(fname, val, sizeof(fname)); sp++; }
            else { safe_copy(fname, val, sizeof(fname)); sp = (char*)""; }
            for (int i = 0; i < meta->num_funcs; i++) {
                if (strcmp(meta->funcs[i].name, fname) == 0) {
                    ura_func_t *fn = &meta->funcs[i];
                    /* Tokenise remaining perms */
                    char reqbuf[MAX_LINE];
                    safe_copy(reqbuf, sp, sizeof(reqbuf));
                    char *tok = strtok(reqbuf, " ");
                    while (tok && fn->num_requires < MAX_PERMS) {
                        safe_copy(fn->requires[fn->num_requires], tok, 64);
                        fn->num_requires++;
                        tok = strtok(NULL, " ");
                    }
                    break;
                }
            }
        } else if (strcmp(key, "perm") == 0 && *val) {
            char permbuf[MAX_LINE];
            safe_copy(permbuf, val, sizeof(permbuf));
            char *tok = strtok(permbuf, " ");
            while (tok && meta->num_perm_req < MAX_PERMS) {
                safe_copy(meta->perm_req[meta->num_perm_req], tok, 64);
                meta->num_perm_req++;
                tok = strtok(NULL, " ");
            }
        }
    }
    fclose(f);
    return 0;
}

/* ── Grants management ────────────────────────────────────────── */

static int grants_path(const char *svc, char *buf, size_t len) {
    const char *home;
#ifdef _WIN32
    home = getenv("APPDATA");
    if (!home) return -1;
    snprintf(buf, len, "%s\\ura\\%s\\grants", home, svc);
#else
    home = getenv("HOME");
    if (!home) return -1;
    snprintf(buf, len, "%s/.config/ura/%s/grants", home, svc);
#endif
    return 0;
}

static int load_grants(const char *svc, char g[][64], int *count) {
    *count = 0;
    char path[MAX_PATH_LEN];
    if (grants_path(svc, path, sizeof(path)) != 0) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64];
    while (fgets(line, sizeof(line), f) && *count < MAX_PERMS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0]) {
            safe_copy(g[*count], line, 64);
            (*count)++;
        }
    }
    fclose(f);
    return 0;
}

static int save_grants(const char *svc, char g[][64], int count) {
    char path[MAX_PATH_LEN];
    if (grants_path(svc, path, sizeof(path)) != 0) return -1;
    /* Ensure parent dir */
    char parent[MAX_PATH_LEN];
    snprintf(parent, sizeof(parent), "%s", path);
    char *s = strrchr(parent, PATH_SEP);
    if (s) { *s = '\0'; mkdirs(parent); }
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    for (int i = 0; i < count; i++) fprintf(f, "%s\n", g[i]);
    fclose(f);
    return 0;
}

static int has_grant(char g[][64], int count, const char *perm) {
    for (int i = 0; i < count; i++)
        if (strcmp(g[i], perm) == 0) return 1;
    return 0;
}

/* ── Python discovery ─────────────────────────────────────────── */

static int find_python(char *buf, size_t len) {
    const char *names[] = {
#ifdef _WIN32
        "python3", "python", "py",
#else
        "python3", "python",
#endif
        NULL
    };
    for (int i = 0; names[i]; i++) {
        char cmd[256];
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd), "where %s 2>nul", names[i]);
        FILE *p = _popen(cmd, "r");
#else
        snprintf(cmd, sizeof(cmd), "command -v %s 2>/dev/null", names[i]);
        FILE *p = popen(cmd, "r");
#endif
        if (p) {
            char result[MAX_PATH_LEN];
            if (fgets(result, sizeof(result), p)) {
                result[strcspn(result, "\r\n")] = '\0';
#ifdef _WIN32
                _pclose(p);
#else
                pclose(p);
#endif
                safe_copy(buf, result, len);
                return 0;
            }
#ifdef _WIN32
            _pclose(p);
#else
            pclose(p);
#endif
        }
    }
    return -1;
}

/* ── Function execution ───────────────────────────────────────── */

static int run_function(const char *tmpdir, const ura_meta_t *meta,
                        const ura_func_t *fn, int argc, char **argv) {
    /* Permission check */
    char grants[MAX_PERMS][64];
    int num_grants = 0;
    load_grants(meta->name, grants, &num_grants);
    for (int i = 0; i < fn->num_requires; i++) {
        if (!has_grant(grants, num_grants, fn->requires[i])) {
            fprintf(stderr, "ura: function '%s' requires permission '%s'\n"
                            "     grant with: %s --grant %s\n",
                    fn->name, fn->requires[i],
                    meta->name, fn->requires[i]);
            return 1;
        }
    }

    /* Find python */
    char python[MAX_PATH_LEN];
    if (find_python(python, sizeof(python)) != 0) {
        fprintf(stderr, "ura: python3 not found on PATH\n");
        return 1;
    }

    /* Build snippet path */
    char snippet[MAX_PATH_LEN];
    snprintf(snippet, sizeof(snippet), "%s%c%s", tmpdir, PATH_SEP, fn->snippet);

    fprintf(stderr, "running '%s' inside Uracil %s runtime\n",
            fn->name, meta->mode);

#ifdef _WIN32
    /* Build command line */
    char cmdline[MAX_PATH_LEN * 2];
    int off = snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s\"", python, snippet);
    for (int i = 0; i < argc && off < (int)sizeof(cmdline) - 64; i++)
        off += snprintf(cmdline + off, sizeof(cmdline) - off, " \"%s\"", argv[i]);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, tmpdir, &si, &pi)) {
        fprintf(stderr, "ura: CreateProcess failed (%lu)\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)ec;
#else
    pid_t pid = fork();
    if (pid == 0) {
        /* Child */
        int nargs = 2 + argc + 1;
        char **child_argv = calloc((size_t)nargs, sizeof(char *));
        if (!child_argv) _exit(127);
        child_argv[0] = python;
        child_argv[1] = snippet;
        for (int i = 0; i < argc; i++) child_argv[i + 2] = argv[i];
        child_argv[2 + argc] = NULL;
        if (chdir(tmpdir) != 0) perror("chdir");
        execvp(python, child_argv);
        perror("exec");
        _exit(127);
    } else if (pid < 0) {
        perror("fork");
        return 1;
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

/* ── CLI commands ─────────────────────────────────────────────── */

static void cmd_describe(const ura_meta_t *meta) {
    printf("Uracil Service Binary\n");
    printf("  Name:     %s\n", meta->name);
    printf("  Version:  %s\n", meta->version);
    printf("  ABI:      %s [%s]\n", meta->abi, meta->mode);
    if (meta->platform[0])
        printf("  Platform: %s\n", meta->platform);
    printf("\nFunctions:\n");
    for (int i = 0; i < meta->num_funcs; i++) {
        const ura_func_t *fn = &meta->funcs[i];
        const char *tag = (strcmp(fn->name, meta->entry) == 0) ? " (default)" : "";
        if (fn->description[0])
            printf("  %-20s %s%s\n", fn->name, fn->description, tag);
        else
            printf("  %s%s\n", fn->name, tag);
        if (fn->num_requires > 0) {
            printf("    requires:");
            for (int j = 0; j < fn->num_requires; j++)
                printf(" %s", fn->requires[j]);
            printf("\n");
        }
    }
    char grants[MAX_PERMS][64];
    int ng = 0;
    load_grants(meta->name, grants, &ng);
    printf("\nRequested permissions: ");
    if (meta->num_perm_req == 0) printf("(none)");
    else for (int i = 0; i < meta->num_perm_req; i++)
        printf("%s%s", i ? ", " : "", meta->perm_req[i]);
    printf("\nGranted permissions:   ");
    if (ng == 0) printf("(none)");
    else for (int i = 0; i < ng; i++)
        printf("%s%s", i ? ", " : "", grants[i]);
    printf("\n");
}

static int cmd_grant(const ura_meta_t *meta, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: --grant <perm> [...]\n"); return 1; }
    char grants[MAX_PERMS][64];
    int n = 0;
    load_grants(meta->name, grants, &n);
    for (int i = 0; i < argc; i++) {
        if (!has_grant(grants, n, argv[i]) && n < MAX_PERMS) {
            safe_copy(grants[n], argv[i], 64);
            n++;
            printf("granted: %s\n", argv[i]);
        }
    }
    return save_grants(meta->name, grants, n);
}

static int cmd_revoke(const ura_meta_t *meta, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: --revoke <perm> [...]\n"); return 1; }
    char grants[MAX_PERMS][64];
    int n = 0;
    load_grants(meta->name, grants, &n);
    for (int i = 0; i < argc; i++) {
        for (int j = 0; j < n; j++) {
            if (strcmp(grants[j], argv[i]) == 0) {
                memmove(&grants[j], &grants[j + 1], (size_t)(n - j - 1) * 64);
                n--;
                printf("revoked: %s\n", argv[i]);
                break;
            }
        }
    }
    return save_grants(meta->name, grants, n);
}

static void cmd_permissions(const ura_meta_t *meta) {
    char grants[MAX_PERMS][64];
    int ng = 0;
    load_grants(meta->name, grants, &ng);

    printf("Service: %s\n", meta->name);
    printf("Mode:    %s\n\n", meta->mode);

    printf("Requested:\n");
    if (meta->num_perm_req == 0) printf("  (none)\n");
    else for (int i = 0; i < meta->num_perm_req; i++)
        printf("  %s\n", meta->perm_req[i]);

    printf("\nGranted:\n");
    if (ng == 0) printf("  (none)\n");
    else for (int i = 0; i < ng; i++)
        printf("  %s\n", grants[i]);

    printf("\nPer-function requirements:\n");
    for (int i = 0; i < meta->num_funcs; i++) {
        const ura_func_t *fn = &meta->funcs[i];
        printf("  %s: ", fn->name);
        if (fn->num_requires == 0) {
            printf("(none)\n");
        } else {
            for (int j = 0; j < fn->num_requires; j++)
                printf("%s%s", j ? ", " : "", fn->requires[j]);
            printf("\n");
        }
    }
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 1. Resolve own path */
    char exe[MAX_PATH_LEN];
    if (self_path(exe, sizeof(exe)) != 0) {
        fprintf(stderr, "ura: cannot determine binary path\n");
        return 1;
    }

    /* 2. Create temp dir */
    if (make_tmpdir(g_tmpdir, sizeof(g_tmpdir)) != 0) {
        fprintf(stderr, "ura: cannot create temp directory\n");
        return 1;
    }
    atexit(cleanup);

    /* 3. Extract payload */
    if (extract_payload(exe, g_tmpdir) != 0) {
        fprintf(stderr, "ura: payload extraction failed\n");
        return 1;
    }

    /* 4. Parse meta */
    ura_meta_t meta;
    if (parse_meta(g_tmpdir, &meta) != 0)
        return 1;

    /* 5. Handle flags */
    if (argc >= 2 && strcmp(argv[1], "--describe") == 0) {
        cmd_describe(&meta);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", meta.name, meta.version);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--permissions") == 0) {
        cmd_permissions(&meta);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--grant") == 0)
        return cmd_grant(&meta, argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "--revoke") == 0)
        return cmd_revoke(&meta, argc - 2, argv + 2);

    /* 6. Determine which function to run */
    const char *func_name = meta.entry;
    int func_argc = 0;
    char **func_argv = NULL;

    if (argc >= 2 && argv[1][0] != '-') {
        /* Check if first arg matches a function name */
        int is_func = 0;
        for (int i = 0; i < meta.num_funcs; i++) {
            if (strcmp(meta.funcs[i].name, argv[1]) == 0) {
                is_func = 1;
                break;
            }
        }
        if (is_func) {
            func_name = argv[1];
            func_argc = argc - 2;
            func_argv = argv + 2;
        } else {
            /* Not a function name — pass everything as args to entry fn */
            func_argc = argc - 1;
            func_argv = argv + 1;
        }
    } else if (argc >= 2) {
        func_argc = argc - 1;
        func_argv = argv + 1;
    }

    /* Find the function descriptor */
    const ura_func_t *fn = NULL;
    for (int i = 0; i < meta.num_funcs; i++) {
        if (strcmp(meta.funcs[i].name, func_name) == 0) {
            fn = &meta.funcs[i];
            break;
        }
    }
    if (!fn) {
        fprintf(stderr, "ura: unknown function '%s'\n\navailable:\n", func_name);
        for (int i = 0; i < meta.num_funcs; i++)
            fprintf(stderr, "  %s\n", meta.funcs[i].name);
        return 1;
    }

    /* 7. Run it */
    return run_function(g_tmpdir, &meta, fn, func_argc, func_argv);
}
