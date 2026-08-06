#include <stdio.h>
#include <stdlib.h>

#include "axe.h"
#include "config.h"

extern void river_window_manager_v1_window(void *data, struct river_window_manager_v1 *obj, struct river_window_v1 *river_window);
extern void river_window_manager_v1_output(void *data, struct river_window_manager_v1 *obj, struct river_output_v1 *river_output);
extern void river_window_manager_v1_seat(void *data, struct river_window_manager_v1 *obj, struct river_seat_v1 *river_seat);

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
        if(window->mon == output && ISVISIBLE(window) && !window->floating && !window->sticky) c++;
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

        output->tiled_count = n;

        bool solo = n <= 1;
        int og = (smart_gap && solo) ? 0 : outer_gap;
        int ig = (smart_gap && solo) ? 0 : inner_gap;

        // Shrink the usable area by outer_gap on every side before any
        // column math runs, so "gap to the screen edge" is exact
        // regardless of how many windows or columns there are.
        int area_w = MAX(output->nex_w - 2 * og, 1);
        int area_h = MAX(output->nex_h - 2 * og, 1);

        // Window *prev = NULL;
        bool two = m < n && m != 0;
        float mfact = two ? output->mfact : 1;
        int master_w = (m == 0) ? 0 : (int) (area_w * mfact);

        int tile_inset = solo ? 0 : (int) borderpx;
        // Half of inner_gap on every side of every tile: between two
        // adjacent tiles that sums to exactly inner_gap; against the
        // outer boundary it stacks with outer_gap (standard gaps
        // behavior - lower outer_gap if you don't want the doubled-up
        // look at the screen edge).
        int gap_inset = ig / 2;
        int inset = tile_inset + gap_inset;

        int prev_slot_bottom = 0;
        wl_list_for_each(window, &axe.windows, link) {
            if(window->mon != output || !ISVISIBLE(window)) continue;

            river_window_v1_show(window->river_window);
            river_window_v1_use_ssd(window->river_window);

            if(window->fullscreen) {
                river_window_v1_fullscreen(window->river_window, output->river_output);
                river_window_v1_inform_fullscreen(window->river_window);
                river_node_v1_place_top(window->river_node);
                continue;
            }

            river_window_v1_exit_fullscreen(window->river_window);
            river_window_v1_inform_not_fullscreen(window->river_window);

            if(window->floating || window->sticky) {
                // Sticky rides the same path as floating here: it uses
                // its own remembered geometry and gets placed above the
                // tiling, which is exactly "pin to top" - see
                // togglesticky() in actions.c. It stays a distinct flag
                // from `floating` itself so untoggling it returns the
                // window to wherever it'd normally tile.
                river_window_v1_set_tiled(window->river_window, RIVER_WINDOW_V1_EDGES_NONE);
                window_set_position(window, window->floatx, window->floaty);
                window_set_dimensions(window, window->floatw, window->floath);
                river_node_v1_place_top(window->river_node);
                continue;
            }

            river_window_v1_set_tiled(window->river_window,
                                      RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
                                      RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT);

            // bool two = m < n && m != 0;

            int si = i < m ? i : i - m;
            int div = two ? (i < m ? m : n - m) : n;


            int slot_h = area_h / div + (si < area_h % div ? 1 : 0);
            int slot_x = og + ((i < m) ? 0 : master_w);
            int slot_w = (i < m) ? master_w : area_w - master_w;
            int slot_y = og + ((si == 0) ? 0 : prev_slot_bottom);
            prev_slot_bottom = (slot_y - og) + slot_h;

            // river_window_v1.dimensions is content size only, unaffected
            // by borders (see protocol) - borders are drawn *outside* it.
            // Inset every side by tile_inset so the border (or the gap
            // where it would be, on a solo window) stays inside this
            // slot instead of bleeding past the output edge or a
            // neighboring tile.
            window_set_position(window, slot_x + inset, slot_y + inset);
            window_set_dimensions(window, MAX(slot_w - 2*inset, 1), MAX(slot_h - 2*inset, 1));

            i++;
        }
    }

    Seat *seat;
    wl_list_for_each(seat, &axe.seats, link) {
        manage_seat(seat);
    }

    bar_redraw_all();
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
