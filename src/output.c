#include <stdlib.h>

#include "axe.h"
#include "config.h"

void river_output_v1_removed(void *data, struct river_output_v1 *obj) {
    Output *output = data;
    bar_destroy(output);

    // FIX: selmon previously kept pointing at this Output after it's freed
    // below if this happened to be the currently-selected monitor (e.g. a
    // monitor unplug/disable) - a use-after-free the next time anything
    // reads selmon. Repoint it at another remaining output, or NULL if
    // this was the last one.
    // if(selmon == output) {
    //     if(output->link.next != &axe.outputs) {
    //         selmon = wl_container_of(output->link.next, selmon, link);
    //     } else if(output->link.prev != &axe.outputs) {
    //         selmon = wl_container_of(output->link.prev, selmon, link);
    //     } else {
    //         selmon = NULL;
    //     }
    // }

    // Pick a fallback output (another remaining one, or NULL if this was
    // the last one) - used both to repoint selmon and to rehome any
    // windows that were living on this output.
    Output *fallback = NULL;
    if(output->link.next != &axe.outputs) {
        fallback = wl_container_of(output->link.next, fallback, link);
    } else if(output->link.prev != &axe.outputs) {
        fallback = wl_container_of(output->link.prev, fallback, link);
    }

    // FIX: selmon previously kept pointing at this Output after it's freed
    // below if this happened to be the currently-selected monitor (e.g. a
    // monitor unplug/disable) - a use-after-free the next time anything
    // reads selmon.
    if(selmon == output) {
        selmon = fallback;
    }

    // FIX: any Window still on this output kept a dangling ->mon pointer
    // after it's freed below - a use-after-free the next time ISVISIBLE()
    // or anything else dereferences window->mon. Rehome them to the
    // fallback output; if there's nowhere left to put them, ask the
    // client to close rather than leave a dangling/NULL Output pointer.
    Window *window;
    wl_list_for_each(window, &axe.windows, link) {
        if(window->pre_fullscreen_mon == output) {
            window->pre_fullscreen_mon = NULL;
        }
        if(window->mon != output) continue;
        if(fallback != NULL) {
            window->mon = fallback;
            // // Remembered float geometry was relative to the old monitor's
            // // resolution - clamp into the new one so a window floating on
            // // a big panel doesn't land off-screen on a smaller projector.
            // if(window->floatw > fallback->nex_w) window->floatw = fallback->nex_w;
            // if(window->floath > fallback->nex_h) window->floath = fallback->nex_h;
            // if(window->floatx + window->floatw > fallback->nex_w) window->floatx = fallback->nex_w - window->floatw;
            // if(window->floaty + window->floath > fallback->nex_h) window->floaty = fallback->nex_h - window->floath;
            // if(window->floatx < 0) window->floatx = 0;
            // if(window->floaty < 0) window->floaty = 0;
            clamp_float_geometry(window, fallback);
        } else {
            // FIX: this was the last output, so there's nowhere to rehome
            // this window - but river_window_v1_close() only *requests*
            // a close; the window sticks around (with a dangling ->mon
            // pointing at the Output we're about to free() below) until
            // the client's closed event actually arrives. Anything that
            // dereferences ->mon in the meantime (ISVISIBLE, bar redraw,
            // manage_seat's fallback focus search, ...) would read freed
            // memory. NULL it out - ISVISIBLE() and friends treat a NULL
            // mon as "not visible", which is exactly right for a window
            // that no longer has anywhere to be shown.
            window->mon = NULL;
            river_window_v1_close(window->river_window);

            // No output left means no valid focus target either - drop
            // any seat's reference to this window so nothing dereferences
            // its (now NULL) ->mon before the close actually completes.
            Seat *seat;
            wl_list_for_each(seat, &axe.seats, link) {
                if(seat->focused == window) seat->focused = NULL;
                if(seat->hovered == window) seat->hovered = NULL;
                if(seat->op_window == window) {
                    seat->op_window = NULL;
                    seat->op_ending = false;
                }
            }
        }
    }

    if(output->river_layer_shell_output != NULL) {
        river_layer_shell_output_v1_destroy(output->river_layer_shell_output);
    }

    if(output->power != NULL) {
       zwlr_output_power_v1_destroy(output->power);
    }
    if(output->wl_output != NULL) {
        wl_output_destroy(output->wl_output); // v1-bound, no release request available
    }

    river_output_v1_destroy(output->river_output);
    wl_list_remove(&output->link);
    free(output);
}

void river_output_v1_wl_output(void *data, struct river_output_v1 *obj, uint32_t name) {
    idle_attach_wl_output((Output *) data, name);
    bar_output_ready((Output *) data);
}

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

    apply_saved_output_state(output);

    if(layer_shell != NULL) {
        output->river_layer_shell_output = river_layer_shell_v1_get_output(layer_shell, output->river_output);
        river_layer_shell_output_v1_add_listener(output->river_layer_shell_output, &layer_shell_output_listener, output);
    }

    if(selmon == NULL) selmon = output;
}
