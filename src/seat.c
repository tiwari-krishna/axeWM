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

    if(seat->op_mode == 0) { // move
        seat->op_window->floatx = seat->op_orig_x + dx;
        seat->op_window->floaty = seat->op_orig_y + dy;
    } else { // resize
        // int w = seat->op_orig_w + dx;
        // int h = seat->op_orig_h + dy;
        // seat->op_window->floatw = w < 32 ? 32 : w;
        // seat->op_window->floath = h < 32 ? 32 : h;
        int w = seat->op_move_x ? seat->op_orig_w - dx : seat->op_orig_w + dx;
        int h = seat->op_move_y ? seat->op_orig_h - dy : seat->op_orig_h + dy;
        if(w < 32) w = 32;
        if(h < 32) h = 32;

        seat->op_window->floatw = w;
        seat->op_window->floath = h;
        // Re-derive x/y from the (possibly clamped) w/h so the opposite
        // edge stays exactly anchored even once the 32px minimum kicks in.
        seat->op_window->floatx = seat->op_move_x ? seat->op_orig_x + (seat->op_orig_w - w) : seat->op_orig_x;
        seat->op_window->floaty = seat->op_move_y ? seat->op_orig_y + (seat->op_orig_h - h) : seat->op_orig_y;
    }
}

void river_seat_v1_op_release(void *data, struct river_seat_v1 *obj) {
    Seat *seat = data;
    seat->op_ending = true;
}

void river_seat_v1_pointer_position(void *data, struct river_seat_v1 *obj, int32_t x, int32_t y) {
    Seat *seat = data;
    seat->pointer_x = x;
    seat->pointer_y = y;

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
        // river_node_v1_place_top(seat->focused->river_node);
        if(seat->focused->floating || seat->focused->fullscreen) {
            river_node_v1_place_top(seat->focused->river_node);
        }
    } else {
        river_seat_v1_clear_focus(seat->river_seat);
    }
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
}
