#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "axe.h"
#include "config.h" // tab_* settings

// i3-style tab strip for LAYOUT_TABBED outputs. Deliberately loads no
// fonts of its own - fill_rect/measure_text_width/draw_text (exported
// from bar.c) draw with whatever fonts bar_init() already loaded, so
// tab labels render identically to the status bar with zero duplicate
// FreeType setup here.

// Mirrors bar.c's bar_visible: starts hidden if tab_autohide is on, and
// is flipped by the shared Super-hold gesture (see bar_setup_seat_
// autohide in bar.c) via tabbar_set_visible(). Unlike bar.c, showing/
// hiding doesn't need its own imperative exclusive-zone/blank-buffer
// dance here - redraw()'s existing `want` transition logic (already
// needed for the layout and window-count edges) handles it for free,
// since tab_visible is just one more input to `want` below.
static bool tab_visible = true;

void tabbar_init(void) {
    tab_visible = !tab_autohide;
}

void tabbar_set_visible(bool visible) {
    if(tab_visible == visible) return;
    tab_visible = visible;
    tabbar_redraw_all();
}


static void draw_blank(Output *o) {
    int w = o->tab_configured_w, h = o->tab_configured_h;
    if(w <= 0 || h <= 0) return;

    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("axe-tabbar-blank", MFD_CLOEXEC);
    if(fd == -1 || ftruncate(fd, size) < 0) {
        fprintf(stderr, "tabbar: failed to create shm fd (blank)\n");
        if(fd != -1) close(fd);
        return;
    }
    uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(buf == MAP_FAILED) {
        fprintf(stderr, "tabbar: mmap failed (blank)\n");
        close(fd);
        return;
    }
    memset(buf, 0, size); // all-zero = fully transparent
    munmap(buf, size);

    if(o->tab_buffer != NULL) wl_buffer_destroy(o->tab_buffer);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    o->tab_buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    o->tab_buf_w = -1; // force a real redraw next time it's shown
    o->tab_buf_h = h;

    wl_surface_attach(o->tab_surface, o->tab_buffer, 0, 0);
    wl_surface_damage_buffer(o->tab_surface, 0, 0, w, h);
    wl_surface_commit(o->tab_surface);
}

static void redraw(Output *o) {
    if(compositor == NULL || shm == NULL || wlr_layer_shell == NULL) return;
    if(o->wl_output == NULL) return;

    // Only actually want the strip up when this tag is in tabbed layout,
    // there's more than one tiled window to switch between, AND (if
    // tab_autohide is on) the reveal gesture currently has it shown.
    bool want = (o->layout[tagidx(o)] == LAYOUT_TABBED) && count_tiled_windows(o) > 1 && tab_visible;

    if(o->tab_surface == NULL) {
        if(!want) return; // never needed yet - nothing to create

        o->tab_surface = wl_compositor_create_surface(compositor);
        o->tab_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            wlr_layer_shell, o->tab_surface, o->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "axe-tabbar");

        extern const struct zwlr_layer_surface_v1_listener tab_layer_surface_listener;
        zwlr_layer_surface_v1_add_listener(o->tab_layer_surface, &tab_layer_surface_listener, o);

        // Anchor edge is independently configurable from the bar's via
        // tab_at_bottom - doesn't need to match bar_at_bottom.
        uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        anchor |= tab_at_bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        zwlr_layer_surface_v1_set_anchor(o->tab_layer_surface, anchor);
        zwlr_layer_surface_v1_set_size(o->tab_layer_surface, 0, tab_height);
        zwlr_layer_surface_v1_set_exclusive_zone(o->tab_layer_surface, tab_height);
        o->tab_last_layout = true;

        struct wl_region *empty = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(o->tab_surface, empty);
        wl_region_destroy(empty);

        wl_surface_commit(o->tab_surface); // triggers configure -> redraw() again
        return;
    }

    // Surface already exists - toggle its reserved space (not the
    // surface itself) to match the current layout. Same "stay mapped,
    // only swap buffer content" approach as bar.c's draw_blank(), for
    // the same protocol-safety reason documented there.
    if(!want) {
        if(o->tab_last_layout) {
            zwlr_layer_surface_v1_set_exclusive_zone(o->tab_layer_surface, 0);
            draw_blank(o);
        }
        o->tab_last_layout = false;
        return;
    }

    if(!o->tab_last_layout) {
        zwlr_layer_surface_v1_set_exclusive_zone(o->tab_layer_surface, tab_height);
        o->tab_buf_w = -1; // force a full redraw once space is reclaimed
    }
    o->tab_last_layout = true;

    if(o->tab_configured_w <= 0 || o->tab_configured_h <= 0) return; // no configure yet

    int w = o->tab_configured_w;
    int h = o->tab_configured_h;

    // Cheap signature of everything that would change the pixels: each
    // tab's identity, title, and whether it's the highlighted one. Skip
    // the redraw if none of that changed - tabbar_redraw_all() runs on
    // every manage_start, including cycles triggered by an unrelated
    // output or a floating-window drag elsewhere.
    uint32_t hash = 2166136261u;
    int count = 0;
    Window *win;
    wl_list_for_each(win, &axe.windows, link) {
        if(win->mon != o || win->floating || win->sticky || !ISVISIBLE(win)) continue;
        count++;
        hash ^= (uint32_t)(uintptr_t) win; hash *= 16777619u;
        hash ^= win->focused ? 1u : 0u;     hash *= 16777619u;
        const char *title = win->title[0] ? win->title : win->app_id;
        for(const char *p = title; *p; p++) { hash ^= (uint8_t) *p; hash *= 16777619u; }
    }

    if(o->tab_buffer != NULL && o->tab_buf_w == w && o->tab_buf_h == h && o->tab_last_hash == hash) {
        return;
    }
    o->tab_last_hash = hash;

    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("axe-tabbar", MFD_CLOEXEC);
    if(fd == -1 || ftruncate(fd, size) < 0) {
        fprintf(stderr, "tabbar: failed to create shm fd\n");
        if(fd != -1) close(fd);
        return;
    }
    uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(buf == MAP_FAILED) {
        fprintf(stderr, "tabbar: mmap failed\n");
        close(fd);
        return;
    }

    fill_rect(buf, w, h, 0, 0, w, h, tab_bg_color);

    if(count > 0) {
        int cellw = w / count;
        int x = 0, idx = 0;
        wl_list_for_each(win, &axe.windows, link) {
            if(win->mon != o || win->floating || win->sticky || !ISVISIBLE(win)) continue;

            int cw = (idx == count - 1) ? (w - x) : cellw; // last tab absorbs the remainder
            const uint8_t *bg = win->focused ? tab_sel_bg_color : tab_bg_color;
            const uint8_t *fg = win->focused ? tab_sel_fg_color : tab_fg_color;
            fill_rect(buf, w, h, x, 0, x + cw, h, bg);

            const char *title = win->title[0] ? win->title : (win->app_id[0] ? win->app_id : "?");
            draw_text(buf, w, h, x + 6, title, fg);

            x += cw;
            idx++;
        }
    }

    munmap(buf, size);

    if(o->tab_buffer != NULL) wl_buffer_destroy(o->tab_buffer);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    o->tab_buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    o->tab_buf_w = w;
    o->tab_buf_h = h;

    wl_surface_attach(o->tab_surface, o->tab_buffer, 0, 0);
    wl_surface_damage_buffer(o->tab_surface, 0, 0, w, h);
    wl_surface_commit(o->tab_surface);
}

static void tab_layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *obj, uint32_t serial, uint32_t width, uint32_t height) {
    Output *o = data;
    zwlr_layer_surface_v1_ack_configure(obj, serial);
    o->tab_configured_w = width;
    o->tab_configured_h = height;
    redraw(o);
}

static void tab_layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *obj) {
    Output *o = data;
    zwlr_layer_surface_v1_destroy(o->tab_layer_surface);
    wl_surface_destroy(o->tab_surface);
    if(o->tab_buffer != NULL) wl_buffer_destroy(o->tab_buffer);
    o->tab_layer_surface = NULL;
    o->tab_surface = NULL;
    o->tab_buffer = NULL;
    o->tab_configured_w = 0;
    o->tab_configured_h = 0;
}

const struct zwlr_layer_surface_v1_listener tab_layer_surface_listener = {
    .configure = tab_layer_surface_configure,
    .closed = tab_layer_surface_closed,
};

void tabbar_output_ready(Output *o) { redraw(o); }

void tabbar_manager_ready(void) {
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) redraw(o);
}

void tabbar_redraw_all(void) {
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) redraw(o);
}

void tabbar_destroy(Output *o) {
    if(o->tab_layer_surface != NULL) zwlr_layer_surface_v1_destroy(o->tab_layer_surface);
    if(o->tab_surface != NULL) wl_surface_destroy(o->tab_surface);
    if(o->tab_buffer != NULL) wl_buffer_destroy(o->tab_buffer);

    o->tab_layer_surface = NULL;
    o->tab_surface = NULL;
    o->tab_buffer = NULL;
    o->tab_configured_w = 0;
    o->tab_configured_h = 0;
}
