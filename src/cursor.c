#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

// cursor.c - hide the pointer cursor after a period of no mouse
// movement specifically (not keyboard activity, not any other input).
//
// river only lets the window manager control the cursor image while no
// client has pointer focus (river_seat_v1 v4+, see its description) -
// normally that's just the empty desktop, or during an interactive
// move/resize. Per river's maintainer (isaacfreund, on the tracker): the
// way to get that state on demand is to start a pointer op
// (river_seat_v1.op_start_pointer) - doing so strips pointer focus from
// whatever client currently has it for the whole seat, handing cursor
// control to us for as long as the op stays open. Setting a NULL surface
// via wl_pointer.set_cursor then hides it; ending the op
// (river_seat_v1.op_end) hands cursor control straight back to whatever
// window is under the pointer, which sets its own cursor again exactly
// as it would normally.
//
// Two separate problems, two separate signals:
//
// - DECIDING WHEN TO HIDE needs to know "N ms with no mouse movement."
//   river_seat_v1.pointer_position only fires when x/y actually changes
//   (so it's already movement-specific, unlike keyboard activity), but
//   the protocol explicitly does NOT guarantee it's delivered promptly
//   on its own: "a change in pointer position alone must not cause the
//   compositor to start a manage sequence" - it only piggybacks on
//   manage sequences that happen for some other reason. So rather than
//   trying to catch every single motion tick, this just records the
//   last time we *did* see the position change, and a small periodic
//   timer (independent of any Wayland event) checks elapsed time against
//   that and requests a hide once it's been long enough. This is an
//   honest approximation given the protocol's limits, not a guarantee of
//   hiding at exactly the configured millisecond - but it's still purely
//   driven by real position changes, never by keyboard input.
//
// - DECIDING WHEN TO SHOW AGAIN, once hidden, has a much cleaner answer:
//   op_delta. Its own description says events "will be sent based on
//   pointer input" for as long as our op is open, with no dependency on
//   any button being held - so the very first op_delta after we hide is
//   unambiguous, protocol-guaranteed proof of real mouse movement, and
//   it's *also* always followed by a manage_start, so ending our op
//   right there is always done from a valid manage-sequence context.
//
// Same op-collision hazard as before applies to movewin/resizewin
// (actions.c) - see the comment there.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include "axe.h"
#include "config.h"

// How often the "has it been long enough" check runs. Independent of
// cursor_hide_timeout_ms itself - just needs to be short enough that the
// actual hide doesn't lag the configured timeout noticeably.
#define CURSOR_CHECK_INTERVAL_MS 250

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// --------------------------------------------------------------------
// seat setup - wl_pointer itself is acquired eagerly by idle.c (see its
// PendingSeat handling), at the point wl_seat is bound, specifically so
// the compositor's one-shot initial capabilities burst isn't missed by
// attaching a listener too late. By the time river_seat_v1.wl_seat fires
// and this gets called, seat->wl_pointer may already be populated - this
// just sets up the periodic check timer.
// --------------------------------------------------------------------

void cursor_attach_wl_seat(Seat *seat) {
    if(seat->wl_seat == NULL || seat->cursor_check_timer_fd >= 0) return;
    if(cursor_hide_timeout_ms <= 0) return;

    seat->cursor_last_motion_ms = now_ms();
    seat->cursor_check_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(seat->cursor_check_timer_fd < 0) {
        fprintf(stderr, "cursor: timerfd_create failed: %s\n", strerror(errno));
        return;
    }
    struct itimerspec its = {0};
    its.it_value.tv_nsec = (long) CURSOR_CHECK_INTERVAL_MS * 1000000L;
    its.it_interval.tv_nsec = (long) CURSOR_CHECK_INTERVAL_MS * 1000000L;
    timerfd_settime(seat->cursor_check_timer_fd, 0, &its, NULL);
}

void cursor_teardown_seat(Seat *seat) {
    if(seat->cursor_check_timer_fd >= 0) {
        close(seat->cursor_check_timer_fd);
        seat->cursor_check_timer_fd = -1;
    }
    if(seat->wl_pointer != NULL) {
        wl_pointer_destroy(seat->wl_pointer);
        seat->wl_pointer = NULL;
    }
}

int cursor_seat_timer_fd(Seat *seat) {
    return seat->cursor_check_timer_fd;
}

// --------------------------------------------------------------------
// motion tracking
// --------------------------------------------------------------------

// Called from seat.c on every genuine river_seat_v1.pointer_position
// event, and on every op_delta while our own hide-op is open (see
// river_seat_v1_op_delta in seat.c) - both are real, protocol-confirmed
// pointer motion, never keyboard or anything else.
void cursor_pointer_moved(Seat *seat) {
    seat->cursor_last_motion_ms = now_ms();

    if(seat->cursor_hidden) {
        seat->pending_cursor_show = true;
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

// Called from main.c when a seat's check timer fires (every
// CURSOR_CHECK_INTERVAL_MS while the feature is enabled).
void cursor_timer_fired(Seat *seat) {
    uint64_t n;
    if(read(seat->cursor_check_timer_fd, &n, sizeof(n)) < 0) { /* just draining the timerfd */ }

    if(cursor_hide_timeout_ms <= 0 || seat->cursor_hidden) return;

    if(now_ms() - seat->cursor_last_motion_ms >= cursor_hide_timeout_ms) {
        seat->pending_cursor_hide = true;
        river_window_manager_v1_manage_dirty(window_manager);
    }
}

// --------------------------------------------------------------------
// applying the pending state - called from manage_seat() (seat.c),
// every manage sequence, so op_start_pointer/op_end are always issued
// from a genuinely valid context.
// --------------------------------------------------------------------

void cursor_apply_seat(Seat *seat) {
    if(seat->pending_cursor_hide) {
        seat->pending_cursor_hide = false;

        // Never steal the op slot out from under a real drag in
        // progress (movewin/resizewin) - if one's active, just skip;
        // there's nothing to hide during an active drag anyway.
        if(!seat->cursor_hidden && seat->op_window == NULL) {
            river_seat_v1_op_start_pointer(seat->river_seat);
            seat->cursor_hidden = true;

            if(seat->wl_pointer != NULL) {
                // Serial is ignored by the compositor for WM-issued
                // set_cursor calls (river_seat_v1 description, v4+) -
                // any value works. A NULL surface hides the cursor
                // (core wl_pointer.set_cursor semantics).
                wl_pointer_set_cursor(seat->wl_pointer, 0, NULL, 0, 0);
            } else {
                fprintf(stderr, "cursor: op started but wl_pointer is still "
                        "NULL - the cursor won't actually hide. "
                        "This shouldn't happen; please report it.\n");
            }
        }
    }

    if(seat->pending_cursor_show) {
        seat->pending_cursor_show = false;

        // Only end an op we ourselves started for this - never touch a
        // real move/resize op, and never double-end.
        if(seat->cursor_hidden) {
            river_seat_v1_op_end(seat->river_seat);
            seat->cursor_hidden = false;
        }
    }
}
