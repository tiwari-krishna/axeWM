// axe-idle.c
//
// Standalone idle-timeout daemon - what swayidle is to sway. Runs
// arbitrary commands after N ms of seat inactivity, and separately
// powers outputs off/on around its own dedicated display-off timeout.
// Deliberately independent of axe: idle detection (ext-idle-notify-v1)
// and display power (wlr-output-power-management-unstable-v1) are both
// plain Wayland globals any client can bind - no river-specific
// protocol involved, no window-manager-client relationship needed.
// Autostart this the same way you'd autostart swayidle.

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include <ext-idle-notify-v1-client-protocol.h>
#include <wlr-output-power-management-unstable-v1-client-protocol.h>

// ---------------------------------------------------------------------
// config - edit and recompile, same convention as axe's config.h
// ---------------------------------------------------------------------

typedef struct {
    int timeout_ms;
    const char *command;         // run once, when idle for timeout_ms
    const char *resume_command;  // run on resume, NULL to skip
} IdleTimeout;

static const IdleTimeout timeouts[] = {
    // { 180000, "brightnessctl -s set 10%", "brightnessctl -r" },
    { 1800000, "systemctl suspend",                 NULL },
};

// Dedicated display-off timeout: turns every output off via
// wlr-output-power-management-unstable-v1, back on on resume.
// 0 or negative disables this.
static const int display_off_timeout_ms = 60000;

// ---------------------------------------------------------------------

static struct wl_seat *seat;
static struct ext_idle_notifier_v1 *idle_manager;
static struct zwlr_output_power_manager_v1 *power_manager;
static struct wl_list outputs;

typedef struct {
    struct wl_output *wl_output;
    struct zwlr_output_power_v1 *power;
    struct wl_list link;
} Output;

static void spawn(const char *cmd) {
    if(cmd == NULL) return;
    if(fork() == 0) {
        execlp("/bin/sh", "/bin/sh", "-c", cmd, NULL);
        fprintf(stderr, "axe-idle: exec failed for '%s'\n", cmd);
        _exit(EXIT_FAILURE);
    }
}

// ---------------------------------------------------------------------
// generic timeout -> command
// ---------------------------------------------------------------------

typedef struct {
    const IdleTimeout *cfg;
    struct ext_idle_notification_v1 *notification;
} TimeoutWatcher;

static TimeoutWatcher watchers[sizeof(timeouts) / sizeof(timeouts[0])];

static void timeout_idled(void *data, struct ext_idle_notification_v1 *obj) {
    spawn(((TimeoutWatcher *) data)->cfg->command);
}

static void timeout_resumed(void *data, struct ext_idle_notification_v1 *obj) {
    spawn(((TimeoutWatcher *) data)->cfg->resume_command);
}

static const struct ext_idle_notification_v1_listener timeout_listener = {
    .idled = timeout_idled,
    .resumed = timeout_resumed,
};

// ---------------------------------------------------------------------
// dedicated display-off timeout
// ---------------------------------------------------------------------

static struct ext_idle_notification_v1 *display_notification;

static void display_idled(void *data, struct ext_idle_notification_v1 *obj) {
    Output *o;
    wl_list_for_each(o, &outputs, link) {
        if(o->power != NULL) zwlr_output_power_v1_set_mode(o->power, ZWLR_OUTPUT_POWER_V1_MODE_OFF);
    }
}

static void display_resumed(void *data, struct ext_idle_notification_v1 *obj) {
    Output *o;
    wl_list_for_each(o, &outputs, link) {
        if(o->power != NULL) zwlr_output_power_v1_set_mode(o->power, ZWLR_OUTPUT_POWER_V1_MODE_ON);
    }
}

static const struct ext_idle_notification_v1_listener display_listener = {
    .idled = display_idled,
    .resumed = display_resumed,
};

// ---------------------------------------------------------------------
// output tracking - only needed to drive the display-off timeout
// ---------------------------------------------------------------------

static void ensure_output_power(Output *o) {
    if(o->power != NULL || power_manager == NULL) return;
    o->power = zwlr_output_power_manager_v1_get_output_power(power_manager, o->wl_output);
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if(strcmp(interface, wl_seat_interface.name) == 0 && seat == NULL) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if(strcmp(interface, wl_output_interface.name) == 0) {
        Output *o = calloc(1, sizeof(Output));
        o->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        wl_list_insert(&outputs, &o->link);
        ensure_output_power(o); // no-op if power_manager isn't bound yet
    } else if(strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        idle_manager = wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 1);
    } else if(strcmp(interface, zwlr_output_power_manager_v1_interface.name) == 0) {
        power_manager = wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1);
    }
}

// Not tracked by registry name here, so an output unplugged mid-session
// leaks its Output/zwlr_output_power_v1 - harmless and bounded for a
// long-running idle daemon, same tradeoff axe's own restart.c makes
// elsewhere, but flagging it rather than pretending it's handled.
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void) {
    signal(SIGCHLD, SIG_IGN);
    wl_list_init(&outputs);

    struct wl_display *display = wl_display_connect(NULL);
    if(display == NULL) {
        fprintf(stderr, "axe-idle: failed to connect to Wayland display\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if(seat == NULL || idle_manager == NULL) {
        fprintf(stderr, "axe-idle: wl_seat or ext_idle_notifier_v1 unavailable\n");
        return 1;
    }

    if(display_off_timeout_ms > 0 && power_manager == NULL) {
        fprintf(stderr, "axe-idle: warning: display_off_timeout_ms is set but zwlr_output_power_manager_v1 is unavailable\n");
    }

    Output *o;
    wl_list_for_each(o, &outputs, link) {
        ensure_output_power(o);
    }

    for(size_t i = 0; i < sizeof(timeouts) / sizeof(timeouts[0]); i++) {
        watchers[i].cfg = &timeouts[i];
        watchers[i].notification = ext_idle_notifier_v1_get_idle_notification(
            idle_manager, timeouts[i].timeout_ms, seat);
        ext_idle_notification_v1_add_listener(watchers[i].notification, &timeout_listener, &watchers[i]);
    }

    if(display_off_timeout_ms > 0) {
        display_notification = ext_idle_notifier_v1_get_idle_notification(
            idle_manager, display_off_timeout_ms, seat);
        ext_idle_notification_v1_add_listener(display_notification, &display_listener, NULL);
    }

    while(true) {
        if(wl_display_dispatch(display) < 0) {
            fprintf(stderr, "axe-idle: dispatch failed\n");
            return 1;
        }
    }

    return 0;
}
