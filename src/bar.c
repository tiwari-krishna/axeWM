#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include "axe.h"
#include "config.h" // bar_* settings

static FT_Library ft_library;
static FT_Face ft_face; // NULL if font load failed - drawing text becomes a no-op, bar still shows colored blocks
static int baseline_y;

static char cmd_output[256] = "";

static int status_fd = -1;
static pid_t status_pid = -1;
static char status_line_buf[512];
static size_t status_line_len = 0;
static uint32_t status_epoch = 0; // bumped on every status text change - see redraw()'s dedup check

// Spawn bar_status_cmd once, left running for the lifetime of this axe
// process. Its stdout is piped back to us non-blocking; bar_status_readable()
// turns each newline-terminated line into the new status text. Runs in its
// own process group (setpgid) so bar_kill_status() can reach a backgrounded
// job the script itself might spawn (e.g. `sleep infinity &`), not just the
// top-level shell.
static void spawn_status(void) {
    if(bar_status_cmd == NULL) return;

    int fds[2];
    if(pipe(fds) != 0) {
        fprintf(stderr, "bar: pipe() failed for status command\n");
        return;
    }

    pid_t pid = fork();
    if(pid < 0) {
        fprintf(stderr, "bar: fork() failed for status command\n");
        close(fds[0]);
        close(fds[1]);
        return;
    }

    if(pid == 0) {
        setpgid(0, 0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execlp("/bin/sh", "/bin/sh", "-c", bar_status_cmd, NULL);
        fprintf(stderr, "bar: exec failed for status command '%s'\n", bar_status_cmd);
        _exit(EXIT_FAILURE);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC); // don't leak this fd across restart_axe()'s execvp
    status_fd = fds[0];
    status_pid = pid;
}

int bar_status_fd(void) {
    return status_fd;
}

// Called from main.c's event loop when poll() says status_fd is readable.
void bar_status_readable(void) {
    char chunk[256];
    ssize_t n = read(status_fd, chunk, sizeof(chunk));

    if(n <= 0) {
        if(n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            // Script exited or the pipe broke - stop polling it. Bar keeps
            // showing its last known text rather than going blank.
            close(status_fd);
            status_fd = -1;
        }
        return;
    }

    for(ssize_t i = 0; i < n; i++) {
        if(chunk[i] == '\n') {
            if(status_line_len > 0) {
                status_line_buf[status_line_len] = '\0';
                strncpy(cmd_output, status_line_buf, sizeof(cmd_output) - 1);
                cmd_output[sizeof(cmd_output) - 1] = '\0';
                status_epoch++;
                bar_redraw_all();
            }
            status_line_len = 0;
        } else if(status_line_len < sizeof(status_line_buf) - 1) {
            status_line_buf[status_line_len++] = chunk[i];
        }
        // else: an unreasonably long line - drop the overflow rather than
        // corrupt memory; a misbehaving script can't crash the bar.
    }
}

// Called once, right before restart_axe()'s execvp() - without this, every
// restart leaks a new copy of the status script (it survives the parent's
// self-exec as an orphan, forever). Negative pid = whole process group,
// so a backgrounded job inside the script (like `sleep infinity &`) gets
// caught too, not just the top-level shell.
void bar_kill_status(void) {
    if(status_pid > 0) {
        kill(-status_pid, SIGTERM);
        status_pid = -1;
    }
}


void bar_init(void) {
    if(FT_Init_FreeType(&ft_library) != 0) {
        fprintf(stderr, "bar: FT_Init_FreeType failed - bar will show without text\n");
        return;
    }

    FcInit();
    FcPattern *pat = FcNameParse((const FcChar8 *) bar_font_name);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);

    if(match == NULL) {
        fprintf(stderr, "bar: fontconfig couldn't resolve '%s'\n", bar_font_name);
        return;
    }

    FcChar8 *file = NULL;
    int index = 0;
    FcPatternGetString(match, FC_FILE, 0, &file);
    FcPatternGetInteger(match, FC_INDEX, 0, &index);

    if(file == NULL || FT_New_Face(ft_library, (const char *) file, index, &ft_face) != 0) {
        fprintf(stderr, "bar: failed to load font file for '%s'\n", bar_font_name);
        ft_face = NULL;
        FcPatternDestroy(match);
        return;
    }
    FcPatternDestroy(match);

    FT_Set_Pixel_Sizes(ft_face, 0, bar_font_size);

    int ascender = ft_face->size->metrics.ascender >> 6;
    int descender = ft_face->size->metrics.descender >> 6; // negative
    int line_h = ascender - descender;
    baseline_y = (bar_height - line_h) / 2 + ascender;

    spawn_status();
}

static int measure_text_width(const char *s) {
    if(ft_face == NULL) return 0;
    int width = 0;
    for(const unsigned char *p = (const unsigned char *) s; *p; p++) {
        if(FT_Load_Char(ft_face, *p, FT_LOAD_DEFAULT) != 0) continue;
        width += ft_face->glyph->advance.x >> 6;
    }
    return width;
}

static void blend_pixel(uint8_t *buf, int w, int h, int x, int y, const uint8_t color[4], int alpha) {
    if(x < 0 || x >= w || y < 0 || y >= h || alpha == 0) return;
    uint8_t *p = buf + (y * w + x) * 4; // B,G,R,A
    int inv = 255 - alpha;
    p[0] = (color[2] * alpha + p[0] * inv) / 255;
    p[1] = (color[1] * alpha + p[1] * inv) / 255;
    p[2] = (color[0] * alpha + p[2] * inv) / 255;
    p[3] = 255;
}

static void draw_text(uint8_t *buf, int w, int h, int x, const char *s, const uint8_t color[4]) {
    if(ft_face == NULL) return;
    int pen_x = x;
    for(const unsigned char *p = (const unsigned char *) s; *p; p++) {
        if(FT_Load_Char(ft_face, *p, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot g = ft_face->glyph;
        int gx = pen_x + g->bitmap_left;
        int gy = baseline_y - g->bitmap_top;
        for(unsigned int row = 0; row < g->bitmap.rows; row++) {
            for(unsigned int col = 0; col < g->bitmap.width; col++) {
                blend_pixel(buf, w, h, gx + (int) col, gy + (int) row, color, g->bitmap.buffer[row * g->bitmap.pitch + col]);
            }
        }
        pen_x += g->advance.x >> 6;
    }
}

static void fill_rect(uint8_t *buf, int w, int h, int x0, int y0, int x1, int y1, const uint8_t color[4]) {
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > w) x1 = w;
    if(y1 > h) y1 = h;
    for(int y = y0; y < y1; y++) {
        for(int x = x0; x < x1; x++) {
            uint8_t *p = buf + (y * w + x) * 4;
            p[0] = color[2]; p[1] = color[1]; p[2] = color[0]; p[3] = color[3];
        }
    }
}

static void redraw(Output *o) {
    if(compositor == NULL || shm == NULL || wlr_layer_shell == NULL) return;
    if(o->wl_output == NULL) return;

    if(o->bar_surface == NULL) {
        o->bar_surface = wl_compositor_create_surface(compositor);
        o->bar_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            wlr_layer_shell, o->bar_surface, o->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "axe-bar");

        extern const struct zwlr_layer_surface_v1_listener bar_layer_surface_listener;
        zwlr_layer_surface_v1_add_listener(o->bar_layer_surface, &bar_layer_surface_listener, o);

        zwlr_layer_surface_v1_set_anchor(o->bar_layer_surface,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(o->bar_layer_surface, 0, bar_height); // 0 width = "fill anchored edges"
        zwlr_layer_surface_v1_set_exclusive_zone(o->bar_layer_surface, bar_height);

        // No interactivity implemented (no tag-click, no keyboard) - pass
        // everything through to whatever's underneath.
        struct wl_region *empty = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(o->bar_surface, empty);
        wl_region_destroy(empty);

        wl_surface_commit(o->bar_surface); // triggers configure -> redraw() again
        return;
    }

    // if(o->bar_configured_w <= 0) return; // no configure yet
    if(o->bar_configured_w <= 0 || o->bar_configured_h <= 0) return; // no configure yet

    int w = o->bar_configured_w;
    int h = o->bar_configured_h;

    uint32_t occupied = 0;
    Window *win;
    wl_list_for_each(win, &axe.windows, link) {
        if(win->mon == o) occupied |= win->tagmask;
    }

    // Skip the redraw entirely if nothing that would change the pixels
    // has actually changed - bar_redraw_all() gets called on every
    // manage_start, including mid-drag frames.
    if(o->bar_buffer != NULL && o->bar_buf_w == w && o->bar_buf_h == h && o->bar_last_occupied == occupied && o->bar_last_seltag == o->seltag && o->bar_last_status_epoch == status_epoch) {
        return;
    }
    o->bar_last_occupied = occupied;
    o->bar_last_seltag = o->seltag;
    o->bar_last_status_epoch = status_epoch;

    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("axe-bar", MFD_CLOEXEC);
    if(fd == -1 || ftruncate(fd, size) < 0) {
        fprintf(stderr, "bar: failed to create shm fd\n");
        if(fd != -1) close(fd);
        return;
    }
    uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(buf == MAP_FAILED) {
        fprintf(stderr, "bar: mmap failed\n");
        close(fd);
        return;
    }

    fill_rect(buf, w, h, 0, 0, w, h, bar_bg_color);

    int cellw = h; // square-ish tag cells, scaled off bar height
    int x = 0;
    for(int t = 0; t < 9; t++) {
        bool sel = (o->seltag & (1u << t)) != 0;
        bool occ = (occupied & (1u << t)) != 0;
        if(!sel && !occ) continue;
        if(sel) fill_rect(buf, w, h, x, 0, x + cellw, h, bar_sel_bg_color);

        char label[2] = { (char) ('1' + t), '\0' };
        int tw = measure_text_width(label);
        draw_text(buf, w, h, x + (cellw - tw) / 2, label, bar_tag_fg_color);

        x += cellw;
    }

    if(cmd_output[0] != '\0') {
        int tw = measure_text_width(cmd_output);
        draw_text(buf, w, h, w - tw - 8, cmd_output, bar_tag_fg_color);
    }

    munmap(buf, size);

    if(o->bar_buffer != NULL) wl_buffer_destroy(o->bar_buffer);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    o->bar_buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    o->bar_buf_w = w;
    o->bar_buf_h = h;

    wl_surface_attach(o->bar_surface, o->bar_buffer, 0, 0);
    wl_surface_damage_buffer(o->bar_surface, 0, 0, w, h);
    wl_surface_commit(o->bar_surface);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *obj, uint32_t serial, uint32_t width, uint32_t height) {
    Output *o = data;
    zwlr_layer_surface_v1_ack_configure(obj, serial);
    o->bar_configured_w = width;
    o->bar_configured_h = height;
    redraw(o);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *obj) {
    Output *o = data;
    zwlr_layer_surface_v1_destroy(o->bar_layer_surface);
    wl_surface_destroy(o->bar_surface);
    if(o->bar_buffer != NULL) wl_buffer_destroy(o->bar_buffer);
    o->bar_layer_surface = NULL;
    o->bar_surface = NULL;
    o->bar_buffer = NULL;
    o->bar_configured_w = 0;
    o->bar_configured_h = 0;
}

const struct zwlr_layer_surface_v1_listener bar_layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

void bar_output_ready(Output *o) { redraw(o); }

void bar_manager_ready(void) {
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) redraw(o);
}

void bar_redraw_all(void) {
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) redraw(o);
}

void bar_destroy(Output *o) {
    if(o->bar_layer_surface != NULL) zwlr_layer_surface_v1_destroy(o->bar_layer_surface);
    if(o->bar_surface != NULL) wl_surface_destroy(o->bar_surface);
    if(o->bar_buffer != NULL) wl_buffer_destroy(o->bar_buffer);

    o->bar_layer_surface = NULL;
    o->bar_surface = NULL;
    o->bar_buffer = NULL;
    o->bar_configured_w = 0;
    o->bar_configured_h = 0;
}
