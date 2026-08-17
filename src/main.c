#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#include "axe.h"

WindowManager axe;
Output *selmon = NULL;
bool passthrough = false;

struct xkb_context *xkb_context;
struct river_xkb_config_v1 *xkb_config;
struct river_xkb_keymap_v1 *xkb_keymap;
struct river_xkb_bindings_v1 *xkb_bindings;
struct river_input_manager_v1 *input_manager;
struct river_window_manager_v1 *window_manager;
struct river_layer_shell_v1 *layer_shell;
struct river_libinput_config_v1 *libinput_config;
struct ext_idle_notifier_v1 *idle_notifier;
struct zwlr_output_power_manager_v1 *power_manager;

struct wl_compositor *compositor;
struct wl_shm *shm;
struct zwlr_layer_shell_v1 *wlr_layer_shell;

void wl_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if(strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
        bar_manager_ready();
        tabbar_manager_ready();
    }

    if(strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        bar_manager_ready();
        tabbar_manager_ready();
    }

    if(strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wlr_layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
        bar_manager_ready();
        tabbar_manager_ready();
    }
    if(strcmp(interface, wl_seat_interface.name) == 0) {
        idle_track_wl_seat(registry, name);
    }

    if(strcmp(interface, wl_output_interface.name) == 0) {
        idle_track_wl_output(registry, name);
    }

    if(strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        idle_notifier = wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 1);
        idle_notifier_ready();
    }

    if(strcmp(interface, zwlr_output_power_manager_v1_interface.name) == 0) {
        power_manager = wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1);
        idle_power_manager_ready();
    }

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
        // FIX: was bound at version 1. river_libinput_device_v1.done is
        // "since=2" - on a version-1 object the compositor can never send
        // it, so river_libinput_device_v1_done() (which is where every
        // accel/tap/click/scroll setting actually gets applied, see
        // libinput.c) silently never ran. Binding at version 2 is the fix
        // for "mouse acceleration flat isn't working" / "touchpad tap/
        // click settings aren't taking effect".
        libinput_config = wl_registry_bind(registry, name, &river_libinput_config_v1_interface, 2);
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

void wl_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    fprintf(stderr, "warning: registry global %u removed unexpectedly\n", name);
    idle_registry_global_remove(name);
}

const struct wl_registry_listener registry_listener = {
    .global = wl_registry_global,
    .global_remove = wl_registry_global_remove,
};

int main(int argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;

    //load config file
    if(config_loder() != 0){
        return 1;
    }

    load_restart_state();

    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    bar_init();
    tabbar_init();

    struct wl_display *display = wl_display_connect(NULL);
    if(display == NULL) {
        fprintf(stderr, "failed to connect to Wayland server\n");
        return 1;
    }
    fcntl(wl_display_get_fd(display), F_SETFD, FD_CLOEXEC);

    signal(SIGCHLD, SIG_IGN);

    wl_list_init(&axe.keyboards);
    wl_list_init(&axe.windows);
    wl_list_init(&axe.outputs);
    wl_list_init(&axe.seats);
    wl_list_init(&axe.libinput_devices);
    rules_init();

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

    // run_autostart();
    bool skip_autostart = false;
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--no-autostart") == 0) {
            skip_autostart = true;
            break;
        }
    }
    if(!skip_autostart) run_autostart();

    while(true) {
        while(wl_display_prepare_read(display) != 0) {
            if(wl_display_dispatch_pending(display) < 0) {
                fprintf(stderr, "dispatch failed\n");
                return 1;
            }
        }
        bool flush_incomplete = false;
        if(wl_display_flush(display) < 0) {
            if(errno != EAGAIN) {
                fprintf(stderr, "dispatch failed\n");
                wl_display_cancel_read(display);
                return 1;
            }
            flush_incomplete = true;
        }

        int status_fd = bar_status_fd();

        struct pollfd fds[2] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN | (flush_incomplete ? POLLOUT : 0) },
            { .fd = status_fd, .events = POLLIN },
        };
        nfds_t nfds = status_fd >= 0 ? 2 : 1;

        if(poll(fds, nfds, -1) < 0) {
            fprintf(stderr, "dispatch failed\n");
            wl_display_cancel_read(display);
            if(errno == EINTR) continue;
            fprintf(stderr, "poll failed\n");
            return 1;
        }

        if(fds[0].revents & POLLOUT) wl_display_flush(display);

        if(fds[0].revents & POLLIN) {
            wl_display_read_events(display);
        } else {
            wl_display_cancel_read(display);
        }

        if(wl_display_dispatch_pending(display) < 0) {
            fprintf(stderr, "dispatch failed\n");
            return 1;
        }

        if(nfds == 2 && (fds[1].revents & (POLLIN | POLLHUP))) {
            bar_status_readable();
        }
    }

    return 0;
}
