#include <stdlib.h>
#include <unistd.h>

#include "axe.h"

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
        if(cand->mon == w->mon && !cand->floating && !cand->sticky && ISVISIBLE(cand)) return cand;
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
    if(window != NULL) {
        window->focused = true;
        if(window->mon != NULL) {
            window->mon->last_focused[tagidx(window->mon)] = window;
        }
    }
}

// Give a window a default centered floating geometry, if it doesn't
// already have one remembered.
void float_default_geometry(Window *w) {
    if(w->floatw != 0 || w->floath != 0) return;

    if(w->got_real_dimensions) {
        w->floatw = w->width;
        w->floath = w->height;
        clamp_float_geometry(w, w->mon);
        w->floatx = (w->mon->nex_w - w->floatw) / 2;
        w->floaty = (w->mon->nex_h - w->floath) / 2;
    }
}

// Clamp a window's remembered floating geometry (size and position) to
// fit within mon's usable area - needed whenever a window ends up on a
// monitor other than the one its floatx/y/w/h were computed for (rules,
// movemon, an output disappearing) since that monitor may differ in size.
void clamp_float_geometry(Window *w, Output *mon) {
    if(mon->nex_w <= 0 || mon->nex_h <= 0) return;

    if(w->floatw > mon->nex_w) w->floatw = mon->nex_w;
    if(w->floath > mon->nex_h) w->floath = mon->nex_h;
    if(w->floatw < 1) w->floatw = 1;
    if(w->floath < 1) w->floath = 1;

    if(w->floatx + w->floatw > mon->nex_w) w->floatx = mon->nex_w - w->floatw;
    if(w->floaty + w->floath > mon->nex_h) w->floaty = mon->nex_h - w->floath;
    if(w->floatx < 0) w->floatx = 0;
    if(w->floaty < 0) w->floaty = 0;
}

// include config.h for definition of keybinds/mousebinds/autostart, and
// the constants (nmaster, mfact, ...) referenced below.
#include "config.h"

// Run every command in config.h's autostart table once at startup. Each
// row is a NULL-terminated argv list; a row whose first element is NULL
// marks the end of the table.
void run_autostart(void) {
    for(int i = 0; autostart[i][0] != NULL; i++) {
        if(fork() == 0) {
            execvp(autostart[i][0], (char **) autostart[i]);
            _exit(EXIT_FAILURE);
        }
    }
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

void movemon(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;

    Window *w = seat->focused;
    struct wl_list *node = arg->i > 0 ? w->mon->link.next : w->mon->link.prev;
    if(node == &axe.outputs) return;

    Output *target = wl_container_of(node, target, link);
    if(target == w->mon) return;

    w->mon = target;
    clamp_float_geometry(w, target);

    selmon = target;
    seat->pending_warp = true;
    river_window_manager_v1_manage_dirty(window_manager);
}

void focus_next(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;

    Window *next = adjacent_visible(seat->focused, +1);
    if(next != NULL) {
        set_focus(seat, next);
        river_seat_v1_pointer_warp(seat->river_seat, seat->focused->x + seat->focused->width/2, seat->focused->y + seat->focused->height/2);
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

void focus_prev(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;

    Window *prev = adjacent_visible(seat->focused, -1);
    if(prev != NULL) {
        set_focus(seat, prev);
        river_seat_v1_pointer_warp(seat->river_seat, seat->focused->x + seat->focused->width/2, seat->focused->y + seat->focused->height/2);
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

void incnmaster(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        Output *mon = seat->focused->mon;
        int t = tagidx(mon);
        mon->nmaster[t] += arg->i;
        CLAMP(mon->nmaster[t], 0, (1 << 16));
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

void setmfact(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        Output *mon = seat->focused->mon;
        int t = tagidx(mon);
        mon->mfact[t] += arg->f;
        CLAMP(mon->mfact[t], 0, 1);
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

void view(Seat *seat, Arg *arg) {
    if(selmon != NULL) {
        selmon->seltag = arg->u;
        selmon->tagmask = arg->u;
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

// Jump straight to the Nth (1-based, arg->i) tiled+visible window on
// selmon, in the same order tabbar.c numbers and lists them - so the
// "(3)" printed on a tab and the keybind that lands on it always agree.
// Works in any layout, not just LAYOUT_TABBED (it's just "select tiled
// window N"), but that's the main use: jumping straight to a tab instead
// of stepping through with focus_next/focus_prev. Silently does nothing
// if there's no Nth tiled window (arg->i out of range, or arg->i < 1).
void tabselect(Seat *seat, Arg *arg) {
    if(selmon == NULL || arg->i < 1) return;

    int idx = 0;
    Window *win;
    wl_list_for_each(win, &axe.windows, link) {
        if(win->mon != selmon || win->floating || win->sticky || !ISVISIBLE(win)) continue;
        idx++;
        if(idx != arg->i) continue;

        set_focus(seat, win);
        river_window_manager_v1_manage_dirty(window_manager);
        return;
    }
}

void togglebar(Seat *seat, Arg *arg) {
    bar_toggle();
}

// "Passthrough" mode: disable every keybind and mousebind - on every
// seat - except this one (the way back out), so a game or VM gets every
// keystroke and click with nothing intercepted along the way.
//
// Deliberately NOT disabled: any hold-to-reveal gesture (currently just
// the bar/tabbar Super-hold peek - see bar_setup_seat_autohide in
// bar.c, the only thing using xkb_hold_binding_create and therefore the
// only Key with a non-NULL release_func). The bar keeps behaving exactly
// as it does outside passthrough - shows while Super is held, hides on
// release, stays out of the way (no forced-visible override) otherwise,
// which matters since the whole point is usually a fullscreen game.
// Checking it mid-game is a real key event, which as a simple side
// effect also resets ext-idle-notify-v1's idle timer - not something
// built on purpose, just a side benefit of not disabling it.
//
// Pure river_xkb_binding_v1/river_pointer_binding_v1 enable/disable
// toggling - no window-management state changes, so no manage_dirty.
void togglepassthrough(Seat *seat, Arg *arg) {
    passthrough = !passthrough;

    Seat *s;
    wl_list_for_each(s, &axe.seats, link) {
        Key *key;
        wl_list_for_each(key, &s->keys, link) {
            if(key->func == togglepassthrough || key->release_func != NULL) continue;
            if(passthrough) river_xkb_binding_v1_disable(key->river_xkb_binding);
            else river_xkb_binding_v1_enable(key->river_xkb_binding);
        }

        Button *button;
        wl_list_for_each(button, &s->buttons, link) {
            if(passthrough) river_pointer_binding_v1_disable(button->river_pointer_binding);
            else river_pointer_binding_v1_enable(button->river_pointer_binding);
        }
    }
}

void toggleview(Seat *seat, Arg *arg) {
    if(selmon == NULL) return;

    // FIX: the previous version XORed the bit off and then, if that made
    // tagmask == 0, immediately reset tagmask back to selmon->seltag -
    // which at that point is still the exact bit that was just removed.
    // The net effect was a silent no-op instead of "keep at least one tag
    // visible": toggling off the only viewed tag appeared to do nothing.
    // Make that explicit: refuse to toggle off the last visible tag.
    uint32_t new_mask = selmon->tagmask ^ arg->u;
    if(new_mask == 0) return;

    selmon->tagmask = new_mask;

    // If current selected tag is toggled off, select leftmost viewed tag
    if(arg->u == selmon->seltag) {
        selmon->seltag = selmon->tagmask & -selmon->tagmask;
    }
    river_window_manager_v1_manage_dirty(window_manager);
}

void tag(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        seat->focused->tagmask = arg->u;
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

void toggletag(Seat *seat, Arg *arg) {
    if(seat->focused != NULL) {
        seat->focused->tagmask ^= arg->u;

        if(seat->focused->tagmask == 0) {
            seat->focused->tagmask = selmon->seltag;
        }
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

// Swap the focused window's place in the master/stack order with its
// neighbor (arg->i == +1 for next, -1 for prev). Floating windows don't
// participate in tiling order and are ignored.
void movestack(Seat *seat, Arg *arg) {
    if(seat->focused == NULL || seat->focused->floating || seat->focused->sticky) return;
    // if(seat->focused->mon != NULL && seat->focused->mon->layout == LAYOUT_TABBED) return;

    Window *other = adjacent_tiled(seat->focused, arg->i);
    if(other == NULL) return;

    list_swap(&seat->focused->link, &other->link);
    seat->pending_warp = true;
    river_window_manager_v1_manage_dirty(window_manager);
}

// Swap the focused window into the master slot. If it is already the
// master, swap it with the next tiled window instead (classic dwm zoom).
void zoom(Seat *seat, Arg *arg) {
    Window *w = seat->focused;
    if(w == NULL || w->floating || w->sticky) return;

    Window *master = NULL;
    Window *iter;
    wl_list_for_each(iter, &axe.windows, link) {
        if(iter->mon == w->mon && ISVISIBLE(iter) && !iter->floating && !iter->sticky) {
            master = iter;
            break;
        }
    }

    if(master == NULL || master == w) {
        Window *other = adjacent_tiled(w, +1);
        // if(other != NULL) list_swap(&w->link, &other->link);
        if(other != NULL) {
            list_swap(&w->link, &other->link);
            seat->pending_warp = true;
            river_window_manager_v1_manage_dirty(window_manager);
        }
        return;
    }

    list_swap(&w->link, &master->link);
    seat->pending_warp = true;
    river_window_manager_v1_manage_dirty(window_manager);
}

// Toggle the focused window between tiled and floating. Floating windows
// keep their own remembered geometry, defaulted to a centered box the
// first time a window floats.
void togglefloating(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;

    Window *w = seat->focused;
    w->floating = !w->floating;

    if(w->floating) float_default_geometry(w);
    river_window_manager_v1_manage_dirty(window_manager);
}

void togglefullscreen(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;
    seat->focused->fullscreen = !seat->focused->fullscreen;
    river_window_manager_v1_manage_dirty(window_manager);
}

// Toggle "show on every tag, always on top" for the focused window.
// Tiled+sticky doesn't make sense (a sticky window ignores tags/tiling
// entirely, so a "tiled slot" for it is meaningless) - stickying always
// forces the window floating too. Un-stickying deliberately leaves it
// floating: no auto-revert to tiled. If you want it back in the tiling,
// toggle floating explicitly with togglefloating.
void togglesticky(Seat *seat, Arg *arg) {
    if(seat->focused == NULL) return;

    Window *w = seat->focused;
    w->sticky = !w->sticky;

    if(w->sticky) {
        w->floating = true;
        float_default_geometry(w);
    }

    river_window_manager_v1_manage_dirty(window_manager);
}

// Toggle the current monitor between normal tiling and the tabbed
// layout (one full-size slot per tiled window, no gaps/borders,
// switched via an i3-style tab strip - see tabbar.c). Per-output, same
// as nmaster/mfact, so different monitors can run different layouts.
void togglelayout(Seat *seat, Arg *arg) {
    if(selmon == NULL) return;
    int t = tagidx(selmon);
    selmon->layout[t] = (selmon->layout[t] == LAYOUT_TABBED) ? LAYOUT_TILE : LAYOUT_TABBED;
    river_window_manager_v1_manage_dirty(window_manager);
}


// Start an interactive move of the hovered window via mouse drag. Only
// affects floating windows, as asked - tiled windows don't drag.
void movewin(Seat *seat, Arg *arg) {
    if(seat->op_window != NULL) return;

    Window *w = seat->hovered;
    if(w == NULL || !w->floating) return;

    seat->op_window = w;
    seat->op_mode = 0;
    seat->op_orig_x = w->floatx;
    seat->op_orig_y = w->floaty;
    seat->op_last_x = w->floatx;
    seat->op_last_y = w->floaty;
    river_seat_v1_op_start_pointer(seat->river_seat);
}

// Start an interactive resize of the hovered window via mouse drag. Only
// affects floating windows, as asked.
void resizewin(Seat *seat, Arg *arg) {
    if(seat->op_window != NULL) return;
    Window *w = seat->hovered;
    if(w == NULL || !w->floating) return;

    seat->op_window = w;
    seat->op_mode = 1;
    seat->op_orig_x = w->floatx;
    seat->op_orig_y = w->floaty;
    seat->op_orig_w = w->floatw;
    seat->op_orig_h = w->floath;
    seat->op_last_x = w->floatx;
    seat->op_last_y = w->floaty;
    seat->op_last_w = w->floatw;
    seat->op_last_h = w->floath;

    // Which corner was grabbed determines which edges move: the
    // left/top half of the window anchors the opposite (right/bottom)
    // edge and drags the near edge with the pointer; the right/bottom
    // half keeps the classic grow-from-top-left behavior. Each axis is
    // independent, so all four corners work.
    seat->op_move_x = (seat->pointer_x - w->x) < w->width / 2;
    seat->op_move_y = (seat->pointer_y - w->y) < w->height / 2;

    river_seat_v1_op_start_pointer(seat->river_seat);
}

void exit_session(Seat *seat, Arg *arg) {
    river_window_manager_v1_exit_session(window_manager);
}

void spawn(Seat *seat, Arg *arg) {
    if(fork() == 0) {
        execvp(((char **) arg->v)[0], (char **) arg->v);
        _exit(EXIT_FAILURE);
    }
}
