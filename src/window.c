#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "axe.h"
#include "config.h"

// Compiled title regexes, parallel to config.h's rules[] array. Compiled
// once at startup (rules_init(), called from main()) rather than per
// event - regcomp isn't cheap and app_id/title can each fire more than
// once per window.
static regex_t title_regex[LENGTH(rules)];
static bool title_regex_ok[LENGTH(rules)];

void rules_init(void) {
    for(size_t i = 0; i < LENGTH(rules); i++) {
        if(rules[i].title == NULL) continue;

        int err = regcomp(&title_regex[i], rules[i].title, REG_EXTENDED | REG_NOSUB | REG_ICASE);
        if(err != 0) {
            char errbuf[256];
            regerror(err, &title_regex[i], errbuf, sizeof(errbuf));
            fprintf(stderr, "warning: rule[%zu] bad title regex '%s': %s (rule will never match)\n",
                    i, rules[i].title, errbuf);
            continue;
        }
        title_regex_ok[i] = true;
    }
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

// Sets floatw/floath/floatx/floaty from a rule's requested fraction of
// the monitor's usable area, centered. No-op if the window already has
// a remembered float geometry (e.g. restored via restart.c, or set by
// an earlier, more specific rule match) - rule-based default size only
// ever fills in a *blank* geometry, same spirit as float_default_geometry().
static void apply_rule_float_geometry(Window *window, float width_frac, float height_frac) {
    if(width_frac <= 0 || height_frac <= 0) return;
    if(window->floatw != 0 || window->floath != 0) return;
    if(window->mon->nex_w <= 0 || window->mon->nex_h <= 0) return;

    window->floatw = (int)(window->mon->nex_w * width_frac);
    window->floath = (int)(window->mon->nex_h * height_frac);
    if(window->floatw < 1) window->floatw = 1;
    if(window->floath < 1) window->floath = 1;

    window->floatx = (window->mon->nex_w - window->floatw) / 2;
    window->floaty = (window->mon->nex_h - window->floath) / 2;
}

// Re-evaluated from scratch every time window->app_id or window->title
// changes (see river_window_v1_app_id/title below), not just once. A
// rule whose app_id/title we don't know yet is skipped rather than
// treated as a mismatch, so as more identifying info arrives (app_id
// usually first, title sometimes later - or the reverse for some
// clients) the result self-corrects instead of locking in a premature
// decision. Put more specific rules earlier in rules[]; first full
// match wins.
void apply_rules(Window *window) {
    for(size_t i = 0; i < LENGTH(rules); i++) {
        if(rules[i].app_id != NULL) {
            if(window->app_id[0] == '\0') continue; // not known yet
            if(strcmp(rules[i].app_id, window->app_id) != 0) continue;
        }

        if(rules[i].title != NULL) {
            if(window->title[0] == '\0') continue; // not known yet
            if(!title_regex_ok[i]) continue;        // bad pattern, never matches
            if(regexec(&title_regex[i], window->title, 0, NULL, 0) != 0) continue;
        }

        if(rules[i].monitor >= 0) {
            Output *o = output_by_index(rules[i].monitor);
            if(o != NULL) window->mon = o;
        }

        if(rules[i].floating == 1) {
            window->floating = true;
            window->floating_explicit = true;
            apply_rule_float_geometry(window, rules[i].float_width, rules[i].float_height);
            float_default_geometry(window);
        } else if(rules[i].floating == 0) {
            window->floating = false;
            window->floating_explicit = true;
        }

        if(rules[i].tag >= 0) {
            window->tagmask = 1u << rules[i].tag;
        }

        return;
    }
}

void window_set_position(Window *window, int x, int y) {
    int abs_x = x + window->mon->nex_x;
    int abs_y = y + window->mon->nex_y;
    // if(abs_x == window->x && abs_y == window->y) return; // nothing changed, skip the wire request
    if(window->got_position && abs_x == window->x && abs_y == window->y) return;
    window->got_position = true;
    window->x = abs_x;
    window->y = abs_y;
    river_node_v1_set_position(window->river_node, window->x, window->y);
}

void window_set_dimensions(Window *window, int width, int height) {
    if(width == window->width && height == window->height) return;
    window->width = width;
    window->height = height;
    river_window_v1_propose_dimensions(window->river_window, window->width, window->height);
}

// The color arguments to set_borders are full 32-bit-per-channel values,
// not the usual 0-255. Widen an 8-bit channel by replicating it across
// all four bytes (0xff -> 0xffffffff), the standard trick for this.
#define CHAN32(c) ((uint32_t)(c) * 0x01010101u)

void render_window(Window *window) {
    if(window->floating || window->sticky) {
        river_window_v1_set_borders(window->river_window,
                                    RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
                                    RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT,
                                    borderpx, CHAN32(bordercolor_float[0]), CHAN32(bordercolor_float[1]),
                                    CHAN32(bordercolor_float[2]), CHAN32(bordercolor_float[3]));
        return;
    }

    if(window->mon->tiled_count <= 1) {
        river_window_v1_set_borders(window->river_window, RIVER_WINDOW_V1_EDGES_NONE, 0, 0, 0, 0, 0);
        return;
    }

    const uint8_t *rgba = window->focused ? bordercolor_focus : bordercolor_normal;
    river_window_v1_set_borders(window->river_window,
                                RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
                                RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT,
                                borderpx, CHAN32(rgba[0]), CHAN32(rgba[1]), CHAN32(rgba[2]), CHAN32(rgba[3]));
}

void river_window_v1_closed(void *data, struct river_window_v1 *obj) {
    Window *window = data;

    // FIX: previously only seat->focused was cleared here. seat->hovered
    // and seat->op_window (mid interactive move/resize) could still point
    // at this Window after it's freed below, leading to a use-after-free
    // the next time pointer_leave/op_delta/op_release fires for that seat.
    Seat *seat;
    wl_list_for_each(seat, &axe.seats, link) {
        if(seat->focused == window) {
            Window *next = adjacent_visible(window, -1);
            if(next == NULL) next = adjacent_visible(window, +1);
            set_focus(seat, next);
        }
        if(seat->hovered == window) {
            seat->hovered = NULL;
        }
        if(seat->op_window == window) {
            seat->op_window = NULL;
            seat->op_ending = false;
        }
    }

    river_window_v1_destroy(window->river_window);
    wl_list_remove(&window->link);
    free(window);
}

void river_window_v1_dimensions_hint(void *data, struct river_window_v1 *obj, int32_t min_width, int32_t min_height, int32_t max_width, int32_t max_height) {}

// Is `window` the subject of an active interactive move/resize on any
// seat? While true, op_delta() in seat.c is the authoritative source for
// floatw/floath/floatx/floaty - a dimensions echo below is a response to
// our own in-flight per-frame proposal, not new information. Acting on
// it anyway is what was causing the resize flicker/CPU spike (propose ->
// echo -> recenter -> propose (now moved) -> echo -> recenter -> ...)
// and the "resize fights back" feeling.
static bool window_has_active_op(Window *window) {
    Seat *seat;
    wl_list_for_each(seat, &axe.seats, link) {
        if(seat->op_window == window) return true;
    }
    return false;
}

void river_window_v1_dimensions(void *data, struct river_window_v1 *obj, int32_t width, int32_t height) {
    struct Window *window = data;
    window->width = width;
    window->height = height;

    if(!window->floating) {
        window->got_real_dimensions = true;
        return;
    }

    // Mid-drag, ignore this entirely - see window_has_active_op() above.
    if(window_has_active_op(window)) return;

    // If fullscreen don't overwrite the position/geometry
    if(window->fullscreen) return;

    bool first_real_size = !window->got_real_dimensions;
    window->got_real_dimensions = true;

    if(width == window->floatw && height == window->floath) return; // just an echo, nothing changed

    window->floatw = width;
    window->floath = height;
    clamp_float_geometry(window, window->mon);

    // Only recenter the one time we first learn this window's real size
    // - i.e. right when it's established as floating. Later adoptions
    // (a video player switching resolution, or a client pushing back on
    // a size we proposed) intentionally leave floatx/y alone, per your
    // ask: recenter on creation only, not on every resize.
    if(first_real_size) {
        window->floatx = (window->mon->nex_w - window->floatw) / 2;
        window->floaty = (window->mon->nex_h - window->floath) / 2;
    }
}

void river_window_v1_app_id(void *data, struct river_window_v1 *obj, const char *app_id) {
    Window *window = data;
    if(app_id != NULL) {
        strncpy(window->app_id, app_id, sizeof(window->app_id) - 1);
        window->app_id[sizeof(window->app_id) - 1] = '\0';
    }
    apply_rules(window);
}

void river_window_v1_title(void *data, struct river_window_v1 *obj, const char *title) {
    Window *window = data;
    if(title != NULL) {
        strncpy(window->title, title, sizeof(window->title) - 1);
        window->title[sizeof(window->title) - 1] = '\0';
    }
    apply_rules(window);
}

// void river_window_v1_title(void *data, struct river_window_v1 *obj, const char *title) {}
void river_window_v1_parent(void *data, struct river_window_v1 *obj, struct river_window_v1 *parent) {
    Window *window = data;

    if(parent != NULL && !window->floating && !window->floating_explicit) {
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
        // if(output != NULL) window->mon = output;
        if(output != NULL && output != window->mon) {
            window->pre_fullscreen_mon = window->mon;
            window->mon = output;
        }
    }
}

void river_window_v1_exit_fullscreen_requested(void *data, struct river_window_v1 *obj) {
    Window *window = data;
    window->fullscreen = false;

    if(window->pre_fullscreen_mon != NULL) {
        window->mon = window->pre_fullscreen_mon;
        window->pre_fullscreen_mon = NULL;
    }
}
void river_window_v1_minimize_requested(void *data, struct river_window_v1 *obj) {}
void river_window_v1_unreliable_pid(void *data, struct river_window_v1 *obj, int32_t unreliable_pid) {}
void river_window_v1_presentation_hint(void *data, struct river_window_v1 *obj, uint32_t hint) {}

void river_window_v1_identifier(void *data, struct river_window_v1 *obj, const char *identifier) {
    Window *window = data;
    strncpy(window->identifier, identifier, sizeof(window->identifier) - 1);
    window->identifier[sizeof(window->identifier) - 1] = '\0';
    apply_saved_window_state(window);
}

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

// New windows are inserted at the front of the list, becoming the master.
void river_window_manager_v1_window(void *data, struct river_window_manager_v1 *obj, struct river_window_v1 *river_window) {
    Window *window = calloc(1, sizeof(Window));
    window->river_window = river_window;
    window->river_node = river_window_v1_get_node(window->river_window);
    window->hovered = false;
    window->focused = false;
    window->width = -1;
    window->height = -1;
    // window->mon = selmon;
    // window->tagmask = selmon->seltag;

    // Defensive: an output should always exist before any window does,
    // but don't trust it blindly - fall back to whatever output exists
    // rather than dereferencing a NULL selmon.
    Output *mon = selmon;
    if(mon == NULL && !wl_list_empty(&axe.outputs)) {
        mon = wl_container_of(axe.outputs.next, mon, link);
    }
    if(mon == NULL) {
        fprintf(stderr, "warning: window announced with no output available yet, dropping\n");
        free(window);
        return;
    }
    window->mon = mon;
    window->tagmask = window->mon->seltag;

    river_window_v1_add_listener(window->river_window, &window_listener, window);
    wl_list_insert(&axe.windows, &window->link);

    river_window_v1_use_ssd(window->river_window);
    river_window_v1_set_tiled(window->river_window,
                              RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
                              RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT);

    // window_set_position(window, 0, 0);
    // window_set_dimensions(window, window->mon->nex_w, window->mon->nex_h);

    // Focus the new window for every seat. Fine for the common single-seat
    // desktop; a multi-seat setup may want to only focus for whichever seat
    // actually spawned the window, but we have no way to know that here.
    Seat *seat;
    wl_list_for_each(seat, &axe.seats, link) {
        set_focus(seat, window);
    }
}
