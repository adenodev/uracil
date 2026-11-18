/*
 * dg_iris.c — doomgeneric platform layer for iris.
 *
 * Built as a STANDALONE wasm module (doom.wasm / doom.wasm32.ura, GPL-2.0 —
 * it links id's Doom engine) that Uracil hosts: the kernel downloads it
 * through its permission-gated network, loads its bundle manifest, spawns
 * the module, and services its syscalls (WASI: file I/O against the kernel
 * filesystem, clock, randomness, stdout into the shell). The engine's
 * exported entry points are doom_start() / doom_tick() / doom_key() /
 * doom_frame() / doom_state(); each DG_DrawFrame converts the 320x200 RGBA
 * framebuffer into ANSI truecolor text using Unicode half-blocks (U+2580),
 * which the iris terminal renders.
 *
 * Config and savegames are written through the host filesystem, so they
 * persist (IDBFS) alongside the WAD under /urashell/doom/.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "doomkeys.h"
#include "doomgeneric.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define IRIS_KEEP EMSCRIPTEN_KEEPALIVE
#else
#define IRIS_KEEP
#endif

/* System calls reach the kernel through the Uracil guest SDK
 * (uracil/sdk/ura_guest.c), which bridges libc to the uracil.* platform
 * imports. Nothing in this module talks to wasi or a JS host. */

#define IRIS_COLS 80
#define IRIS_ROWS 28
#define IRIS_CELL_MAX 48
#define IRIS_FRAME_SIZE (IRIS_ROWS * (IRIS_COLS * IRIS_CELL_MAX + 16) + 1)

static char iris_frame[IRIS_FRAME_SIZE];

static int doom_ready = 0;

/* ── key queue (down/up pairs fed from the page) ───────────────────────── */

#define IRIS_KEYQ 64
static unsigned short keyq[IRIS_KEYQ];
static unsigned wi = 0;
static unsigned ri = 0;

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    unsigned short data;

    if (ri == wi) {
        return 0;
    }
    data = keyq[ri];
    ri = (ri + 1) % IRIS_KEYQ;
    *pressed = data >> 8;
    *doomKey = (unsigned char)(data & 0xFF);
    return 1;
}

void DG_Init(void)
{
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

void DG_SleepMs(uint32_t ms)
{
    (void)ms; /* the page paces ticks with requestAnimationFrame */
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)ts.tv_sec * 1000ULL) +
                      ((uint64_t)ts.tv_nsec / 1000000ULL));
}

/* ── framebuffer → ANSI truecolor half-blocks ──────────────────────────── */
/* rgba8888: blue bits 0-7, green 8-15, red 16-23. */

static int put(char *dst, int pos, const char *s)
{
    while (*s != '\0' && pos < IRIS_FRAME_SIZE - 1) {
        dst[pos++] = *s++;
    }
    return pos;
}

static int put_int(char *dst, int pos, int v)
{
    char tmp[12];
    int n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && pos < IRIS_FRAME_SIZE - 1) {
        dst[pos++] = tmp[--n];
    }
    return pos;
}

static int put_fg(char *dst, int pos, int r, int g, int b)
{
    pos = put(dst, pos, "\x1b[38;2;");
    pos = put_int(dst, pos, r);
    pos = put(dst, pos, ";");
    pos = put_int(dst, pos, g);
    pos = put(dst, pos, ";");
    pos = put_int(dst, pos, b);
    return put(dst, pos, "m");
}

static int put_bg(char *dst, int pos, int r, int g, int b)
{
    pos = put(dst, pos, "\x1b[48;2;");
    pos = put_int(dst, pos, r);
    pos = put(dst, pos, ";");
    pos = put_int(dst, pos, g);
    pos = put(dst, pos, ";");
    pos = put_int(dst, pos, b);
    return put(dst, pos, "m");
}

static void avg_block(const pixel_t *fb, int x0, int y0, int bw, int bh,
                      int *out_r, int *out_g, int *out_b)
{
    long rs = 0, gs = 0, bs = 0, n = 0;
    int y, x;

    for (y = y0; y < y0 + bh && y < DOOMGENERIC_RESY; y++) {
        const pixel_t *row = fb + (long)y * DOOMGENERIC_RESX;
        for (x = x0; x < x0 + bw && x < DOOMGENERIC_RESX; x++) {
            pixel_t p = row[x];
            rs += (p >> 16) & 0xff;
            gs += (p >> 8) & 0xff;
            bs += p & 0xff;
            n++;
        }
    }
    if (n == 0) {
        n = 1;
    }
    *out_r = (int)(rs / n);
    *out_g = (int)(gs / n);
    *out_b = (int)(bs / n);
}

void DG_DrawFrame(void)
{
    int pos = 0;
    int cx, cy;
    int block_w, block_h;

    if (DG_ScreenBuffer == NULL) {
        return;
    }

    /* ceil so the full framebuffer is always covered (some overlap when
     * the block grid does not divide evenly). */
    block_w = (DOOMGENERIC_RESX + IRIS_COLS - 1) / IRIS_COLS;
    block_h = (DOOMGENERIC_RESY + IRIS_ROWS * 2 - 1) / (IRIS_ROWS * 2);
    if (block_w < 1) {
        block_w = 1;
    }
    if (block_h < 1) {
        block_h = 1;
    }

    for (cy = 0; cy < IRIS_ROWS; cy++) {
        int last_fg = -1;
        int last_bg = -1;
        int last_fg_r = 0, last_fg_g = 0, last_fg_b = 0;
        int last_bg_r = 0, last_bg_g = 0, last_bg_b = 0;
        int top_y = ((cy * 2) * DOOMGENERIC_RESY) / (IRIS_ROWS * 2);
        int bot_y = ((cy * 2 + 1) * DOOMGENERIC_RESY) / (IRIS_ROWS * 2);

        for (cx = 0; cx < IRIS_COLS; cx++) {
            int tr, tg, tb, br, bg, bb;
            int same;
            int x0 = (cx * DOOMGENERIC_RESX) / IRIS_COLS;

            avg_block(DG_ScreenBuffer, x0, top_y, block_w, block_h, &tr, &tg, &tb);
            avg_block(DG_ScreenBuffer, x0, bot_y, block_w, block_h, &br, &bg, &bb);

            same = (abs(tr - br) < 12) && (abs(tg - bg) < 12) && (abs(tb - bb) < 12);

            if (same) {
                /* one flat color: a space painted with the background */
                if (last_bg != 0 || last_bg_r != br || last_bg_g != bg || last_bg_b != bb) {
                    pos = put_bg(iris_frame, pos, br, bg, bb);
                    last_bg = 0;
                    last_bg_r = br; last_bg_g = bg; last_bg_b = bb;
                }
                iris_frame[pos++] = ' ';
            } else {
                if (last_fg != 1 || last_fg_r != tr || last_fg_g != tg || last_fg_b != tb) {
                    pos = put_fg(iris_frame, pos, tr, tg, tb);
                    last_fg = 1;
                    last_fg_r = tr; last_fg_g = tg; last_fg_b = tb;
                }
                if (last_bg != 0 || last_bg_r != br || last_bg_g != bg || last_bg_b != bb) {
                    pos = put_bg(iris_frame, pos, br, bg, bb);
                    last_bg = 0;
                    last_bg_r = br; last_bg_g = bg; last_bg_b = bb;
                }
                pos = put(iris_frame, pos, "\xE2\x96\x80"); /* ▀ */
            }
        }
        pos = put(iris_frame, pos, "\x1b[0m\n");
    }
    iris_frame[pos] = '\0';
}

/* ── exports driven by the page ────────────────────────────────────────── */

IRIS_KEEP int doom_state(void)
{
    return doom_ready ? 1 : 0;
}

IRIS_KEEP int doom_start(void)
{
    static char *argv[4];
    FILE *wad;

    if (doom_ready) {
        return 0;
    }

    wad = fopen("/doom1.wad", "rb");
    if (wad == NULL) {
        return 1; /* iwad not staged yet */
    }
    fclose(wad);

    argv[0] = "doom";
    argv[1] = "-iwad";
    argv[2] = "/doom1.wad";
    argv[3] = NULL;

    doomgeneric_Create(3, argv);
    doom_ready = 1;
    return 0;
}

IRIS_KEEP void doom_tick(void)
{
    if (doom_ready) {
        doomgeneric_Tick();
    }
}

IRIS_KEEP const char *doom_frame(void)
{
    return doom_ready ? iris_frame : "";
}

IRIS_KEEP void doom_key(int key, int pressed)
{
    keyq[wi] = (unsigned short)(((pressed ? 1 : 0) << 8) | (key & 0xFF));
    wi = (wi + 1) % IRIS_KEYQ;
}
