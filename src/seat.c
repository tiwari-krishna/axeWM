#include <stdlib.h>

#include "axe.h"
#include "config.h"

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

    idle_teardown_seat(seat);

    river_seat_v1_destroy(seat->river_seat);
    wl_list_remove(&seat->link);
    free(seat);
}

void river_seat_v1_wl_seat(void *data, struct river_seat_v1 *obj, uint32_t id) {
    idle_attach_wl_seat((Seat *) data, id);
}

void river_seat_v1_pointer_enter(void *data, struct river_seat_v1 *obj, struct river_window_v1 *river_window) {
    Seat *seat = data;
    Window *window = river_window_v1_get_user_data(river_window);

    seat->hovered = window;
    window->hovered = true;

    // focus-follows-mouse
    // set_focus(seat, window);
}

void river_seat_v1_pointer_leave(void *data, struct river_seat_v1 *obj) {
    Seat *seat = data;
    Window *window = seat->hovered;

    seat->hovered = NULL;
    if(window != NULL) window->hovered = false;
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

    Window *ow = seat->op_window;

    if(seat->op_mode == 0) { // move
        ow->floatx = seat->op_orig_x + dx;
        ow->floaty = seat->op_orig_y + dy;

        // Skip the manage/render round trip if this tick didn't actually
        // move anything (sub-pixel jitter, duplicate motion events).
        if(ow->floatx == seat->op_last_x && ow->floaty == seat->op_last_y) return;
        seat->op_last_x = ow->floatx;
        seat->op_last_y = ow->floaty;
    } else { // resize
        int w = seat->op_move_x ? seat->op_orig_w - dx : seat->op_orig_w + dx;
        int h = seat->op_move_y ? seat->op_orig_h - dy : seat->op_orig_h + dy;
        if(w < 32) w = 32;
        if(h < 32) h = 32;

        ow->floatw = w;
        ow->floath = h;
        // Re-derive x/y from the (possibly clamped) w/h so the opposite
        // edge stays exactly anchored even once the 32px minimum kicks in.
        ow->floatx = seat->op_move_x ? seat->op_orig_x + (seat->op_orig_w - w) : seat->op_orig_x;
        ow->floaty = seat->op_move_y ? seat->op_orig_y + (seat->op_orig_h - h) : seat->op_orig_y;

        if(ow->floatx == seat->op_last_x && ow->floaty == seat->op_last_y &&
            ow->floatw == seat->op_last_w && ow->floath == seat->op_last_h) return;
        seat->op_last_x = ow->floatx;
        seat->op_last_y = ow->floaty;
        seat->op_last_w = ow->floatw;
        seat->op_last_h = ow->floath;
    }
    river_window_manager_v1_manage_dirty(window_manager);
}

void river_seat_v1_op_release(void *data, struct river_seat_v1 *obj) {
    Seat *seat = data;
    seat->op_ending = true;
}

void river_seat_v1_pointer_position(void *data, struct river_seat_v1 *obj, int32_t x, int32_t y) {
    Seat *seat = data;
    seat->pointer_x = x;
    seat->pointer_y = y;

    if(seat->op_window != NULL) return;

    // Focus-follows-mouse lives here, not in pointer_enter(): this event
    // only fires on genuine pointer motion, so focus only ever follows a
    // cursor you actually moved - not a window that reflowed underneath
    // a stationary one. seat->hovered is kept accurate by
    // pointer_enter/pointer_leave regardless of why it changed; we just
    // wait for real motion before acting on it.
    if(seat->hovered != NULL && seat->hovered != seat->focused && ISVISIBLE(seat->hovered)) {
        set_focus(seat, seat->hovered);
    }

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
        Window *found = NULL;

        // Prefer whatever was last focused while viewing this tag on
        // this output - this is the actual fix for "switching tags and
        // back always lands on master/the first tab": without it, the
        // scan below just grabs the first ISVISIBLE window in list
        // order every time, which is always master since new windows
        // are inserted at the front (see window.c).
        if(selmon != NULL) {
            Window *remembered = selmon->last_focused[tagidx(selmon)];
            if(remembered != NULL && remembered->mon == selmon && ISVISIBLE(remembered)) {
                found = remembered;
            }
        }

        if(found == NULL) {
            Window *w;
            wl_list_for_each(w, &axe.windows, link) {
                if(w->mon == selmon && ISVISIBLE(w)) {
                    found = w;
                    break;
                }
            }
        }

        set_focus(seat, found);
    }

    if(seat->focused != NULL) {
        river_seat_v1_focus_window(seat->river_seat, seat->focused->river_window);
        // river_node_v1_place_top(seat->focused->river_node);
        if(seat->focused->floating || seat->focused->sticky || seat->focused->fullscreen ||
            (seat->focused->mon != NULL && seat->focused->mon->layout[tagidx(seat->focused->mon)] == LAYOUT_TABBED)) {
            river_node_v1_place_top(seat->focused->river_node);
        }
        if(seat->pending_warp && (seat->pending_warp_any_state || (!seat->focused->floating && !seat->focused->sticky))) {
            Window *w = seat->focused;
            int cx, cy;
            if(w->fullscreen && w->mon != NULL) {
                // wm.c never calls window_set_position/window_set_dimensions
                // for fullscreen windows (it just tells the compositor to
                // fullscreen it and leaves the geometry to it) - w->x/y are
                // stale leftovers from before it went fullscreen. Warp to
                // the output's center instead.
                cx = w->mon->nex_x + w->mon->nex_w / 2;
                cy = w->mon->nex_y + w->mon->nex_h / 2;
            } else {
                cx = w->x + w->width / 2;
                cy = w->y + w->height / 2;
            }
            river_seat_v1_pointer_warp(seat->river_seat, cx, cy);
        }
    } else {
        river_seat_v1_clear_focus(seat->river_seat);
    }
    seat->pending_warp = false;
    seat->pending_warp_any_state = false;
}

void render_seat(Seat *seat) {}

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

    for(size_t i = 0; i < LENGTH(keybinds); i++) {
        xkb_binding_create(seat, keybinds[i].mods, keybinds[i].key, keybinds[i].func, &keybinds[i].arg);
    }

    for(size_t i = 0; i < LENGTH(mousebinds); i++) {
        pointer_binding_create(seat, mousebinds[i].mods, mousebinds[i].button, mousebinds[i].func, &mousebinds[i].arg);
    }
    bar_setup_seat_autohide(seat);
    marksui_setup_seat(seat);
}
