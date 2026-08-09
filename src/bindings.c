#include <stdlib.h>

#include "axe.h"

void river_xkb_binding_v1_pressed(void *data, struct river_xkb_binding_v1 *obj) {
    Key *key = data;
    key->func(key->seat, key->arg);
}

void river_xkb_binding_v1_released(void *data, struct river_xkb_binding_v1 *obj) {}

const struct river_xkb_binding_v1_listener xkb_binding_listener = {
    .pressed = river_xkb_binding_v1_pressed,
    .released = river_xkb_binding_v1_released,
};

void xkb_binding_create(Seat *seat, uint32_t modifiers, xkb_keysym_t keysym, void (*func)(Seat *seat, Arg *arg), Arg *arg) {
    Key *key = calloc(1, sizeof(Key));
    key->river_xkb_binding = river_xkb_bindings_v1_get_xkb_binding(xkb_bindings, seat->river_seat, keysym, modifiers);
    key->seat = seat;
    key->func = func;
    key->arg = arg;

    river_xkb_binding_v1_add_listener(key->river_xkb_binding, &xkb_binding_listener, key);
    river_xkb_binding_v1_enable(key->river_xkb_binding);

    wl_list_insert(&seat->keys, &key->link);
}

void xkb_binding_destroy(Key *key) {
    river_xkb_binding_v1_destroy(key->river_xkb_binding);
    wl_list_remove(&key->link);
    free(key);
}

void river_pointer_binding_v1_pressed(void *data, struct river_pointer_binding_v1 *obj) {
    Button *button = data;
    button->pressed = true;
    button->func(button->seat, button->arg);
}

void river_pointer_binding_v1_released(void *data, struct river_pointer_binding_v1 *obj) {
    ((Button*) data)->pressed = false;
}

const struct river_pointer_binding_v1_listener pointer_binding_listener = {
    .pressed = river_pointer_binding_v1_pressed,
    .released = river_pointer_binding_v1_released,
};

void pointer_binding_create(Seat *seat, uint32_t modifiers, uint32_t ibutton, void (*func)(Seat *seat, Arg *arg), Arg *arg) {
    Button *button = calloc(1, sizeof(Button));
    button->river_pointer_binding = river_seat_v1_get_pointer_binding(seat->river_seat, ibutton, modifiers);
    button->seat = seat;
    button->func = func;
    button->arg = arg;

    river_pointer_binding_v1_add_listener(button->river_pointer_binding, &pointer_binding_listener, button);
    river_pointer_binding_v1_enable(button->river_pointer_binding);

    wl_list_insert(&seat->buttons, &button->link);
}

void pointer_binding_destroy(Button *button) {
    river_pointer_binding_v1_destroy(button->river_pointer_binding);
    wl_list_remove(&button->link);
    free(button);
}
