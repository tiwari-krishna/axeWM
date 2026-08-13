#include "axe.h"
#include "config.h"

void river_input_manager_v1_finished(void *data, struct river_input_manager_v1 *river_input_manager_v1) {
    river_input_manager_v1_destroy(river_input_manager_v1);
    if(input_manager == river_input_manager_v1) input_manager = NULL;
}

void river_input_manager_v1_input_device(void *data, struct river_input_manager_v1 *river_input_manager_v1, struct river_input_device_v1 *id) {
    river_input_device_v1_add_listener(id, &input_device_listener, NULL);

    // set_repeat_info is documented as a no-op for non-keyboard devices, so
    // there's no need to wait for the type event first.
    river_input_device_v1_set_repeat_info(id, repeat_rate, repeat_delay);
}

const struct river_input_manager_v1_listener input_manager_listener = {
    .finished = river_input_manager_v1_finished,
    .input_device = river_input_manager_v1_input_device,
};

void river_input_device_v1_removed(void *data, struct river_input_device_v1 *river_input_device_v1) {
    river_input_device_v1_destroy(river_input_device_v1);
}
void river_input_device_v1_type(void *data, struct river_input_device_v1 *river_input_device_v1, uint32_t type) {}
void river_input_device_v1_name(void *data, struct river_input_device_v1 *river_input_device_v1, const char *name) {}

const struct river_input_device_v1_listener input_device_listener = {
    .removed = river_input_device_v1_removed,
    .type = river_input_device_v1_type,
    .name = river_input_device_v1_name,
};
