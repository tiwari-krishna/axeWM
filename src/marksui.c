#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "axe.h"
#include "config.h" // marksui_* settings

// Harpoon-style graphical picker over axe.marks[] (see markwindow/
// gotomark in actions.c) - a small overlay centered on selmon, toggled
// open/closed with togglemarksui.
//
// Keyboard interaction is handled the exact same way every other axe
// keybind is: as river_xkb_bindings_v1 global bindings on each seat,
// left disabled until the picker opens and re-disabled when it closes
// (marksui_setup_seat, called once per seat at startup - same pattern
// as bar_setup_seat_autohide). This is deliberately NOT real Wayland
// keyboard focus on the picker's own surface - that would mean
// requesting keyboard_interactivity on the layer surface and
// implementing a wl_keyboard/xkb-state client path entirely separate
// from river's own binding mechanism, a much bigger and more fragile
// lift for what's fundamentally the same kind of "global shortcut"
// axe already is everywhere else. The tradeoff: j/k/J/K/Return/Escape/d/q
// are captured on every seat uniformly while open, not scoped to
// "whichever seat pressed the toggle key" - matches how passthrough and
// the bar/tabbar autohide gesture already treat the realistic
// single-seat case elsewhere in this codebase.

static bool marksui_open = false;
static int marksui_selected = 0;
static Output *marksui_output; // which output it's currently shown on

static struct wl_surface *marksui_surface;
static struct zwlr_layer_surface_v1 *marksui_layer_surface;
static struct wl_buffer *marksui_buffer;
static int marksui_configured_w, marksui_configured_h;

static void marksui_set_nav_enabled(bool enabled) {
    Seat *s;
    wl_list_for_each(s, &axe.seats, link) {
        Key *keys[] = {
            s->marksui_down, s->marksui_up,
            s->marksui_move_down, s->marksui_move_up,
            s->marksui_confirm, s->marksui_cancel, s->marksui_quit, s->marksui_clear,
        };
        for(size_t i = 0; i < LENGTH(keys); i++) {
            if(enabled) river_xkb_binding_v1_enable(keys[i]->river_xkb_binding);
            else river_xkb_binding_v1_disable(keys[i]->river_xkb_binding);
        }
    }
}

static void marksui_paint(void) {
    if(marksui_surface == NULL) return; // picker isn't open
    if(marksui_configured_w <= 0 || marksui_configured_h <= 0) return; // no configure yet

    int w = marksui_configured_w;
    int h = marksui_configured_h;
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("axe-marksui", MFD_CLOEXEC);
    if(fd == -1 || ftruncate(fd, size) < 0) {
        fprintf(stderr, "marksui: failed to create shm fd\n");
        if(fd != -1) close(fd);
        return;
    }
    uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(buf == MAP_FAILED) {
        fprintf(stderr, "marksui: mmap failed\n");
        close(fd);
        return;
    }

    fill_rect(buf, w, h, 0, 0, w, h, marksui_bg_color);

    for(int i = 0; i < MARK_COUNT; i++) {
        int y0 = i * marksui_row_height;
        int y1 = y0 + marksui_row_height;
        bool sel = (i == marksui_selected);
        if(sel) fill_rect(buf, w, h, 0, y0, w, y1, marksui_sel_bg_color);

        Window *win = axe.marks[i];
        char line[300];
        if(win == NULL) {
            snprintf(line, sizeof(line), "%d. (empty)", i + 1);
        } else {
            const char *title = win->title[0] ? win->title : (win->app_id[0] ? win->app_id : "?");
            snprintf(line, sizeof(line), "%d. %s", i + 1, title);
        }

        const uint8_t *fg = sel ? marksui_sel_fg_color : marksui_fg_color;
        draw_text(buf, w, h, 10, y0, marksui_row_height, line, fg);
    }

    munmap(buf, size);

    if(marksui_buffer != NULL) wl_buffer_destroy(marksui_buffer);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    marksui_buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    wl_surface_attach(marksui_surface, marksui_buffer, 0, 0);
    wl_surface_damage_buffer(marksui_surface, 0, 0, w, h);
    wl_surface_commit(marksui_surface);
}

static void marksui_layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *obj, uint32_t serial, uint32_t width, uint32_t height) {
    zwlr_layer_surface_v1_ack_configure(obj, serial);
    marksui_configured_w = width;
    marksui_configured_h = height;
    marksui_paint();
}

static void marksui_layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *obj) {
    // Compositor-initiated close (e.g. output gone away mid-use) - tear
    // down the same way marksui_close() does, just without needing to
    // destroy objects the compositor already considers gone.
    marksui_open = false;
    marksui_set_nav_enabled(false);
    marksui_layer_surface = NULL;
    marksui_surface = NULL;
    marksui_buffer = NULL;
    marksui_configured_w = marksui_configured_h = 0;
}

static const struct zwlr_layer_surface_v1_listener marksui_layer_surface_listener = {
    .configure = marksui_layer_surface_configure,
    .closed = marksui_layer_surface_closed,
};

static void marksui_close(Seat *seat, Arg *arg) {
    if(!marksui_open) return;
    marksui_open = false;
    marksui_set_nav_enabled(false);

    if(marksui_layer_surface != NULL) {
        zwlr_layer_surface_v1_destroy(marksui_layer_surface);
        wl_surface_destroy(marksui_surface);
        if(marksui_buffer != NULL) wl_buffer_destroy(marksui_buffer);
        marksui_layer_surface = NULL;
        marksui_surface = NULL;
        marksui_buffer = NULL;
        marksui_configured_w = marksui_configured_h = 0;
    }
}

static void marksui_move_down(Seat *seat, Arg *arg) {
    if(marksui_selected < MARK_COUNT - 1) marksui_selected++;
    marksui_paint();
}

static void marksui_move_up(Seat *seat, Arg *arg) {
    if(marksui_selected > 0) marksui_selected--;
    marksui_paint();
}

static void marksui_reorder(int delta) {
    int other = marksui_selected + delta;
    if(other < 0 || other >= MARK_COUNT) return;

    Window *tmp = axe.marks[marksui_selected];
    axe.marks[marksui_selected] = axe.marks[other];
    axe.marks[other] = tmp;

    marksui_selected = other; // selection follows the moved entry
    marksui_paint();
}

static void marksui_reorder_down(Seat *seat, Arg *arg) { marksui_reorder(+1); }
static void marksui_reorder_up(Seat *seat, Arg *arg)   { marksui_reorder(-1); }

static void marksui_clear_selected(Seat *seat, Arg *arg) {
    axe.marks[marksui_selected] = NULL;
    marksui_paint();
}

static void marksui_confirm(Seat *seat, Arg *arg) {
    Arg jump = { .i = marksui_selected };
    gotomark(seat, &jump);
    marksui_close(seat, arg);
}

void marksui_setup_seat(Seat *seat) {
    seat->marksui_down      = xkb_binding_create(seat, 0,     XKB_KEY_j, marksui_move_down,     NULL);
    seat->marksui_up        = xkb_binding_create(seat, 0,     XKB_KEY_k, marksui_move_up,       NULL);
    seat->marksui_move_down = xkb_binding_create(seat, SHIFT, XKB_KEY_j, marksui_reorder_down,  NULL);
    seat->marksui_move_up   = xkb_binding_create(seat, SHIFT, XKB_KEY_k, marksui_reorder_up,    NULL);
    seat->marksui_confirm   = xkb_binding_create(seat, 0, XKB_KEY_Return, marksui_confirm,      NULL);
    seat->marksui_cancel    = xkb_binding_create(seat, 0, XKB_KEY_Escape, marksui_close,        NULL);
    seat->marksui_quit      = xkb_binding_create(seat, 0,     XKB_KEY_q, marksui_close,          NULL);
    seat->marksui_clear     = xkb_binding_create(seat, 0,     XKB_KEY_d, marksui_clear_selected, NULL);

    // Our own open/close logic (marksui_set_nav_enabled) is the sole
    // owner of whether these are enabled - togglepassthrough (actions.c)
    // must never force these on/off when it sweeps every other key on
    // entering/leaving passthrough mode. Without this, leaving
    // passthrough re-enables all seven globally regardless of whether
    // the picker is actually open.
    Key *nav_keys[] = {
        seat->marksui_down, seat->marksui_up,
        seat->marksui_move_down, seat->marksui_move_up,
        seat->marksui_confirm, seat->marksui_cancel, seat->marksui_quit, seat->marksui_clear,
    };
    for(size_t i = 0; i < LENGTH(nav_keys); i++) {
        nav_keys[i]->managed_externally = true;
    }

    river_xkb_binding_v1_disable(seat->marksui_down->river_xkb_binding);
    river_xkb_binding_v1_disable(seat->marksui_up->river_xkb_binding);
    river_xkb_binding_v1_disable(seat->marksui_move_down->river_xkb_binding);
    river_xkb_binding_v1_disable(seat->marksui_move_up->river_xkb_binding);
    river_xkb_binding_v1_disable(seat->marksui_confirm->river_xkb_binding);
    river_xkb_binding_v1_disable(seat->marksui_cancel->river_xkb_binding);
    river_xkb_binding_v1_disable(seat->marksui_quit->river_xkb_binding);
    river_xkb_binding_v1_disable(seat->marksui_clear->river_xkb_binding);
}

void marksui_toggle(void) {
    if(marksui_open) {
        marksui_close(NULL, NULL); // body never touches seat/arg
        return;
    }

    if(compositor == NULL || shm == NULL || wlr_layer_shell == NULL) return;
    if(selmon == NULL || selmon->wl_output == NULL) return;

    marksui_open = true;
    marksui_selected = 0;
    marksui_output = selmon;

    marksui_surface = wl_compositor_create_surface(compositor);
    marksui_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wlr_layer_shell, marksui_surface, marksui_output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "axe-marksui");
    zwlr_layer_surface_v1_add_listener(marksui_layer_surface, &marksui_layer_surface_listener, NULL);

    // No anchor set at all = centered on the output, per wlr-layer-shell.
    // Doesn't reserve any space (exclusive zone 0, same "no reservation"
    // convention as the bar/tab strip use in overlay mode) - it's a
    // transient popup, not a permanent dock.
    zwlr_layer_surface_v1_set_size(marksui_layer_surface, marksui_width, MARK_COUNT * marksui_row_height);
    zwlr_layer_surface_v1_set_exclusive_zone(marksui_layer_surface, 0);

    struct wl_region *empty = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(marksui_surface, empty);
    wl_region_destroy(empty);

    marksui_set_nav_enabled(true);

    wl_surface_commit(marksui_surface); // triggers configure -> marksui_paint()
}

void marksui_output_removed(Output *output) {
    if(marksui_output == output) marksui_close(NULL, NULL);
}

// Called from window.c right after a closing window's mark slots get
// cleared, so the picker (if open) doesn't keep showing a title for a
// window that no longer exists until the next keypress.
void marksui_notify_mark_changed(void) {
    if(marksui_open) marksui_paint();
}
