#include <stdio.h>
#include <stdlib.h>

#include "axe.h"
#include "config.h"

// --- libinput device configuration -----------------------------------
// We deliberately don't track the *_support/*_default/*_current state for
// every property - we just fire off the settings we want once the
// device's initial info batch is complete (the done event) and let
// unsupported settings get silently ignored by the result object below.
//
// NOTE: `done` is only sent on objects created through a
// river_libinput_config_v1 bound at version >= 2 (see river-libinput-
// config-v1.xml). main.c binds it at version 2, which is what makes all
// of the settings below actually take effect.

void river_libinput_result_v1_success(void *data, struct river_libinput_result_v1 *obj) {
    wl_proxy_destroy((struct wl_proxy *) obj);
}
void river_libinput_result_v1_unsupported(void *data, struct river_libinput_result_v1 *obj) {
    wl_proxy_destroy((struct wl_proxy *) obj);
}
void river_libinput_result_v1_invalid(void *data, struct river_libinput_result_v1 *obj) {
    fprintf(stderr, "libinput config request was invalid\n");
    wl_proxy_destroy((struct wl_proxy *) obj);
}

const struct river_libinput_result_v1_listener libinput_result_listener = {
    .success = river_libinput_result_v1_success,
    .unsupported = river_libinput_result_v1_unsupported,
    .invalid = river_libinput_result_v1_invalid,
};

#define LIBINPUT_APPLY(RESULT_EXPR) \
river_libinput_result_v1_add_listener((RESULT_EXPR), &libinput_result_listener, NULL)

typedef struct {
    struct river_libinput_device_v1 *dev;
    bool configured;
    int tap_finger_count;
    struct wl_list link;
} LibinputDevice;

void river_libinput_device_v1_send_events_support(void *data, struct river_libinput_device_v1 *obj, uint32_t modes) {}
void river_libinput_device_v1_send_events_default(void *data, struct river_libinput_device_v1 *obj, uint32_t mode) {}
void river_libinput_device_v1_send_events_current(void *data, struct river_libinput_device_v1 *obj, uint32_t mode) {}
void river_libinput_device_v1_tap_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_tap_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_tap_button_map_default(void *data, struct river_libinput_device_v1 *obj, uint32_t button_map) {}
void river_libinput_device_v1_tap_button_map_current(void *data, struct river_libinput_device_v1 *obj, uint32_t button_map) {}
void river_libinput_device_v1_drag_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_drag_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_drag_lock_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_drag_lock_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_three_finger_drag_support(void *data, struct river_libinput_device_v1 *obj, int32_t finger_count) {}
void river_libinput_device_v1_three_finger_drag_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_three_finger_drag_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_calibration_matrix_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_calibration_matrix_default(void *data, struct river_libinput_device_v1 *obj, struct wl_array *matrix) {}
void river_libinput_device_v1_calibration_matrix_current(void *data, struct river_libinput_device_v1 *obj, struct wl_array *matrix) {}
void river_libinput_device_v1_accel_profiles_support(void *data, struct river_libinput_device_v1 *obj, uint32_t profiles) {}
void river_libinput_device_v1_accel_profile_default(void *data, struct river_libinput_device_v1 *obj, uint32_t profile) {}
void river_libinput_device_v1_accel_profile_current(void *data, struct river_libinput_device_v1 *obj, uint32_t profile) {}
void river_libinput_device_v1_accel_speed_default(void *data, struct river_libinput_device_v1 *obj, struct wl_array *speed) {}
void river_libinput_device_v1_accel_speed_current(void *data, struct river_libinput_device_v1 *obj, struct wl_array *speed) {}
void river_libinput_device_v1_natural_scroll_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_natural_scroll_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_natural_scroll_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_left_handed_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_left_handed_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_left_handed_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_click_method_support(void *data, struct river_libinput_device_v1 *obj, uint32_t methods) {}
void river_libinput_device_v1_click_method_default(void *data, struct river_libinput_device_v1 *obj, uint32_t method) {}
void river_libinput_device_v1_click_method_current(void *data, struct river_libinput_device_v1 *obj, uint32_t method) {}
void river_libinput_device_v1_clickfinger_button_map_default(void *data, struct river_libinput_device_v1 *obj, uint32_t button_map) {}
void river_libinput_device_v1_clickfinger_button_map_current(void *data, struct river_libinput_device_v1 *obj, uint32_t button_map) {}
void river_libinput_device_v1_middle_emulation_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_middle_emulation_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_middle_emulation_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_scroll_method_support(void *data, struct river_libinput_device_v1 *obj, uint32_t methods) {}
void river_libinput_device_v1_scroll_method_default(void *data, struct river_libinput_device_v1 *obj, uint32_t method) {}
void river_libinput_device_v1_scroll_method_current(void *data, struct river_libinput_device_v1 *obj, uint32_t method) {}
void river_libinput_device_v1_scroll_button_default(void *data, struct river_libinput_device_v1 *obj, uint32_t button) {}
void river_libinput_device_v1_scroll_button_current(void *data, struct river_libinput_device_v1 *obj, uint32_t button) {}
void river_libinput_device_v1_scroll_button_lock_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_scroll_button_lock_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_dwt_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_dwt_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_dwt_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_dwtp_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_dwtp_default(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_dwtp_current(void *data, struct river_libinput_device_v1 *obj, uint32_t state) {}
void river_libinput_device_v1_rotation_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {}
void river_libinput_device_v1_rotation_default(void *data, struct river_libinput_device_v1 *obj, uint32_t angle) {}
void river_libinput_device_v1_rotation_current(void *data, struct river_libinput_device_v1 *obj, uint32_t angle) {}

void river_libinput_device_v1_removed(void *data, struct river_libinput_device_v1 *obj) {
    LibinputDevice *dev = data;
    river_libinput_device_v1_destroy(dev->dev);
    wl_list_remove(&dev->link);
    free(dev);
}

void river_libinput_device_v1_input_device(void *data, struct river_libinput_device_v1 *obj, struct river_input_device_v1 *device) {}

void river_libinput_device_v1_tap_support(void *data, struct river_libinput_device_v1 *obj, int32_t finger_count) {
    ((LibinputDevice *) data)->tap_finger_count = finger_count;
}

// Encode a C double as the native-endian 8-byte wl_array this protocol
// uses in place of a native floating point Wayland argument type.
struct wl_array double_arg(double v) {
    struct wl_array arr;
    wl_array_init(&arr);
    double *p = wl_array_add(&arr, sizeof(double));
    *p = v;
    return arr;
}

void river_libinput_device_v1_done(void *data, struct river_libinput_device_v1 *obj) {
    LibinputDevice *dev = data;

    if(dev->configured) return;
    dev->configured = true;

    // Applied to every pointer-capable device (mice and touchpads alike),
    // matching a sway `input * { accel_profile ...; pointer_accel ...; }`
    // block.
    LIBINPUT_APPLY(river_libinput_device_v1_set_accel_profile(dev->dev, pointer_accel_profile));

    struct wl_array speed = double_arg(pointer_accel_speed);
    LIBINPUT_APPLY(river_libinput_device_v1_set_accel_speed(dev->dev, &speed));
    wl_array_release(&speed);

    // libinput has no explicit "is this a touchpad" flag; reporting any
    // tap-to-click finger support is the standard heuristic for one, and
    // matches sway's `input type:touchpad { ... }` block.
    if(dev->tap_finger_count > 0) {
        LIBINPUT_APPLY(river_libinput_device_v1_set_tap(dev->dev, touchpad_tap ? RIVER_LIBINPUT_DEVICE_V1_TAP_STATE_ENABLED : RIVER_LIBINPUT_DEVICE_V1_TAP_STATE_DISABLED));
        LIBINPUT_APPLY(river_libinput_device_v1_set_natural_scroll(dev->dev, touchpad_natural_scroll ? RIVER_LIBINPUT_DEVICE_V1_NATURAL_SCROLL_STATE_ENABLED : RIVER_LIBINPUT_DEVICE_V1_NATURAL_SCROLL_STATE_DISABLED));
        LIBINPUT_APPLY(river_libinput_device_v1_set_dwt(dev->dev, touchpad_dwt ? RIVER_LIBINPUT_DEVICE_V1_DWT_STATE_ENABLED : RIVER_LIBINPUT_DEVICE_V1_DWT_STATE_DISABLED));
        LIBINPUT_APPLY(river_libinput_device_v1_set_click_method(dev->dev, touchpad_click_method));
        LIBINPUT_APPLY(river_libinput_device_v1_set_middle_emulation(dev->dev, touchpad_middle_emulation ? RIVER_LIBINPUT_DEVICE_V1_MIDDLE_EMULATION_STATE_ENABLED : RIVER_LIBINPUT_DEVICE_V1_MIDDLE_EMULATION_STATE_DISABLED));
        LIBINPUT_APPLY(river_libinput_device_v1_set_scroll_method(dev->dev, touchpad_scroll_method));
    }
}

const struct river_libinput_device_v1_listener libinput_device_listener = {
    .removed = river_libinput_device_v1_removed,
    .input_device = river_libinput_device_v1_input_device,
    .send_events_support = river_libinput_device_v1_send_events_support,
    .send_events_default = river_libinput_device_v1_send_events_default,
    .send_events_current = river_libinput_device_v1_send_events_current,
    .tap_support = river_libinput_device_v1_tap_support,
    .tap_default = river_libinput_device_v1_tap_default,
    .tap_current = river_libinput_device_v1_tap_current,
    .tap_button_map_default = river_libinput_device_v1_tap_button_map_default,
    .tap_button_map_current = river_libinput_device_v1_tap_button_map_current,
    .drag_default = river_libinput_device_v1_drag_default,
    .drag_current = river_libinput_device_v1_drag_current,
    .drag_lock_default = river_libinput_device_v1_drag_lock_default,
    .drag_lock_current = river_libinput_device_v1_drag_lock_current,
    .three_finger_drag_support = river_libinput_device_v1_three_finger_drag_support,
    .three_finger_drag_default = river_libinput_device_v1_three_finger_drag_default,
    .three_finger_drag_current = river_libinput_device_v1_three_finger_drag_current,
    .calibration_matrix_support = river_libinput_device_v1_calibration_matrix_support,
    .calibration_matrix_default = river_libinput_device_v1_calibration_matrix_default,
    .calibration_matrix_current = river_libinput_device_v1_calibration_matrix_current,
    .accel_profiles_support = river_libinput_device_v1_accel_profiles_support,
    .accel_profile_default = river_libinput_device_v1_accel_profile_default,
    .accel_profile_current = river_libinput_device_v1_accel_profile_current,
    .accel_speed_default = river_libinput_device_v1_accel_speed_default,
    .accel_speed_current = river_libinput_device_v1_accel_speed_current,
    .natural_scroll_support = river_libinput_device_v1_natural_scroll_support,
    .natural_scroll_default = river_libinput_device_v1_natural_scroll_default,
    .natural_scroll_current = river_libinput_device_v1_natural_scroll_current,
    .left_handed_support = river_libinput_device_v1_left_handed_support,
    .left_handed_default = river_libinput_device_v1_left_handed_default,
    .left_handed_current = river_libinput_device_v1_left_handed_current,
    .click_method_support = river_libinput_device_v1_click_method_support,
    .click_method_default = river_libinput_device_v1_click_method_default,
    .click_method_current = river_libinput_device_v1_click_method_current,
    .clickfinger_button_map_default = river_libinput_device_v1_clickfinger_button_map_default,
    .clickfinger_button_map_current = river_libinput_device_v1_clickfinger_button_map_current,
    .middle_emulation_support = river_libinput_device_v1_middle_emulation_support,
    .middle_emulation_default = river_libinput_device_v1_middle_emulation_default,
    .middle_emulation_current = river_libinput_device_v1_middle_emulation_current,
    .scroll_method_support = river_libinput_device_v1_scroll_method_support,
    .scroll_method_default = river_libinput_device_v1_scroll_method_default,
    .scroll_method_current = river_libinput_device_v1_scroll_method_current,
    .scroll_button_default = river_libinput_device_v1_scroll_button_default,
    .scroll_button_current = river_libinput_device_v1_scroll_button_current,
    .scroll_button_lock_default = river_libinput_device_v1_scroll_button_lock_default,
    .scroll_button_lock_current = river_libinput_device_v1_scroll_button_lock_current,
    .dwt_support = river_libinput_device_v1_dwt_support,
    .dwt_default = river_libinput_device_v1_dwt_default,
    .dwt_current = river_libinput_device_v1_dwt_current,
    .dwtp_support = river_libinput_device_v1_dwtp_support,
    .dwtp_default = river_libinput_device_v1_dwtp_default,
    .dwtp_current = river_libinput_device_v1_dwtp_current,
    .rotation_support = river_libinput_device_v1_rotation_support,
    .rotation_default = river_libinput_device_v1_rotation_default,
    .rotation_current = river_libinput_device_v1_rotation_current,
    .done = river_libinput_device_v1_done,
};

void river_libinput_config_v1_finished(void *data, struct river_libinput_config_v1 *obj) {
    river_libinput_config_v1_destroy(obj);
}

void river_libinput_config_v1_libinput_device(void *data, struct river_libinput_config_v1 *obj, struct river_libinput_device_v1 *id) {
    LibinputDevice *dev = calloc(1, sizeof(LibinputDevice));
    dev->dev = id;
    wl_list_insert(&axe.libinput_devices, &dev->link);
    river_libinput_device_v1_add_listener(id, &libinput_device_listener, dev);
}

const struct river_libinput_config_v1_listener libinput_config_listener = {
    .finished = river_libinput_config_v1_finished,
    .libinput_device = river_libinput_config_v1_libinput_device,
};
