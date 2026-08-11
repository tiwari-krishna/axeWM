#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "axe.h"

// Two independent, ordering-agnostic problems solved here:
//
// 1. We need a raw wl_seat/wl_output to hand to ext-idle-notify-v1 and
//    wlr-output-power-management-unstable-v1 - river's own protocol
//    only gives us their *registry names* via river_seat_v1.wl_seat /
//    river_output_v1.wl_output, so we bind them eagerly whenever the
//    registry advertises them and hold them in a small pending list
//    until the matching river event tells us which Seat/Output they
//    belong to.
//
// 2. ext_idle_notifier_v1 / zwlr_output_power_manager_v1 themselves may
//    be bound before or after any given Seat/Output exists - no
//    ordering is guaranteed between unrelated globals - so both
//    directions call back into each other: attaching a wl_seat/wl_output
//    tries to finish setup immediately, and binding either manager
//    retroactively sweeps every Seat/Output that's already waiting.

typedef struct {
    uint32_t name;
    struct wl_seat *wl_seat;
    struct wl_list link;
} PendingSeat;

typedef struct {
    uint32_t name;
    struct wl_output *wl_output;
    struct wl_list link;
} PendingOutput;

typedef struct {
    const IdleTimeout *cfg;
    struct ext_idle_notification_v1 *notification;
    struct wl_list link;
} IdleWatcher;

static struct wl_list pending_seats;
static struct wl_list pending_outputs;
static bool lists_initialized = false;

static void ensure_lists(void) {
    if(lists_initialized) return;
    wl_list_init(&pending_seats);
    wl_list_init(&pending_outputs);
    lists_initialized = true;
}

void idle_track_wl_seat(struct wl_registry *registry, uint32_t name) {
    ensure_lists();
    PendingSeat *p = calloc(1, sizeof(PendingSeat));
    p->name = name;
    p->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    wl_list_insert(&pending_seats, &p->link);
}

void idle_track_wl_output(struct wl_registry *registry, uint32_t name) {
    ensure_lists();
    PendingOutput *p = calloc(1, sizeof(PendingOutput));
    p->name = name;
    p->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    wl_list_insert(&pending_outputs, &p->link);
}

static void spawn_cmd(const char *cmd) {
    if(cmd == NULL) return;
    if(fork() == 0) {
        execlp("/bin/sh", "/bin/sh", "-c", cmd, NULL);
        fprintf(stderr, "idle: exec failed for '%s'\n", cmd);
        _exit(EXIT_FAILURE);
    }
}

static void watcher_idled(void *data, struct ext_idle_notification_v1 *obj) {
    spawn_cmd(((IdleWatcher *) data)->cfg->command);
}
static void watcher_resumed(void *data, struct ext_idle_notification_v1 *obj) {
    spawn_cmd(((IdleWatcher *) data)->cfg->resume_command);
}
static const struct ext_idle_notification_v1_listener watcher_listener = {
    .idled = watcher_idled,
    .resumed = watcher_resumed,
};

static void display_idled(void *data, struct ext_idle_notification_v1 *obj) {
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) {
        if(o->power != NULL) zwlr_output_power_v1_set_mode(o->power, ZWLR_OUTPUT_POWER_V1_MODE_OFF);
    }
}
static void display_resumed(void *data, struct ext_idle_notification_v1 *obj) {
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) {
        if(o->power != NULL) zwlr_output_power_v1_set_mode(o->power, ZWLR_OUTPUT_POWER_V1_MODE_ON);
    }
}
static const struct ext_idle_notification_v1_listener display_listener = {
    .idled = display_idled,
    .resumed = display_resumed,
};

#include "config.h" // for IdleTimeout idle_timeouts[] / display_off_timeout_ms

static void setup_seat_idle(Seat *seat) {
    if(seat->wl_seat == NULL || idle_notifier == NULL || seat->idle_setup_done) return;
    seat->idle_setup_done = true;

    wl_list_init(&seat->idle_watchers);

    for(size_t i = 0; i < LENGTH(idle_timeouts); i++) {
        IdleWatcher *w = calloc(1, sizeof(IdleWatcher));
        w->cfg = &idle_timeouts[i];
        w->notification = ext_idle_notifier_v1_get_idle_notification(idle_notifier, idle_timeouts[i].timeout_ms, seat->wl_seat);
        ext_idle_notification_v1_add_listener(w->notification, &watcher_listener, w);
        wl_list_insert(&seat->idle_watchers, &w->link);
    }

    if(display_off_timeout_ms > 0) {
        seat->display_notification = ext_idle_notifier_v1_get_idle_notification(idle_notifier, display_off_timeout_ms, seat->wl_seat);
        ext_idle_notification_v1_add_listener(seat->display_notification, &display_listener, NULL);
    }
}

static void output_power_mode(void *data, struct zwlr_output_power_v1 *obj, uint32_t mode) {}

static void output_power_failed(void *data, struct zwlr_output_power_v1 *obj) {
    Output *output = data;
    zwlr_output_power_v1_destroy(obj);
    if(output->power == obj) output->power = NULL;
}

static const struct zwlr_output_power_v1_listener output_power_listener = {
    .mode = output_power_mode,
    .failed = output_power_failed,
};

static void ensure_output_power(Output *output) {
    if(output->wl_output == NULL || power_manager == NULL || output->power != NULL) return;
    output->power = zwlr_output_power_manager_v1_get_output_power(power_manager, output->wl_output);
    zwlr_output_power_v1_add_listener(output->power, &output_power_listener, output);
}

void idle_attach_wl_seat(Seat *seat, uint32_t name) {
    ensure_lists();
    PendingSeat *p, *tmp;
    wl_list_for_each_safe(p, tmp, &pending_seats, link) {
        if(p->name != name) continue;
        seat->wl_seat = p->wl_seat;
        wl_list_remove(&p->link);
        free(p);
        break;
    }
    setup_seat_idle(seat);
}

void idle_attach_wl_output(Output *output, uint32_t name) {
    ensure_lists();
    PendingOutput *p, *tmp;
    wl_list_for_each_safe(p, tmp, &pending_outputs, link) {
        if(p->name != name) continue;
        output->wl_output = p->wl_output;
        wl_list_remove(&p->link);
        free(p);
        break;
    }
    ensure_output_power(output);
}

void idle_notifier_ready(void) {
    Seat *s;
    wl_list_for_each(s, &axe.seats, link) {
        setup_seat_idle(s);
    }
}

void idle_power_manager_ready(void) {
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) {
        ensure_output_power(o);
    }
}

// Called from main.c's wl_registry_global_remove. A wl_seat/wl_output
// global can in principle disappear before river ever sends the matching
// river_seat_v1.wl_seat / river_output_v1.wl_output event that would
// claim it out of the pending list below - without this, that entry (and
// the wl_seat/wl_output proxy it holds) would leak for the life of the
// process. Already-claimed seats/outputs are unaffected: their wl_seat/
// wl_output is torn down by idle_teardown_seat()/river_output_v1_removed()
// via the normal river removal path, not here.
void idle_registry_global_remove(uint32_t name) {
    ensure_lists();

    PendingSeat *ps, *ps_tmp;
    wl_list_for_each_safe(ps, ps_tmp, &pending_seats, link) {
        if(ps->name != name) continue;
        wl_seat_destroy(ps->wl_seat);
        wl_list_remove(&ps->link);
        free(ps);
        return;
    }

    PendingOutput *po, *po_tmp;
    wl_list_for_each_safe(po, po_tmp, &pending_outputs, link) {
        if(po->name != name) continue;
        wl_output_destroy(po->wl_output);
        wl_list_remove(&po->link);
        free(po);
        return;
    }
}

void idle_teardown_seat(Seat *seat) {
    if(seat->idle_setup_done) {
        IdleWatcher *w, *w_tmp;
        wl_list_for_each_safe(w, w_tmp, &seat->idle_watchers, link) {
            ext_idle_notification_v1_destroy(w->notification);
            wl_list_remove(&w->link);
            free(w);
        }
        if(seat->display_notification != NULL) {
            ext_idle_notification_v1_destroy(seat->display_notification);
        }
    }
    if(seat->wl_seat != NULL) {
        wl_seat_destroy(seat->wl_seat);
    }
}
