#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include <sys/mman.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "axe.h"
#include "config.h"

typedef struct {
    struct river_xkb_keyboard_v1 *river_xkb_keyboard;

    struct wl_list link;
} Keyboard;

void river_xkb_keyboard_v1_removed(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {
    Keyboard *keyboard = data;

    river_xkb_keyboard_v1_destroy(keyboard->river_xkb_keyboard);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

void river_xkb_keyboard_v1_input_device(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1, struct river_input_device_v1 *device) {}
void river_xkb_keyboard_v1_layout(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1, uint32_t index, const char *name) {}
void river_xkb_keyboard_v1_capslock_enabled(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {}
void river_xkb_keyboard_v1_capslock_disabled(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {}
void river_xkb_keyboard_v1_numlock_enabled(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {}
void river_xkb_keyboard_v1_numlock_disabled(void *data, struct river_xkb_keyboard_v1 *river_xkb_keyboard_v1) {}

const struct river_xkb_keyboard_v1_listener xkb_keyboard_listener = {
    .removed = river_xkb_keyboard_v1_removed,
    .input_device = river_xkb_keyboard_v1_input_device,
    .layout = river_xkb_keyboard_v1_layout,
    .capslock_enabled = river_xkb_keyboard_v1_capslock_enabled,
    .capslock_disabled = river_xkb_keyboard_v1_capslock_disabled,
    .numlock_enabled = river_xkb_keyboard_v1_numlock_enabled,
    .numlock_disabled = river_xkb_keyboard_v1_numlock_disabled,
};

void river_xkb_config_v1_finished(void *data, struct river_xkb_config_v1 *river_xkb_config_v1) {
    river_xkb_config_v1_destroy(river_xkb_config_v1);
    if(xkb_config == river_xkb_config_v1) xkb_config = NULL;
}

void river_xkb_config_v1_xkb_keyboard(void *data, struct river_xkb_config_v1 *river_xkb_config_v1, struct river_xkb_keyboard_v1 *id) {
    Keyboard *keyboard = calloc(1, sizeof(Keyboard));
    keyboard->river_xkb_keyboard = id;

    wl_list_insert(&axe.keyboards, &keyboard->link);
    river_xkb_keyboard_v1_add_listener(id, &xkb_keyboard_listener, keyboard);

    if(xkb_keymap) {
        river_xkb_keyboard_v1_set_keymap(keyboard->river_xkb_keyboard, xkb_keymap);
    }

    if(numlock_default_on) {
        river_xkb_keyboard_v1_numlock_enable(keyboard->river_xkb_keyboard);
    }
}

const struct river_xkb_config_v1_listener xkb_config_listener = {
    .finished = river_xkb_config_v1_finished,
    .xkb_keyboard = river_xkb_config_v1_xkb_keyboard,
};

// credit to https://git.sr.ht/~zuki/zrwm/tree/afc021dd91bba7a69b1f10fbbf8c5d7bfd66490a/item/zrwm.c#L636
struct river_xkb_keymap_v1* create_keymap(void) {
    struct xkb_rule_names keymap_rule_names = {0};
    keymap_rule_names.layout = xkb_layout;
    keymap_rule_names.options = xkb_options;

    struct xkb_keymap *keymap = xkb_keymap_new_from_names2(xkb_context, &keymap_rule_names, XKB_KEYMAP_FORMAT_TEXT_V2, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if(keymap == NULL) {
        fprintf(stderr, "Failed to create xkb keymap\n");
        return NULL;
    }

    char *keymap_str = xkb_keymap_get_as_string2(keymap, XKB_KEYMAP_FORMAT_TEXT_V2, XKB_KEYMAP_SERIALIZE_NO_FLAGS);
    xkb_keymap_unref(keymap);
    int keymap_str_len = strlen(keymap_str) + 1;
    int keymap_fd = memfd_create("axe-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if(keymap_fd == -1 || ftruncate(keymap_fd, keymap_str_len) < 0) {
        fprintf(stderr, "Failed to create or truncate mem fd\n");
        close(keymap_fd);
        free(keymap_str);
        return NULL;
    }

    void *data = mmap(NULL, keymap_str_len, PROT_READ | PROT_WRITE, MAP_SHARED, keymap_fd, 0);
    if(data == MAP_FAILED) {
        fprintf(stderr, "Failed to map data\n");
        close(keymap_fd);
        free(keymap_str);
        return NULL;
    }

    memcpy(data, keymap_str, keymap_str_len);
    free(keymap_str);

    if(munmap(data, keymap_str_len) < 0) {
        fprintf(stderr, "Failed to unmap data\n");
        close(keymap_fd);
        // free(keymap_str);
        return NULL;
    }

    if(fcntl(keymap_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        fprintf(stderr, "Failed to seal mem fd\n");
        close(keymap_fd);
        // free(keymap_str);
        return NULL;
    }

    // return river_xkb_config_v1_create_keymap(xkb_config, keymap_fd, XKB_KEYMAP_FORMAT_TEXT_V2);
    struct river_xkb_keymap_v1 *result = river_xkb_config_v1_create_keymap(xkb_config, keymap_fd, XKB_KEYMAP_FORMAT_TEXT_V2);
    close(keymap_fd);
    return result;
}

void river_xkb_keymap_v1_success(void *data, struct river_xkb_keymap_v1 *river_xkb_keymap_v1) {
    Keyboard *keyboard;
    wl_list_for_each(keyboard, &axe.keyboards, link) {
        river_xkb_keyboard_v1_set_keymap(keyboard->river_xkb_keyboard, xkb_keymap);
        if(numlock_default_on) {
            river_xkb_keyboard_v1_numlock_enable(keyboard->river_xkb_keyboard);
        }
    }

    fprintf(stderr, "Successfully created keymap\n");
}

void river_xkb_keymap_v1_failure(void *data, struct river_xkb_keymap_v1 *river_xkb_keymap_v1, const char *error_msg) {
    fprintf(stderr, "Failed to create keymap: %s\n", error_msg ? error_msg : "(no message)");
    if(xkb_keymap == river_xkb_keymap_v1) {
        river_xkb_keymap_v1_destroy(river_xkb_keymap_v1);
        xkb_keymap = NULL;
    }
}

const struct river_xkb_keymap_v1_listener xkb_keymap_listener = {
    .success = river_xkb_keymap_v1_success,
    .failure = river_xkb_keymap_v1_failure,
};

