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

#include "axe.h"

WindowManager axe;
Output *selmon = NULL;

struct xkb_context *xkb_context;
struct river_xkb_config_v1 *xkb_config;
struct river_xkb_keymap_v1 *xkb_keymap;
struct river_xkb_bindings_v1 *xkb_bindings;
struct river_input_manager_v1 *input_manager;
struct river_window_manager_v1 *window_manager;
struct river_layer_shell_v1 *layer_shell;
struct river_libinput_config_v1 *libinput_config;

void wl_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
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

void wl_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

const struct wl_registry_listener registry_listener = {
    .global = wl_registry_global,
    .global_remove = wl_registry_global_remove,
};

int main(int argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;
    load_restart_state();
    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

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
        if(wl_display_dispatch(display) < 0) {
            fprintf(stderr, "dispatch failed\n");
            return 1;
        }
    }

    return 0;
}
