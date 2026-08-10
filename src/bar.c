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

#include <limits.h>

static FT_Library ft_library;
static FT_Face ft_face; // NULL if font load failed - drawing text becomes a no-op, bar still shows colored blocks
static FT_Face ft_face_emoji; // NULL if unavailable/disabled - codepoints ft_face lacks fall back here

// Second, larger instance of the same two faces, loaded once alongside
// the normal ones - purely for marksui.c's picker (see marksui_font_scale
// in config.h). Kept as fully separate FT_Face objects rather than
// dynamically resizing ft_face/ft_face_emoji per-draw and restoring
// afterward: simpler, avoids any "forgot to restore" hazard, and the
// bitmap-strike selection load_face() does for emoji fonts doesn't have
// a clean temporary-resize story anyway (it's picking among fixed
// strikes, not continuously scaling).
static FT_Face ft_face_lg, ft_face_emoji_lg;

static char cmd_output[256] = "";

static bool bar_visible = true;
static Arg bar_hold_arg = {0}; // shared, inert - persists for program lifetime, unlike a stack Arg
static int status_fd = -1;
static pid_t status_pid = -1;
static char status_line_buf[512];
static size_t status_line_len = 0;
static uint32_t status_epoch = 0; // bumped on every status text change - see redraw()'s dedup check

// Font metrics only - height-independent. draw_text() is shared with
// tabbar.c, whose strip height (tab_height) can differ from the status
// bar's (bar_height), so the actual baseline has to be derived per-call
// from the target buffer's own height, not baked in once at startup.
static int line_ascender, line_descender;
static int line_ascender_lg, line_descender_lg; // metrics for ft_face_lg

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
            status_pid = -1;
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

static void load_face(const char *font_name, int pixel_size, FT_Face *out_face) {
    *out_face = NULL;
    if(font_name == NULL) return;

    FcPattern *pat = FcNameParse((const FcChar8 *) font_name);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);

    if(match == NULL) {
        fprintf(stderr, "bar: fontconfig couldn't resolve '%s'\n", font_name);
        return;
    }

    FcChar8 *file = NULL;
    int index = 0;
    FcPatternGetString(match, FC_FILE, 0, &file);
    FcPatternGetInteger(match, FC_INDEX, 0, &index);

    if(file == NULL || FT_New_Face(ft_library, (const char *) file, index, out_face) != 0) {
        fprintf(stderr, "bar: failed to load font file for '%s'\n", font_name);
        *out_face = NULL;
        FcPatternDestroy(match);
        return;
    }
    FcPatternDestroy(match);

    if((*out_face)->num_fixed_sizes > 0) {
        // Bitmap-strike font (Noto Color Emoji etc) - it has no scalable
        // outline, so pick whichever fixed strike is closest to the
        // requested size rather than FT_Set_Pixel_Sizes, which doesn't
        // reliably select a sane strike on bitmap-only fonts.
        int best = 0, best_diff = INT_MAX;
        for(int i = 0; i < (*out_face)->num_fixed_sizes; i++) {
            int diff = abs((*out_face)->available_sizes[i].height - pixel_size);
            if(diff < best_diff) { best_diff = diff; best = i; }
        }
        FT_Select_Size(*out_face, best);
    } else {
        FT_Set_Pixel_Sizes(*out_face, 0, pixel_size);
    }
}

void bar_init(void) {
    bar_visible = !bar_autohide;
    if(FT_Init_FreeType(&ft_library) != 0) {
        fprintf(stderr, "bar: FT_Init_FreeType failed - bar will show without text\n");
        return;
    }

    FcInit();
    load_face(bar_font_name, bar_font_size, &ft_face);
    load_face(bar_emoji_font_name, bar_font_size, &ft_face_emoji);
    load_face(bar_font_name, bar_font_size * marksui_font_scale, &ft_face_lg);
    load_face(bar_emoji_font_name, bar_font_size * marksui_font_scale, &ft_face_emoji_lg);

    if(ft_face == NULL) {
        fprintf(stderr, "bar: primary font unavailable - bar will show without text\n");
        return;
    }

    line_ascender = ft_face->size->metrics.ascender >> 6;
    line_descender = ft_face->size->metrics.descender >> 6; // negative

    if(ft_face_lg != NULL) {
        line_ascender_lg = ft_face_lg->size->metrics.ascender >> 6;
        line_descender_lg = ft_face_lg->size->metrics.descender >> 6;
    } else {
        fprintf(stderr, "marksui: larger font unavailable - picker will show without text\n");
    }

    spawn_status();
}

// Decodes one UTF-8 codepoint at *p, advances *p past it. Returns 0 at
// end of string; returns U+FFFD and advances by 1 byte on malformed
// input, so a corrupt byte can't desync the whole rest of the string.
static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *) *p;
    if(*s == 0) return 0;

    uint32_t cp; int len;
    if((*s & 0x80) == 0)         { cp = *s;        len = 1; }
    else if((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; len = 2; }
    else if((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; len = 3; }
    else if((*s & 0xF8) == 0xF0) { cp = *s & 0x07; len = 4; }
    else { *p += 1; return 0xFFFD; }

    for(int i = 1; i < len; i++) {
        if((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += len;
    return cp;
}

static FT_Face face_for_codepoint(uint32_t cp, bool large) {
    FT_Face primary = large ? ft_face_lg : ft_face;
    FT_Face emoji = large ? ft_face_emoji_lg : ft_face_emoji;
    if(primary != NULL && FT_Get_Char_Index(primary, cp) != 0) return primary;
    if(emoji != NULL && FT_Get_Char_Index(emoji, cp) != 0) return emoji;
    return NULL;
}

int measure_text_width(const char *s) {
    int width = 0;
    uint32_t cp;
    while((cp = utf8_next(&s)) != 0) {
        FT_Face face = face_for_codepoint(cp, false);
        if(face == NULL) continue;

        bool is_emoji = (face == ft_face_emoji);
        if(FT_Load_Char(face, cp, is_emoji ? (FT_LOAD_RENDER | FT_LOAD_COLOR) : FT_LOAD_RENDER) != 0) continue;

        if(is_emoji && face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
            // Mirror draw_text's scale-to-line-height math exactly, or
            // measured width won't match what actually gets drawn.
            if(face->glyph->bitmap.rows == 0) continue;
            int target_h = bar_height - 2;
            float scale = (float) target_h / (float) face->glyph->bitmap.rows;
            width += (int) (face->glyph->bitmap.width * scale);
        } else {
            width += face->glyph->advance.x >> 6;
        }
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

// Composites a premultiplied-BGRA source pixel (FreeType's convention for
// FT_PIXEL_MODE_BGRA glyphs - each byte already scaled by that pixel's own
// alpha) onto an opaque destination.
static void blend_pixel_bgra(uint8_t *buf, int w, int h, int x, int y, const uint8_t *src) {
    if(x < 0 || x >= w || y < 0 || y >= h) return;
    int alpha = src[3];
    if(alpha == 0) return;
    uint8_t *p = buf + (y * w + x) * 4;
    int inv = 255 - alpha;
    p[0] = src[0] + (p[0] * inv) / 255;
    p[1] = src[1] + (p[1] * inv) / 255;
    p[2] = src[2] + (p[2] * inv) / 255;
    p[3] = 255;
}


void draw_text(uint8_t *buf, int w, int h, int x, int y0, int row_h, const char *s, const uint8_t color[4], bool large) {
    int pen_x = x;
    int asc = large ? line_ascender_lg : line_ascender;
    int desc = large ? line_descender_lg : line_descender;
    int line_h = asc - desc;
    int baseline_y = y0 + (row_h - line_h) / 2 + asc;
    uint32_t cp;
    while((cp = utf8_next(&s)) != 0) {
        FT_Face face = face_for_codepoint(cp,large);
        if(face == NULL) continue;

        FT_Face emoji_face = large ? ft_face_emoji_lg : ft_face_emoji;
        bool is_emoji = (face == emoji_face);
        if(FT_Load_Char(face, cp, is_emoji ? (FT_LOAD_RENDER | FT_LOAD_COLOR) : FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot g = face->glyph;

        if(is_emoji && g->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
            if(g->bitmap.rows == 0) continue;
            int target_h = row_h - 2;
            float scale = (float) target_h / (float) g->bitmap.rows;
            int dst_w = (int) (g->bitmap.width * scale);
            int gy_top = y0 + (row_h - target_h) / 2;

            for(int dy = 0; dy < target_h; dy++) {
                int sy = (int) (dy / scale);
                if(sy >= (int) g->bitmap.rows) sy = g->bitmap.rows - 1;
                for(int dx = 0; dx < dst_w; dx++) {
                    int sx = (int) (dx / scale);
                    if(sx >= (int) g->bitmap.width) sx = g->bitmap.width - 1;
                    blend_pixel_bgra(buf, w, h, pen_x + dx, gy_top + dy, g->bitmap.buffer + sy * g->bitmap.pitch + sx * 4);
                }
            }
            pen_x += dst_w;
        } else {
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
}

void fill_rect(uint8_t *buf, int w, int h, int x0, int y0, int x1, int y1, const uint8_t color[4]) {
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
        uint32_t bar_layer = bar_autohide ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY : ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        o->bar_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            wlr_layer_shell, o->bar_surface, o->wl_output, bar_layer, "axe-bar");

        extern const struct zwlr_layer_surface_v1_listener bar_layer_surface_listener;
        zwlr_layer_surface_v1_add_listener(o->bar_layer_surface, &bar_layer_surface_listener, o);

        uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        anchor |= bar_at_bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        zwlr_layer_surface_v1_set_anchor(o->bar_layer_surface, anchor);
        zwlr_layer_surface_v1_set_size(o->bar_layer_surface, 0, bar_height); // 0 width = "fill anchored edges"
        zwlr_layer_surface_v1_set_exclusive_zone(o->bar_layer_surface, bar_autohide ? 0 : (bar_visible ? bar_height : 0));

        // No interactivity implemented (no tag-click, no keyboard) - pass
        // everything through to whatever's underneath.
        struct wl_region *empty = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(o->bar_surface, empty);
        wl_region_destroy(empty);

        wl_surface_commit(o->bar_surface); // triggers configure -> redraw() again
        return;
    }

    if(o->bar_configured_w <= 0 || o->bar_configured_h <= 0) return; // no configure yet
    if(!bar_visible) return;

    int w = o->bar_configured_w;
    int h = o->bar_configured_h;

    uint32_t occupied = 0;
    Window *win;
    wl_list_for_each(win, &axe.windows, link) {
        if(win->mon != o) continue;
        occupied |= win->tagmask;
    }

    // Skip the redraw entirely if nothing that would change the pixels
    // has actually changed - bar_redraw_all() gets called on every
    // manage_start, including mid-drag frames.
    if(o->bar_buffer != NULL && o->bar_buf_w == w && o->bar_buf_h == h && o->bar_last_occupied == occupied && o->bar_last_seltag == o->seltag && o->bar_last_status_epoch == status_epoch && o->bar_last_passthrough == passthrough) {
        return;
    }
    o->bar_last_occupied = occupied;
    o->bar_last_seltag = o->seltag;
    o->bar_last_status_epoch = status_epoch;
    o->bar_last_passthrough = passthrough;

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

    fill_rect(buf, w, h, 0, 0, w, h, passthrough ? bar_passthrough_bg_color : bar_bg_color);

    const uint8_t *fg = passthrough ? bar_passthrough_fg_color : bar_tag_fg_color;

    int cellw = h; // square-ish tag cells, scaled off bar height
    int x = 0;
    for(int t = 0; t < 9; t++) {
        bool sel = (o->seltag & (1u << t)) != 0;
        bool occ = (occupied & (1u << t)) != 0;
        if(!sel && !occ) continue;
        if(sel) fill_rect(buf, w, h, x, 0, x + cellw, h, bar_sel_bg_color);

        char label[2] = { (char) ('1' + t), '\0' };
        int tw = measure_text_width(label);
        draw_text(buf, w, h, x + (cellw - tw) / 2, 0, h, label, fg, false);

        x += cellw;
    }

    if(passthrough) {
        x += 6;
        draw_text(buf, w, h, x, 0, h, "PASSTHROUGH", fg, false);
    }

    if(cmd_output[0] != '\0') {
        int tw = measure_text_width(cmd_output);
        draw_text(buf, w, h, w - tw - 8, 0, h, cmd_output, fg, false);
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

// A fully transparent buffer of the surface's already-acked size,
// committed the ordinary way (no null-attach, no unmap) - this is
// "hidden" now, deliberately, instead of detaching the buffer. Once
// a layer surface has been through its initial configure/ack
// handshake, attaching NULL almost certainly puts it back into that
// same unconfigured state on River, requiring a *fresh* configure
// before a real buffer can be attached again - and nothing obligates
// the compositor to send one just because we voluntarily unmapped.
// Attaching a buffer without waiting for that is a protocol
// violation, which is exactly what killed the connection last time.
// Staying mapped the entire time and only ever swapping buffer
// *content* sidesteps that whole class of problem.
static void draw_blank(Output *o) {
    int w = o->bar_configured_w, h = o->bar_configured_h;
    if(w <= 0 || h <= 0) return;

    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("axe-bar-blank", MFD_CLOEXEC);
    if(fd == -1 || ftruncate(fd, size) < 0) {
        fprintf(stderr, "bar: failed to create shm fd (blank)\n");
        if(fd != -1) close(fd);
        return;
    }
    uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(buf == MAP_FAILED) {
        fprintf(stderr, "bar: mmap failed (blank)\n");
        close(fd);
        return;
    }
    memset(buf, 0, size); // all-zero = fully transparent (premultiplied alpha 0)
    munmap(buf, size);

    if(o->bar_buffer != NULL) wl_buffer_destroy(o->bar_buffer);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    o->bar_buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    o->bar_buf_w = -1; // force a real redraw the next time we're shown, regardless of cached state
    o->bar_buf_h = h;

    wl_surface_attach(o->bar_surface, o->bar_buffer, 0, 0);
    wl_surface_damage_buffer(o->bar_surface, 0, 0, w, h);
    wl_surface_commit(o->bar_surface);
}

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

void bar_set_visible(bool visible) {
    if(bar_visible == visible) return;
    bar_visible = visible;

    Output *o;
    wl_list_for_each(o, &axe.outputs, link) {
        if(o->bar_layer_surface == NULL) continue;
        if(o->bar_configured_w <= 0 || o->bar_configured_h <= 0) continue; // no configure yet - nothing to (re)draw

        if(!visible) {
            if(!bar_autohide) zwlr_layer_surface_v1_set_exclusive_zone(o->bar_layer_surface, 0);
            draw_blank(o);
        } else {
            if(!bar_autohide) zwlr_layer_surface_v1_set_exclusive_zone(o->bar_layer_surface, bar_height);
            o->bar_buf_w = -1;
            redraw(o);
        }
    }
}

void bar_toggle(void) {
    bar_set_visible(!bar_visible);
}

// FIX: these now drive tabbar visibility too, not just the bar - see the
// comment on bar_setup_seat_autohide below for why this is one shared
// hold gesture rather than a second independent binding.
static void bar_show_on_press(Seat *seat, Arg *arg) {
    if(bar_autohide) bar_set_visible(true);
    if(tab_autohide) tabbar_set_visible(true);
}

static void bar_hide_on_release(Seat *seat, Arg *arg) {
    if(bar_autohide) bar_set_visible(false);
    if(tab_autohide) tabbar_set_visible(false);
}

void bar_setup_seat_autohide(Seat *seat) {
    if(!bar_autohide && !tab_autohide) return;
    xkb_hold_binding_create(seat, 0, XKB_KEY_Super_L, bar_show_on_press, bar_hide_on_release, &bar_hold_arg);
    xkb_hold_binding_create(seat, 0, XKB_KEY_Super_R, bar_show_on_press, bar_hide_on_release, &bar_hold_arg);
}
