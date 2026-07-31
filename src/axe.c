#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include <sys/mman.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "axe.h"

#define MIN(A, B) (A < B ? A : B)
#define MAX(A, B) (A > B ? A : B)
#define LENGTH(A) (sizeof A / sizeof A[0])
#define ISVISIBLE(C) (C->tagmask & C->mon->tagmask)
#define CLAMP(VAL, MIN, MAX) VAL = VAL < MIN ? MIN : (VAL > MAX ? MAX : VAL)

WindowManager axe;
Output *selmon = NULL;

struct xkb_context *xkb_context;
struct river_xkb_config_v1 *xkb_config;
struct river_xkb_keymap_v1 *xkb_keymap;
struct river_xkb_bindings_v1 *xkb_bindings;
struct river_input_manager_v1 *input_manager;
struct river_window_manager_v1 *window_manager;
struct river_layer_shell_v1 *layer_shell;
struct river_libinput_config_v1 *libinput_config;

// Swap the positions of two nodes in a wl_list (handles both the adjacent
// and non-adjacent cases). Used to swap a window's place in the tiling
// order without changing the identity of either Window struct, so any
// existing Window* pointers (seat->focused, seat->hovered, etc.) held
// elsewhere stay valid.
void list_swap(struct wl_list *x, struct wl_list *y) {
    if(x == y) return;

    // normalize so that if the nodes are adjacent, x comes right before y
    if(y->next == x) {
        struct wl_list *t = x;
        x = y;
        y = t;
    }

    if(x->next == y) {
        struct wl_list *x_prev = x->prev;
        struct wl_list *y_next = y->next;

        x_prev->next = y; y->prev = x_prev;
        y->next = x;      x->prev = y;
        x->next = y_next; y_next->prev = x;
    } else {
        struct wl_list *x_prev = x->prev, *x_next = x->next;
        struct wl_list *y_prev = y->prev, *y_next = y->next;

        x_prev->next = y; y->prev = x_prev;
        y->next = x_next; x_next->prev = y;
        y_prev->next = x; x->prev = y_prev;
        x->next = y_next; y_next->prev = x;
    }
}

// Find the next (dir > 0) or previous (dir < 0) window that is tiled
// (not floating) and visible on the same output as `w`. Returns NULL if
// there is none.
Window *adjacent_tiled(Window *w, int dir) {
    struct wl_list *node = &w->link;

    for(;;) {
        node = dir > 0 ? node->next : node->prev;
        if(node == &axe.windows) {
            node = dir > 0 ? node->next : node->prev;
        }
        if(node == &w->link) return NULL;

        Window *cand = wl_container_of(node, cand, link);
        if(cand->mon == w->mon && !cand->floating && ISVISIBLE(cand)) return cand;
    }
}

// Find the next (dir > 0) or previous (dir < 0) window visible on the
// same output/tag as `w` - tiled or floating both count. Returns NULL if
// there is none. Naturally wraps around to the first match if `w` is the
// last one, since it keeps walking the circular list until it either
// finds a match or comes back around to `w` itself.
Window *adjacent_visible(Window *w, int dir) {
    struct wl_list *node = &w->link;

    for(;;) {
        node = dir > 0 ? node->next : node->prev;
        if(node == &axe.windows) {
            node = dir > 0 ? node->next : node->prev;
        }
        if(node == &w->link) return NULL;

        Window *cand = wl_container_of(node, cand, link);
        if(cand->mon == w->mon && ISVISIBLE(cand)) return cand;
    }
}

// Single place that changes which window a seat considers focused. Keeps
// the focused-boolean (used e.g. for border color) consistent regardless
// of whether the change came from a click, a keybind, a hover, or an
// automatic refocus - all of those previously updated seat->focused
// directly without touching the boolean, which made border color
// accurate only right after a real mouse click.
void set_focus(Seat *seat, Window *window) {
    if(seat->focused == window) return;

    if(seat->focused != NULL) seat->focused->focused = false;
    seat->focused = window;
    if(window != NULL) window->focused = true;
}

void destroy_window(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        river_window_v1_close(seat->focused->river_window);
    }
}

void select_next_mon(Seat *seat, Arg *arg) {
    if(selmon != NULL) {
        Output *next = wl_container_of(selmon->link.next, selmon, link);
        if(next != NULL && &next->link != &axe.outputs) {
            selmon = next;
            river_seat_v1_pointer_warp(seat->river_seat, selmon->x + selmon->width/2, selmon->y + selmon->height/2);
        }
    }
}

void select_prev_mon(Seat *seat, Arg *arg) {
    if(selmon != NULL) {
        Output *prev = wl_container_of(selmon->link.prev, selmon, link);
        if(prev != NULL && &prev->link != &axe.outputs) {
            selmon = prev;
            river_seat_v1_pointer_warp(seat->river_seat, selmon->x + selmon->width/2, selmon->y + selmon->height/2);
        }
    }
}

void focus_next(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;

    Window *next = adjacent_visible(seat->focused, +1);
    if(next != NULL) {
        set_focus(seat, next);
        river_seat_v1_pointer_warp(seat->river_seat, seat->focused->x + seat->focused->width/2, seat->focused->y + seat->focused->height/2);
    }
}

void focus_prev(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;

    Window *prev = adjacent_visible(seat->focused, -1);
    if(prev != NULL) {
        set_focus(seat, prev);
        river_seat_v1_pointer_warp(seat->river_seat, seat->focused->x + seat->focused->width/2, seat->focused->y + seat->focused->height/2);
    }
}

void incnmaster(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        seat->focused->mon->nmaster += arg->i;
        CLAMP(seat->focused->mon->nmaster, 0, (1 << 16));
    }
}

void setmfact(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        seat->focused->mon->mfact += arg->f;
        CLAMP(seat->focused->mon->mfact, 0, 1);
    }
}

void view(Seat *seat, Arg *arg) {
    if(selmon != NULL) {
        selmon->seltag = arg->u;
        selmon->tagmask = arg->u;
    }
}

void toggleview(Seat *seat, Arg *arg) {
    if(selmon != NULL) {
        selmon->tagmask ^= arg->u;

        if(selmon->tagmask == 0) {
            selmon->tagmask = selmon->seltag;
        }

        // If current selected tag is toggled off, select leftmost viewed tag
        if(arg->u == selmon->seltag) {
            selmon->seltag = selmon->tagmask & -selmon->tagmask;
        }
    }
}

void tag(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        seat->focused->tagmask = arg->u;
    }
}

void toggletag(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        seat->focused->tagmask ^= arg->u;

        if(seat->focused->tagmask == 0) {
            seat->focused->tagmask = selmon->seltag;
        }
    }
}

// Swap the focused window's place in the master/stack order with its
// neighbor (arg->i == +1 for next, -1 for prev). Floating windows don't
// participate in tiling order and are ignored.
void movestack(Seat *seat, Arg *arg) {
    if(seat->focused == NULL || seat->focused->floating) return;

    Window *other = adjacent_tiled(seat->focused, arg->i);
    if(other == NULL) return;

    river_seat_v1_pointer_warp(seat->river_seat, seat->focused->x + seat->focused->width/2, seat->focused->y + seat->focused->height/2);
    list_swap(&seat->focused->link, &other->link);

}

// Swap the focused window into the master slot. If it is already the
// master, swap it with the next tiled window instead (classic dwm zoom).
void zoom(Seat *seat, Arg *arg) {
    Window *w = seat->focused;
    if(w == NULL || w->floating) return;

    Window *master = NULL;
    Window *iter;
    wl_list_for_each(iter, &axe.windows, link) {
        if(iter->mon == w->mon && ISVISIBLE(iter) && !iter->floating) {
            master = iter;
            break;
        }
    }

    if(master == NULL || master == w) {
        Window *other = adjacent_tiled(w, +1);
        if(other != NULL) list_swap(&w->link, &other->link);
        return;
    }

    list_swap(&w->link, &master->link);
}

// Give a window a default centered floating geometry, if it doesn't
// already have one remembered.
void float_default_geometry(Window *w) {
    if(w->floatw != 0 || w->floath != 0) return;

    w->floatw = w->mon->nex_w * 6 / 10;
    w->floath = w->mon->nex_h * 6 / 10;
    w->floatx = (w->mon->nex_w - w->floatw) / 2;
    w->floaty = (w->mon->nex_h - w->floath) / 2;
}

// Toggle the focused window between tiled and floating. Floating windows
// keep their own remembered geometry, defaulted to a centered box the
// first time a window floats.
void togglefloating(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;

    Window *w = seat->focused;
    w->floating = !w->floating;

    if(w->floating) float_default_geometry(w);
}

void togglefullscreen(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;
    seat->focused->fullscreen = !seat->focused->fullscreen;
}

// Start an interactive move of the hovered window via mouse drag. Only
// affects floating windows, as asked - tiled windows don't drag.
void movewin(Seat *seat, Arg *arg) {
    Window *w = seat->hovered;
    if(w == NULL || !w->floating) return;

    seat->op_window = w;
    seat->op_mode = 0;
    seat->op_orig_x = w->floatx;
    seat->op_orig_y = w->floaty;
    river_seat_v1_op_start_pointer(seat->river_seat);
}

// Start an interactive resize of the hovered window via mouse drag. Only
// affects floating windows, as asked.
void resizewin(Seat *seat, Arg *arg) {
    Window *w = seat->hovered;
    if(w == NULL || !w->floating) return;

    seat->op_window = w;
    seat->op_mode = 1;
    seat->op_orig_w = w->floatw;
    seat->op_orig_h = w->floath;
    river_seat_v1_op_start_pointer(seat->river_seat);
}

void exit_session(Seat *seat, Arg *arg) {
    river_window_manager_v1_exit_session(window_manager);
}

void spawn(Seat *seat, Arg *arg) {
    if(fork() == 0) execvp(((char **) arg->v)[0], (char **) arg->v);
}

// include config.h for definition of keybinds
#include "config.h"

// Run every command in config.h's autostart table once at startup. Each
// row is a NULL-terminated argv list; a row whose first element is NULL
// marks the end of the table.
void run_autostart() {
    for(int i = 0; autostart[i][0] != NULL; i++) {
        if(fork() == 0) execvp(autostart[i][0], (char **) autostart[i]);
    }
}

void river_output_v1_removed(void *data, struct river_output_v1 *obj) {
    Output *output = data;

    if(output->river_layer_shell_output != NULL) {
        river_layer_shell_output_v1_destroy(output->river_layer_shell_output);
    }

    river_output_v1_destroy(output->river_output);
    wl_list_remove(&output->link);
    free(output);
}

void river_output_v1_wl_output(void *data, struct river_output_v1 *obj, uint32_t name) {}

void river_output_v1_position(void *data, struct river_output_v1 *obj, int32_t x, int32_t y) {
    Output *output = data;

    output->x = x;
    output->y = y;
    output->nex_x = x;
    output->nex_y = y;
}

void river_output_v1_dimensions(void *data, struct river_output_v1 *obj, int32_t width, int32_t height) {
    Output *output = data;

    output->width = width;
    output->height = height;
    output->nex_w = width;
    output->nex_h = height;
}

const struct river_output_v1_listener output_listener = {
    .removed = river_output_v1_removed,
    .wl_output = river_output_v1_wl_output,
    .position = river_output_v1_position,
    .dimensions = river_output_v1_dimensions,
};

void river_window_v1_closed(void *data, struct river_window_v1 *obj) {
    Window *window = data;

    Seat *seat;
    wl_list_for_each(seat, &axe.seats, link) {
        if(seat->focused == window) {
            set_focus(seat, NULL);
        }
    }

    river_window_v1_destroy(window->river_window);
    wl_list_remove(&window->link);
    free(window);
}

void river_window_v1_dimensions_hint(void *data, struct river_window_v1 *obj, int32_t min_width, int32_t min_height, int32_t max_width, int32_t max_height) {}

void river_window_v1_dimensions(void *data, struct river_window_v1 *obj, int32_t width, int32_t height) {
    struct Window *window = data;
    window->width = width;
    window->height = height;
}

Output *output_by_index(int idx) {
    int i = 0;
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) {
        if(i == idx) return o;
        i++;
    }
    return NULL;
}

void apply_rules(Window *window, const char *app_id) {
    if(app_id == NULL) return;

    for(int i = 0; i < LENGTH(rules); i++) {
        if(strcmp(rules[i].app_id, app_id) != 0) continue;

        if(rules[i].floating) {
            window->floating = true;
            float_default_geometry(window);
        }

        if(rules[i].tag >= 0) {
            window->tagmask = 1u << rules[i].tag;
        }

        if(rules[i].monitor >= 0) {
            Output *o = output_by_index(rules[i].monitor);
            if(o != NULL) window->mon = o;
        }

        return;
    }
}

void river_window_v1_app_id(void *data, struct river_window_v1 *obj, const char *app_id) {
    Window *window = data;
    apply_rules(window, app_id);
}
void river_window_v1_title(void *data, struct river_window_v1 *obj, const char *title) {}
void river_window_v1_parent(void *data, struct river_window_v1 *obj, struct river_window_v1 *parent) {
    Window *window = data;

    if(parent != NULL && !window->floating) {
        window->floating = true;
        float_default_geometry(window);
    }
}
void river_window_v1_decoration_hint(void *data, struct river_window_v1 *obj, uint32_t hint) {}
void river_window_v1_pointer_move_requested(void *data, struct river_window_v1 *obj, struct river_seat_v1 *river_seat) {}
void river_window_v1_pointer_resize_requested(void *data, struct river_window_v1 *obj, struct river_seat_v1 *river_seat, uint32_t edges) {}
void river_window_v1_show_window_menu_requested(void *data, struct river_window_v1 *obj, int32_t x, int32_t y) {}
void river_window_v1_maximize_requested(void *data, struct river_window_v1 *obj) {}
void river_window_v1_unmaximize_requested(void *data, struct river_window_v1 *obj) {}
void river_window_v1_fullscreen_requested(void *data, struct river_window_v1 *obj, struct river_output_v1 *river_output) {
    Window *window = data;
    window->fullscreen = true;

    if(river_output != NULL) {
        Output *output = river_output_v1_get_user_data(river_output);
        if(output != NULL) window->mon = output;
    }
}

void river_window_v1_exit_fullscreen_requested(void *data, struct river_window_v1 *obj) {
    Window *window = data;
    window->fullscreen = false;
}
void river_window_v1_minimize_requested(void *data, struct river_window_v1 *obj) {}
void river_window_v1_unreliable_pid(void *data, struct river_window_v1 *obj, int32_t unreliable_pid) {}
void river_window_v1_presentation_hint(void *data, struct river_window_v1 *obj, uint32_t hint) {}
void river_window_v1_identifier(void *data, struct river_window_v1 *obj, const char *indentifier) {}

const struct river_window_v1_listener window_listener = {
    .closed = river_window_v1_closed,
    .dimensions_hint = river_window_v1_dimensions_hint,
    .dimensions = river_window_v1_dimensions,
    .app_id = river_window_v1_app_id,
    .title = river_window_v1_title,
    .parent = river_window_v1_parent,
    .decoration_hint = river_window_v1_decoration_hint,
    .pointer_move_requested = river_window_v1_pointer_move_requested,
    .pointer_resize_requested = river_window_v1_pointer_resize_requested,
    .show_window_menu_requested = river_window_v1_show_window_menu_requested,
    .maximize_requested = river_window_v1_maximize_requested,
    .unmaximize_requested = river_window_v1_unmaximize_requested,
    .fullscreen_requested = river_window_v1_fullscreen_requested,
    .exit_fullscreen_requested = river_window_v1_exit_fullscreen_requested,
    .minimize_requested = river_window_v1_minimize_requested,
    .unreliable_pid = river_window_v1_unreliable_pid,
    .presentation_hint = river_window_v1_presentation_hint,
    .identifier = river_window_v1_identifier,
};

void window_set_position(Window *window, int x, int y) {
    window->x = x + window->mon->nex_x;
    window->y = y + window->mon->nex_y;
    river_node_v1_set_position(window->river_node, window->x, window->y);
}

void window_set_dimensions(Window *window, int width, int height) {
    window->width = width;
    window->height = height;
    river_window_v1_propose_dimensions(window->river_window, window->width, window->height);
}

void river_xkb_binding_v1_pressed(void *data, struct river_xkb_binding_v1 *obj) {
    Key *key = data;
    key->func(key->seat, key->arg);
}

void river_xkb_binding_v1_released(void *data, struct river_xkb_binding_v1 *obj) {}

const struct river_xkb_binding_v1_listener xkb_binding_listener = {
    .pressed = river_xkb_binding_v1_pressed,
    .released = river_xkb_binding_v1_released,
};

void xkb_binding_create(Seat *seat, uint32_t modifiers, xkb_keysym_t keysym, void (*func)(Seat *seat, Arg *arg), Arg *arg) {
    Key *key = calloc(1, sizeof(Key));
    key->river_xkb_binding = river_xkb_bindings_v1_get_xkb_binding(xkb_bindings, seat->river_seat, keysym, modifiers);
    key->seat = seat;
    key->func = func;
    key->arg = arg;

    river_xkb_binding_v1_add_listener(key->river_xkb_binding, &xkb_binding_listener, key);
    river_xkb_binding_v1_enable(key->river_xkb_binding);

    wl_list_insert(&seat->keys, &key->link);
}

void xkb_binding_destroy(Key *key) {
    river_xkb_binding_v1_destroy(key->river_xkb_binding);
    wl_list_remove(&key->link);
    free(key);
}

void river_pointer_binding_v1_pressed(void *data, struct river_pointer_binding_v1 *obj) {
    Button *button = data;
    button->pressed = true;
    button->func(button->seat, button->arg);
}

void river_pointer_binding_v1_released(void *data, struct river_pointer_binding_v1 *obj) {
    ((Button*) data)->pressed = false;
}

const struct river_pointer_binding_v1_listener pointer_binding_listener = {
    .pressed = river_pointer_binding_v1_pressed,
    .released = river_pointer_binding_v1_released,
};

void pointer_binding_create(Seat *seat, uint32_t modifiers, uint32_t ibutton, void (*func)(Seat *seat, Arg *arg), Arg *arg) {
    Button *button = calloc(1, sizeof(Button));
    button->river_pointer_binding = river_seat_v1_get_pointer_binding(seat->river_seat, ibutton, modifiers);
    button->seat = seat;
    button->func = func;
    button->arg = arg;

    river_pointer_binding_v1_add_listener(button->river_pointer_binding, &pointer_binding_listener, button);
    river_pointer_binding_v1_enable(button->river_pointer_binding);

    wl_list_insert(&seat->buttons, &button->link);
}

void pointer_binding_destroy(Button *button) {
    river_pointer_binding_v1_destroy(button->river_pointer_binding);
    wl_list_remove(&button->link);
    free(button);
}

void river_seat_v1_removed(void *data, struct river_seat_v1 *obj) {
    Seat *seat = data;

    Key *key, *key_tmp;
    wl_list_for_each_safe(key, key_tmp, &seat->keys, link) {
        xkb_binding_destroy(key);
    }

    Button *button, *button_tmp;
    wl_list_for_each_safe(button, button_tmp, &seat->buttons, link) {
        pointer_binding_destroy(button);
    }

    if(seat->river_layer_shell_seat != NULL) {
        river_layer_shell_seat_v1_destroy(seat->river_layer_shell_seat);
    }

    river_seat_v1_destroy(seat->river_seat);
    wl_list_remove(&seat->link);
    free(seat);
}

void river_seat_v1_wl_seat(void *data, struct river_seat_v1 *obj, uint32_t id) {}

void river_seat_v1_pointer_enter(void *data, struct river_seat_v1 *obj, struct river_window_v1 *river_window) {
    Seat *seat = data;
    Window *window = river_window_v1_get_user_data(river_window);

    seat->hovered = window;
    window->hovered = true;

    // focus-follows-mouse
    set_focus(seat, window);
}

void river_seat_v1_pointer_leave(void *data, struct river_seat_v1 *obj) {
    Seat *seat = data;
    Window *window = seat->hovered;

    seat->hovered = NULL;
    window->hovered = false;
}

void river_seat_v1_window_interaction(void *data, struct river_seat_v1 *obj, struct river_window_v1 *river_window) {
    Seat *seat = data;
    Window *window = river_window_v1_get_user_data(river_window);

    set_focus(seat, window);
}

void river_seat_v1_shell_surface_interaction(void *data, struct river_seat_v1 *obj, struct river_shell_surface_v1 *river_shell_surface) {}
void river_seat_v1_op_delta(void *data, struct river_seat_v1 *obj, int32_t dx, int32_t dy) {
    Seat *seat = data;
    if(seat->op_window == NULL) return;

    if(seat->op_mode == 0) { // move
        seat->op_window->floatx = seat->op_orig_x + dx;
        seat->op_window->floaty = seat->op_orig_y + dy;
    } else { // resize
        int w = seat->op_orig_w + dx;
        int h = seat->op_orig_h + dy;
        seat->op_window->floatw = w < 32 ? 32 : w;
        seat->op_window->floath = h < 32 ? 32 : h;
    }
}

void river_seat_v1_op_release(void *data, struct river_seat_v1 *obj) {
    Seat *seat = data;
    seat->op_ending = true;
}

void river_seat_v1_pointer_position(void *data, struct river_seat_v1 *obj, int32_t x, int32_t y) {
    Output *output;
    wl_list_for_each(output, &axe.outputs, link) {
        if(x >= output->x && x < output->x + output->width && y >= output->y && y < output->y + output->height) {
            selmon = output;
            return;
        }
    }

    selmon = NULL;
}

const struct river_seat_v1_listener seat_listener = {
    .removed = river_seat_v1_removed,
    .wl_seat = river_seat_v1_wl_seat,
    .pointer_enter = river_seat_v1_pointer_enter,
    .pointer_leave = river_seat_v1_pointer_leave,
    .window_interaction = river_seat_v1_window_interaction,
    .shell_surface_interaction = river_seat_v1_shell_surface_interaction,
    .op_delta = river_seat_v1_op_delta,
    .op_release = river_seat_v1_op_release,
    .pointer_position = river_seat_v1_pointer_position,
};

void manage_seat(Seat *seat) {
    if(seat->op_ending) {
        river_seat_v1_op_end(seat->river_seat);
        seat->op_window = NULL;
        seat->op_ending = false;
    }

    if(seat->focused == NULL || !ISVISIBLE(seat->focused)) {
        Window *w, *found = NULL;
        wl_list_for_each(w, &axe.windows, link) {
            if(w->mon == selmon && ISVISIBLE(w)) {
                found = w;
                break;
            }
        }

        set_focus(seat, found);

        if(seat->focused != NULL) {
            river_seat_v1_pointer_warp(seat->river_seat, seat->focused->x + seat->focused->width/2, seat->focused->y + seat->focused->height/2);
        }
    }

    if(seat->focused != NULL) {
        river_seat_v1_focus_window(seat->river_seat, seat->focused->river_window);
        river_node_v1_place_top(seat->focused->river_node);
    } else {
        river_seat_v1_clear_focus(seat->river_seat);
    }
}

// The color arguments to set_borders are full 32-bit-per-channel values,
// not the usual 0-255. Widen an 8-bit channel by replicating it across
// all four bytes (0xff -> 0xffffffff), the standard trick for this.
#define CHAN32(c) ((uint32_t)(c) * 0x01010101u)

void render_window(Window *window) {
    const uint8_t *rgba = window->focused ? bordercolor_focus : bordercolor_normal;
    river_window_v1_set_borders(window->river_window,
                                RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
                                RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT,
                                borderpx, CHAN32(rgba[0]), CHAN32(rgba[1]), CHAN32(rgba[2]), CHAN32(rgba[3]));
}

void render_seat(Seat *seat) {}

void river_window_manager_v1_unavailable(void *data, struct river_window_manager_v1 *obj) {
    fprintf(stderr, "error: Unavailable.\n");
    exit(1);
}

void river_window_manager_v1_finished(void *data, struct river_window_manager_v1 *obj) {
    exit(0);
}

int count_tiled_windows(Output *output) {
    int c = 0;
    Window *window;
    wl_list_for_each(window, &axe.windows, link) {
        if(window->mon == output && ISVISIBLE(window) && !window->floating) c++;
    }

    return c;
}

void river_window_manager_v1_manage_start(void *data, struct river_window_manager_v1 *obj) {
    Window *window;
    wl_list_for_each(window, &axe.windows, link) {
        river_window_v1_hide(window->river_window);
    }

    if(selmon != NULL && selmon->river_layer_shell_output != NULL) {
        river_layer_shell_output_v1_set_default(selmon->river_layer_shell_output);
    }

    Output *output;
    wl_list_for_each(output, &axe.outputs, link) {
        int i = 0;
        int n = count_tiled_windows(output);
        int m = output->nmaster;

        Window *prev = NULL;
        wl_list_for_each(window, &axe.windows, link) {
            if(window->mon != output || !ISVISIBLE(window)) continue;

            river_window_v1_show(window->river_window);
            river_window_v1_use_ssd(window->river_window);

            if(window->fullscreen) {
                // The compositor owns position/dimensions of a fullscreen window;
                // set_position/propose_dimensions have no effect while it's active.
                river_window_v1_fullscreen(window->river_window, output->river_output);
                river_window_v1_inform_fullscreen(window->river_window);
                river_node_v1_place_top(window->river_node);
                continue;
            }

            river_window_v1_exit_fullscreen(window->river_window);
            river_window_v1_inform_not_fullscreen(window->river_window);

            if(window->floating) {
                // Floating windows sit outside the master/stack layout entirely,
                // at their own remembered geometry, always above tiled windows.
                river_window_v1_set_tiled(window->river_window, RIVER_WINDOW_V1_EDGES_NONE);
                window_set_position(window, window->floatx, window->floaty);
                window_set_dimensions(window, window->floatw, window->floath);
                river_node_v1_place_top(window->river_node);
                continue;
            }

            river_window_v1_set_tiled(window->river_window,
                                      RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
                                      RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT);

            bool two = m < n && m != 0;

            int si = i < m ? i : i - m;
            int div = two ? (i < m ? m : n - m) : n;

            // Split the column's height evenly, handing the remainder pixels
            // to the first `rem` windows so the column always sums exactly to
            // the usable area's height (no gaps, no overlap).
            int height = output->nex_h / div + (si < output->nex_h % div ? 1 : 0);

            float mfact = two ? output->mfact : 1;

            window_set_position(window, 0, 0);
            window_set_dimensions(window, output->nex_w*mfact, height);

            if(two && i >= m) {
                window_set_position(window, window->width, 0);
                window_set_dimensions(window, output->nex_w - (window->width), window->height);
            }

            if(si != 0) {
                window_set_position(window, window->x - window->mon->nex_x, prev->y + prev->height - window->mon->nex_y);
            }

            prev = window;
            i++;
        }
    }

    Seat *seat;
    wl_list_for_each(seat, &axe.seats, link) {
        manage_seat(seat);
    }

    river_window_manager_v1_manage_finish(window_manager);
}

void river_window_manager_v1_render_start(void *data, struct river_window_manager_v1 *obj) {
    Window *window;
    wl_list_for_each(window, &axe.windows, link) {
        render_window(window);
    }

    Seat *seat;
    wl_list_for_each(seat, &axe.seats, link) {
        render_seat(seat);
    }

    river_window_manager_v1_render_finish(window_manager);
}

void river_window_manager_v1_session_locked(void *data, struct river_window_manager_v1 *obj) {}
void river_window_manager_v1_session_unlocked(void *data, struct river_window_manager_v1 *obj) {}

// New windows are inserted at the front of the list, becoming the master.
void river_window_manager_v1_window(void *data, struct river_window_manager_v1 *obj, struct river_window_v1 *river_window) {
    Window *window = calloc(1, sizeof(Window));
    window->river_window = river_window;
    window->river_node = river_window_v1_get_node(window->river_window);
    window->hovered = false;
    window->focused = false;
    window->mon = selmon;
    window->tagmask = selmon->seltag;

    river_window_v1_add_listener(window->river_window, &window_listener, window);
    wl_list_insert(&axe.windows, &window->link);

    river_window_v1_use_ssd(window->river_window);
    river_window_v1_set_tiled(window->river_window,
                              RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
                              RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT);

    window_set_position(window, 0, 0);
    window_set_dimensions(window, window->mon->nex_w, window->mon->nex_h);

    // Focus the new window for every seat. Fine for the common single-seat
    // desktop; a multi-seat setup may want to only focus for whichever seat
    // actually spawned the window, but we have no way to know that here.
    Seat *seat;
    wl_list_for_each(seat, &axe.seats, link) {
        set_focus(seat, window);
    }
}

void river_layer_shell_output_v1_non_exclusive_area(void *data, struct river_layer_shell_output_v1 *obj, int32_t x, int32_t y, int32_t width, int32_t height) {
    Output *output = data;

    output->nex_x = x;
    output->nex_y = y;
    output->nex_w = width;
    output->nex_h = height;
}

const struct river_layer_shell_output_v1_listener layer_shell_output_listener = {
    .non_exclusive_area = river_layer_shell_output_v1_non_exclusive_area,
};

void river_window_manager_v1_output(void *data, struct river_window_manager_v1 *obj, struct river_output_v1 *river_output) {
    Output *output = calloc(1, sizeof(Output));
    output->river_output = river_output;
    output->nmaster = nmaster;
    output->mfact = mfact;
    output->seltag = 1;
    output->tagmask = 1;

    river_output_v1_add_listener(output->river_output, &output_listener, output);
    wl_list_insert(&axe.outputs, &output->link);

    if(layer_shell != NULL) {
        output->river_layer_shell_output = river_layer_shell_v1_get_output(layer_shell, output->river_output);
        river_layer_shell_output_v1_add_listener(output->river_layer_shell_output, &layer_shell_output_listener, output);
    }

    if(selmon == NULL) selmon = output;
}

// A layer-shell surface (bar, launcher popup, etc.) taking focus is handled
// entirely by the compositor - our own focus_window requests are simply
// ignored while it holds exclusive focus, and manage_seat() re-asserts our
// last-known seat->focused as soon as it's not, so nothing to do here.
void river_layer_shell_seat_v1_focus_exclusive(void *data, struct river_layer_shell_seat_v1 *obj) {}
void river_layer_shell_seat_v1_focus_non_exclusive(void *data, struct river_layer_shell_seat_v1 *obj) {}
void river_layer_shell_seat_v1_focus_none(void *data, struct river_layer_shell_seat_v1 *obj) {}

const struct river_layer_shell_seat_v1_listener layer_shell_seat_listener = {
    .focus_exclusive = river_layer_shell_seat_v1_focus_exclusive,
    .focus_non_exclusive = river_layer_shell_seat_v1_focus_non_exclusive,
    .focus_none = river_layer_shell_seat_v1_focus_none,
};

void river_window_manager_v1_seat(void *data, struct river_window_manager_v1 *obj, struct river_seat_v1 *river_seat) {
    Seat *seat = calloc(1, sizeof(Seat));
    seat->river_seat = river_seat;
    seat->focused = NULL;
    seat->hovered = NULL;

    wl_list_init(&seat->keys);
    wl_list_init(&seat->buttons);

    river_seat_v1_add_listener(seat->river_seat, &seat_listener, seat);
    wl_list_insert(&axe.seats, &seat->link);

    if(layer_shell != NULL) {
        seat->river_layer_shell_seat = river_layer_shell_v1_get_seat(layer_shell, seat->river_seat);
        river_layer_shell_seat_v1_add_listener(seat->river_layer_shell_seat, &layer_shell_seat_listener, seat);
    }

    for(int i = 0; i < LENGTH(keybinds); i++) {
        xkb_binding_create(seat, keybinds[i].mods, keybinds[i].key, keybinds[i].func, &keybinds[i].arg);
    }

    for(int i = 0; i < LENGTH(mousebinds); i++) {
        pointer_binding_create(seat, mousebinds[i].mods, mousebinds[i].button, mousebinds[i].func, &mousebinds[i].arg);
    }
}

const struct river_window_manager_v1_listener window_manager_listener = {
    .unavailable = river_window_manager_v1_unavailable,
    .finished = river_window_manager_v1_finished,
    .manage_start = river_window_manager_v1_manage_start,
    .render_start = river_window_manager_v1_render_start,
    .session_locked = river_window_manager_v1_session_locked,
    .session_unlocked = river_window_manager_v1_session_unlocked,
    .window = river_window_manager_v1_window,
    .output = river_window_manager_v1_output,
    .seat = river_window_manager_v1_seat,
};

extern const struct river_input_device_v1_listener input_device_listener;

void river_input_manager_v1_finished(void *data, struct river_input_manager_v1 *river_input_manager_v1) {}

void river_input_manager_v1_input_device(void *data, struct river_input_manager_v1 *river_input_manager_v1, struct river_input_device_v1 *id) {
    river_input_device_v1_add_listener(id, &input_device_listener, NULL);

    // set_repeat_info is documented as a no-op for non-keyboard devices, so
    // there's no need to wait for the type event first.
    river_input_device_v1_set_repeat_info(id, repeat_rate, repeat_delay);
}

const struct river_input_manager_v1_listener input_manager_listener = {
    .finished = river_input_manager_v1_finished,
    .input_device = river_input_manager_v1_input_device,
};

void river_input_device_v1_removed(void *data, struct river_input_device_v1 *river_input_device_v1) {
    river_input_device_v1_destroy(river_input_device_v1);
}
void river_input_device_v1_type(void *data, struct river_input_device_v1 *river_input_device_v1, uint32_t type) {}
void river_input_device_v1_name(void *data, struct river_input_device_v1 *river_input_device_v1, const char *name) {}

const struct river_input_device_v1_listener input_device_listener = {
    .removed = river_input_device_v1_removed,
    .type = river_input_device_v1_type,
    .name = river_input_device_v1_name,
};

// --- libinput device configuration -----------------------------------
// We deliberately don't track the *_support/*_default/*_current state for
// every property - we just fire off the settings we want once the
// device's initial info batch is complete (the done event) and let
// unsupported settings get silently ignored by the result object below.

void river_libinput_result_v1_success(void *data, struct river_libinput_result_v1 *obj) {
    wl_proxy_destroy((struct wl_proxy *) obj);
}
void river_libinput_result_v1_unsupported(void *data, struct river_libinput_result_v1 *obj) {
    wl_proxy_destroy((struct wl_proxy *) obj);
}
void river_libinput_result_v1_invalid(void *data, struct river_libinput_result_v1 *obj) {
    fprintf(stderr, "libinput config request was invalid\n");
    wl_proxy_destroy((struct wl_proxy *) obj);
}

const struct river_libinput_result_v1_listener libinput_result_listener = {
    .success = river_libinput_result_v1_success,
    .unsupported = river_libinput_result_v1_unsupported,
    .invalid = river_libinput_result_v1_invalid,
};

#define LIBINPUT_APPLY(RESULT_EXPR) \
river_libinput_result_v1_add_listener((RESULT_EXPR), &libinput_result_listener, NULL)

typedef struct {
    struct river_libinput_device_v1 *dev;
    int tap_finger_count;
    struct wl_list link;
} LibinputDevice;

void river_libinput_device_v1_send_events_support(void *data, struct river_libinput_device_v1 *obj, uint32_t modes) {}
void river_libinput_device_v1_send_events_default(void *data, struct river_libinput_device_v1 *obj, uint32_t mode) {}
void river_libinput_device_v1_send_events_current(void *data, struct river_libinput_device_v1 *obj, uint32_t mode) {}
void river_libinput_device_v1_tap_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_tap_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_tap_button_map_default(void *data, struct river_libinput_device_v1 *obj, uint32_t button_map) {}
void river_libinput_device_v1_tap_button_map_current(void *data, struct river_libinput_device_v1 *obj, uint32_t button_map) {}
void river_libinput_device_v1_drag_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_drag_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_drag_lock_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_drag_lock_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_three_finger_drag_support(void *data, struct river_libinput_device_v1 *obj, int32_t finger_count) {}
void river_libinput_device_v1_three_finger_drag_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_three_finger_drag_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_calibration_matrix_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_calibration_matrix_default(void *data, struct river_libinput_device_v1 *obj, struct wl_array *matrix) {}
void river_libinput_device_v1_calibration_matrix_current(void *data, struct river_libinput_device_v1 *obj, struct wl_array *matrix) {}
void river_libinput_device_v1_accel_profiles_support(void *data, struct river_libinput_device_v1 *obj, uint32_t profiles) {}
void river_libinput_device_v1_accel_profile_default(void *data, struct river_libinput_device_v1 *obj, uint32_t profile) {}
void river_libinput_device_v1_accel_profile_current(void *data, struct river_libinput_device_v1 *obj, uint32_t profile) {}
void river_libinput_device_v1_accel_speed_default(void *data, struct river_libinput_device_v1 *obj, struct wl_array *speed) {}
void river_libinput_device_v1_accel_speed_current(void *data, struct river_libinput_device_v1 *obj, struct wl_array *speed) {}
void river_libinput_device_v1_natural_scroll_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_natural_scroll_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_natural_scroll_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_left_handed_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_left_handed_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_left_handed_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_click_method_support(void *data, struct river_libinput_device_v1 *obj, uint32_t methods) {}
void river_libinput_device_v1_click_method_default(void *data, struct river_libinput_device_v1 *obj, uint32_t method) {}
void river_libinput_device_v1_click_method_current(void *data, struct river_libinput_device_v1 *obj, uint32_t method) {}
void river_libinput_device_v1_clickfinger_button_map_default(void *data, struct river_libinput_device_v1 *obj, uint32_t button_map) {}
void river_libinput_device_v1_clickfinger_button_map_current(void *data, struct river_libinput_device_v1 *obj, uint32_t button_map) {}
void river_libinput_device_v1_middle_emulation_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_middle_emulation_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_middle_emulation_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_scroll_method_support(void *data, struct river_libinput_device_v1 *obj, uint32_t methods) {}
void river_libinput_device_v1_scroll_method_default(void *data, struct river_libinput_device_v1 *obj, uint32_t method) {}
void river_libinput_device_v1_scroll_method_current(void *data, struct river_libinput_device_v1 *obj, uint32_t method) {}
void river_libinput_device_v1_scroll_button_default(void *data, struct river_libinput_device_v1 *obj, uint32_t button) {}
void river_libinput_device_v1_scroll_button_current(void *data, struct river_libinput_device_v1 *obj, uint32_t button) {}
void river_libinput_device_v1_scroll_button_lock_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_scroll_button_lock_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_dwt_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_dwt_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_dwt_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_dwtp_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_dwtp_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_dwtp_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_rotation_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_rotation_default(void *data, struct river_libinput_device_v1 *obj, uint32_t angle) {}
void river_libinput_device_v1_rotation_current(void *data, struct river_libinput_device_v1 *obj, uint32_t angle) {}

void river_libinput_device_v1_removed(void *data, struct river_libinput_device_v1 *obj) {
    LibinputDevice *dev = data;
    river_libinput_device_v1_destroy(dev->dev);
    wl_list_remove(&dev->link);
    free(dev);
}

void river_libinput_device_v1_input_device(void *data, struct river_libinput_device_v1 *obj, struct river_input_device_v1 *device) {}

void river_libinput_device_v1_tap_support(void *data, struct river_libinput_device_v1 *obj, int32_t finger_count) {
    ((LibinputDevice *) data)->tap_finger_count = finger_count;
}

// Encode a C double as the native-endian 8-byte wl_array this protocol
// uses in place of a native floating point Wayland argument type.
struct wl_array double_arg(double v) {
    struct wl_array arr;
    wl_array_init(&arr);
    double *p = wl_array_add(&arr, sizeof(double));
    *p = v;
    return arr;
}

void river_libinput_device_v1_done(void *data, struct river_libinput_device_v1 *obj) {
    LibinputDevice *dev = data;

    // Applied to every pointer-capable device (mice and touchpads alike),
    // matching a sway `input * { accel_profile ...; pointer_accel ...; }`
    // block.
    LIBINPUT_APPLY(river_libinput_device_v1_set_accel_profile(dev->dev, pointer_accel_profile));

    struct wl_array speed = double_arg(pointer_accel_speed);
    LIBINPUT_APPLY(river_libinput_device_v1_set_accel_speed(dev->dev, &speed));
    wl_array_release(&speed);

    // libinput has no explicit "is this a touchpad" flag; reporting any
    // tap-to-click finger support is the standard heuristic for one, and
    // matches sway's `input type:touchpad { ... }` block.
    if(dev->tap_finger_count > 0) {
        LIBINPUT_APPLY(river_libinput_device_v1_set_tap(dev->dev, touchpad_tap ? RIVER_LIBINPUT_DEVICE_V1_TAP_STATE_ENABLED : RIVER_LIBINPUT_DEVICE_V1_TAP_STATE_DISABLED));
        LIBINPUT_APPLY(river_libinput_device_v1_set_natural_scroll(dev->dev, touchpad_natural_scroll ? RIVER_LIBINPUT_DEVICE_V1_NATURAL_SCROLL_STATE_ENABLED : RIVER_LIBINPUT_DEVICE_V1_NATURAL_SCROLL_STATE_DISABLED));
        LIBINPUT_APPLY(river_libinput_device_v1_set_dwt(dev->dev, touchpad_dwt ? RIVER_LIBINPUT_DEVICE_V1_DWT_STATE_ENABLED : RIVER_LIBINPUT_DEVICE_V1_DWT_STATE_DISABLED));
        LIBINPUT_APPLY(river_libinput_device_v1_set_click_method(dev->dev, touchpad_click_method));
        LIBINPUT_APPLY(river_libinput_device_v1_set_middle_emulation(dev->dev, touchpad_middle_emulation ? RIVER_LIBINPUT_DEVICE_V1_MIDDLE_EMULATION_STATE_ENABLED : RIVER_LIBINPUT_DEVICE_V1_MIDDLE_EMULATION_STATE_DISABLED));
        LIBINPUT_APPLY(river_libinput_device_v1_set_scroll_method(dev->dev, touchpad_scroll_method));
    }
}

const struct river_libinput_device_v1_listener libinput_device_listener = {
    .removed = river_libinput_device_v1_removed,
    .input_device = river_libinput_device_v1_input_device,
    .send_events_support = river_libinput_device_v1_send_events_support,
    .send_events_default = river_libinput_device_v1_send_events_default,
    .send_events_current = river_libinput_device_v1_send_events_current,
    .tap_support = river_libinput_device_v1_tap_support,
    .tap_default = river_libinput_device_v1_tap_default,
    .tap_current = river_libinput_device_v1_tap_current,
    .tap_button_map_default = river_libinput_device_v1_tap_button_map_default,
    .tap_button_map_current = river_libinput_device_v1_tap_button_map_current,
    .drag_default = river_libinput_device_v1_drag_default,
    .drag_current = river_libinput_device_v1_drag_current,
    .drag_lock_default = river_libinput_device_v1_drag_lock_default,
    .drag_lock_current = river_libinput_device_v1_drag_lock_current,
    .three_finger_drag_support = river_libinput_device_v1_three_finger_drag_support,
    .three_finger_drag_default = river_libinput_device_v1_three_finger_drag_default,
    .three_finger_drag_current = river_libinput_device_v1_three_finger_drag_current,
    .calibration_matrix_support = river_libinput_device_v1_calibration_matrix_support,
    .calibration_matrix_default = river_libinput_device_v1_calibration_matrix_default,
    .calibration_matrix_current = river_libinput_device_v1_calibration_matrix_current,
    .accel_profiles_support = river_libinput_device_v1_accel_profiles_support,
    .accel_profile_default = river_libinput_device_v1_accel_profile_default,
    .accel_profile_current = river_libinput_device_v1_accel_profile_current,
    .accel_speed_default = river_libinput_device_v1_accel_speed_default,
    .accel_speed_current = river_libinput_device_v1_accel_speed_current,
    .natural_scroll_support = river_libinput_device_v1_natural_scroll_support,
    .natural_scroll_default = river_libinput_device_v1_natural_scroll_default,
    .natural_scroll_current = river_libinput_device_v1_natural_scroll_current,
    .left_handed_support = river_libinput_device_v1_left_handed_support,
    .left_handed_default = river_libinput_device_v1_left_handed_default,
    .left_handed_current = river_libinput_device_v1_left_handed_current,
    .click_method_support = river_libinput_device_v1_click_method_support,
    .click_method_default = river_libinput_device_v1_click_method_default,
    .click_method_current = river_libinput_device_v1_click_method_current,
    .clickfinger_button_map_default = river_libinput_device_v1_clickfinger_button_map_default,
    .clickfinger_button_map_current = river_libinput_device_v1_clickfinger_button_map_current,
    .middle_emulation_support = river_libinput_device_v1_middle_emulation_support,
    .middle_emulation_default = river_libinput_device_v1_middle_emulation_default,
    .middle_emulation_current = river_libinput_device_v1_middle_emulation_current,
    .scroll_method_support = river_libinput_device_v1_scroll_method_support,
    .scroll_method_default = river_libinput_device_v1_scroll_method_default,
    .scroll_method_current = river_libinput_device_v1_scroll_method_current,
    .scroll_button_default = river_libinput_device_v1_scroll_button_default,
    .scroll_button_current = river_libinput_device_v1_scroll_button_current,
    .scroll_button_lock_default = river_libinput_device_v1_scroll_button_lock_default,
    .scroll_button_lock_current = river_libinput_device_v1_scroll_button_lock_current,
    .dwt_support = river_libinput_device_v1_dwt_support,
    .dwt_default = river_libinput_device_v1_dwt_default,
    .dwt_current = river_libinput_device_v1_dwt_current,
    .dwtp_support = river_libinput_device_v1_dwtp_support,
    .dwtp_default = river_libinput_device_v1_dwtp_default,
    .dwtp_current = river_libinput_device_v1_dwtp_current,
    .rotation_support = river_libinput_device_v1_rotation_support,
    .rotation_default = river_libinput_device_v1_rotation_default,
    .rotation_current = river_libinput_device_v1_rotation_current,
    .done = river_libinput_device_v1_done,
};

void river_libinput_config_v1_finished(void *data, struct river_libinput_config_v1 *obj) {
    river_libinput_config_v1_destroy(obj);
}

void river_libinput_config_v1_libinput_device(void *data, struct river_libinput_config_v1 *obj, struct river_libinput_device_v1 *id) {
    LibinputDevice *dev = calloc(1, sizeof(LibinputDevice));
    dev->dev = id;
    wl_list_insert(&axe.libinput_devices, &dev->link);
    river_libinput_device_v1_add_listener(id, &libinput_device_listener, dev);
}

const struct river_libinput_config_v1_listener libinput_config_listener = {
    .finished = river_libinput_config_v1_finished,
    .libinput_device = river_libinput_config_v1_libinput_device,
};


typedef struct {
    struct river_xkb_keyboard_v1 *river_xkb_keyboard;

    struct wl_list link;
} Keyboard;

void river_xkb_keyboard_v1_removed(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {
    Keyboard *keyboard = data;

    river_xkb_keyboard_v1_destroy(keyboard->river_xkb_keyboard);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

void river_xkb_keyboard_v1_input_device(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1, struct river_input_device_v1 *device) {}
void river_xkb_keyboard_v1_layout(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1, uint32_t index, const char *name) {}
void river_xkb_keyboard_v1_capslock_enabled(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {}
void river_xkb_keyboard_v1_capslock_disabled(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {}
void river_xkb_keyboard_v1_numlock_enabled(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {}
void river_xkb_keyboard_v1_numlock_disabled(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {}

const struct river_xkb_keyboard_v1_listener xkb_keyboard_listener = {
    .removed = river_xkb_keyboard_v1_removed,
    .input_device = river_xkb_keyboard_v1_input_device,
    .layout = river_xkb_keyboard_v1_layout,
    .capslock_enabled = river_xkb_keyboard_v1_capslock_enabled,
    .capslock_disabled = river_xkb_keyboard_v1_capslock_disabled,
    .numlock_enabled = river_xkb_keyboard_v1_numlock_enabled,
    .numlock_disabled = river_xkb_keyboard_v1_numlock_disabled,
};

void river_xkb_config_v1_finished(void *data, struct river_xkb_config_v1 *river_xkb_config_v1) {
    river_xkb_config_v1_destroy(river_xkb_config_v1);
}

void river_xkb_config_v1_xkb_keyboard(void *data, struct river_xkb_config_v1 *river_xkb_config_v1, struct river_xkb_keyboard_v1 *id) {
    Keyboard *keyboard = calloc(1, sizeof(Keyboard));
    keyboard->river_xkb_keyboard = id;

    wl_list_insert(&axe.keyboards, &keyboard->link);
    river_xkb_keyboard_v1_add_listener(id, &xkb_keyboard_listener, keyboard);

    if(xkb_keymap) {
        river_xkb_keyboard_v1_set_keymap(keyboard->river_xkb_keyboard, xkb_keymap);
    }

    if(numlock_default_on) {
        river_xkb_keyboard_v1_numlock_enable(keyboard->river_xkb_keyboard);
    }
}

const struct river_xkb_config_v1_listener xkb_config_listener = {
    .finished = river_xkb_config_v1_finished,
    .xkb_keyboard = river_xkb_config_v1_xkb_keyboard,
};

// credit to https://git.sr.ht/~zuki/zrwm/tree/afc021dd91bba7a69b1f10fbbf8c5d7bfd66490a/item/zrwm.c#L636
struct river_xkb_keymap_v1* create_keymap() {
    struct xkb_rule_names keymap_rule_names = {0};
    keymap_rule_names.layout = xkb_layout;
    keymap_rule_names.options = xkb_options;

    struct xkb_keymap *keymap = xkb_keymap_new_from_names2(xkb_context, &keymap_rule_names, XKB_KEYMAP_FORMAT_TEXT_V2, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if(keymap == NULL) {
        fprintf(stderr, "Failed to create xkb keymap\n");
        return NULL;
    }

    char *keymap_str = xkb_keymap_get_as_string2(keymap, XKB_KEYMAP_FORMAT_TEXT_V2, XKB_KEYMAP_SERIALIZE_NO_FLAGS);
    xkb_keymap_unref(keymap);
    int keymap_str_len = strlen(keymap_str) + 1;
    int keymap_fd = memfd_create("axe-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if(keymap_fd == -1 || ftruncate(keymap_fd, keymap_str_len) < 0) {
        fprintf(stderr, "Failed to create or truncate mem fd\n");
        close(keymap_fd);
        free(keymap_str);
        return NULL;
    }

    void *data = mmap(NULL, keymap_str_len, PROT_READ | PROT_WRITE, MAP_SHARED, keymap_fd, 0);
    if(data == MAP_FAILED) {
        fprintf(stderr, "Failed to map data\n");
        close(keymap_fd);
        free(keymap_str);
        return NULL;
    }

    memcpy(data, keymap_str, keymap_str_len);
    free(keymap_str);

    if(munmap(data, keymap_str_len) < 0) {
        fprintf(stderr, "Failed to unmap data\n");
        close(keymap_fd);
        free(keymap_str);
        return NULL;
    }

    if(fcntl(keymap_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        fprintf(stderr, "Failed to seal mem fd\n");
        close(keymap_fd);
        free(keymap_str);
        return NULL;
    }

    return river_xkb_config_v1_create_keymap(xkb_config, keymap_fd, XKB_KEYMAP_FORMAT_TEXT_V2);
}

void river_xkb_keymap_v1_success(void *data, struct river_xkb_keymap_v1 *river_xkb_keymap_v1) {
    Keyboard *keyboard;
    wl_list_for_each(keyboard, &axe.keyboards, link) {
        river_xkb_keyboard_v1_set_keymap(keyboard->river_xkb_keyboard, xkb_keymap);
        if(numlock_default_on) {
            river_xkb_keyboard_v1_numlock_enable(keyboard->river_xkb_keyboard);
        }
    }

    fprintf(stderr, "Successfully created keymap\n");
}

void river_xkb_keymap_v1_failure(void *data, struct river_xkb_keymap_v1 *river_xkb_keymap_v1, const char *error_msg) {
    fprintf(stderr, "Failed to create keymap\n");
}

const struct river_xkb_keymap_v1_listener xkb_keymap_listener = {
    .success = river_xkb_keymap_v1_success,
    .failure = river_xkb_keymap_v1_failure,
};

void wl_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if(strcmp(interface, river_window_manager_v1_interface.name) == 0) {
        window_manager = wl_registry_bind(registry, name, &river_window_manager_v1_interface, 4);
        if(window_manager) {
            river_window_manager_v1_add_listener(window_manager, &window_manager_listener, NULL);
        }
    }

    if(strcmp(interface, river_xkb_bindings_v1_interface.name) == 0) {
        xkb_bindings = wl_registry_bind(registry, name, &river_xkb_bindings_v1_interface, 1);
    }

    if(strcmp(interface, river_input_manager_v1_interface.name) == 0) {
        input_manager = wl_registry_bind(registry, name, &river_input_manager_v1_interface, 1);
        if(input_manager != NULL) {
            river_input_manager_v1_add_listener(input_manager, &input_manager_listener, NULL);
        }
    } 

    if(strcmp(interface, river_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(registry, name, &river_layer_shell_v1_interface, 1);
    }

    if(strcmp(interface, river_libinput_config_v1_interface.name) == 0) {
        libinput_config = wl_registry_bind(registry, name, &river_libinput_config_v1_interface, 1);
        if(libinput_config != NULL) {
            river_libinput_config_v1_add_listener(libinput_config, &libinput_config_listener, NULL);
        }
    }

    if(strcmp(interface, river_xkb_config_v1_interface.name) == 0) {
        xkb_config = wl_registry_bind(registry,name,&river_xkb_config_v1_interface, 1);
        if(xkb_config) {
            river_xkb_config_v1_add_listener(xkb_config, &xkb_config_listener, NULL);

            fprintf(stderr, "Trying to create keymap\n");
            xkb_keymap = create_keymap();
            if(xkb_keymap) {
                river_xkb_keymap_v1_add_listener(xkb_keymap, &xkb_keymap_listener, NULL);
            }
        }
    }
}

void wl_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

const struct wl_registry_listener registry_listener = {
    .global = wl_registry_global,
    .global_remove = wl_registry_global_remove,
};

int main() {
    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    struct wl_display *display = wl_display_connect(NULL);
    if(display == NULL) {
        fprintf(stderr, "failed to connect to Wayland server\n");
        return 1;
    }

    signal(SIGCHLD, SIG_IGN);

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if(wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "roundtrip failed\n");
        return 1;
    }

    if(window_manager == NULL || xkb_bindings == NULL) {
        fprintf(stderr, "river_window_manager_v1 or river_xkb_bindings_v1 not supported by the Wayland server\n");
        return 1;
    }

    if(layer_shell == NULL) {
        fprintf(stderr, "warning: river_layer_shell_v1 not available; layer-shell clients (launchers, bars, etc.) will be closed by the compositor\n");
    }

    if(libinput_config == NULL) {
        fprintf(stderr, "warning: river_libinput_config_v1 not available; input device settings in config.h will not be applied\n");
    }

    wl_list_init(&axe.keyboards);
    wl_list_init(&axe.windows);
    wl_list_init(&axe.outputs);
    wl_list_init(&axe.seats);
    wl_list_init(&axe.libinput_devices);

    run_autostart();

    while(true) {
        if(wl_display_dispatch(display) < 0) {
            fprintf(stderr, "dispatch failed\n");
            return 1;
        }
    }

    return 0;
}
