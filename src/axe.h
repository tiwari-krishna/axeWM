#ifndef AXEH
#define AXEH 

#include <stdbool.h>
#include <stdint.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <river-xkb-config-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-input-management-v1-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>
#include <river-libinput-config-v1-client-protocol.h>
#include <ext-idle-notify-v1-client-protocol.h>
#include <wlr-output-power-management-unstable-v1-client-protocol.h>
#include <wlr-layer-shell-unstable-v1-client-protocol.h>

#include <regex.h>

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define LENGTH(A) (sizeof A / sizeof A[0])
#define TAG_COUNT 9
#define ISVISIBLE(C) ((C) != NULL && (C)->mon != NULL && (((C)->tagmask & (C)->mon->tagmask) || (C)->sticky))
#define CLAMP(VAL, MIN, MAX) VAL = VAL < MIN ? MIN : (VAL > MAX ? MAX : VAL)

typedef struct Window Window;
typedef struct Output Output;
typedef struct Seat Seat;

typedef enum {
    LAYOUT_TILE = 0,
    LAYOUT_TABBED,
} Layout;

struct Window {
    struct river_window_v1 *river_window;
    struct river_node_v1 *river_node;
    struct wl_list link;
    char identifier[33];
    char app_id[128];
    char title[256];

    int x;
    int y;

    int width;
    int height;

    bool focused;
    bool hovered;

    bool floating;
    bool fullscreen;
    bool sticky;

    bool floating_explicit;

    int floatx;
    int floaty;
    int floatw;
    int floath;

    Output *pre_fullscreen_mon;

    Output *mon;
    uint32_t tagmask;
    bool got_real_dimensions;
    bool got_position;
};

struct Output {
    struct river_output_v1 *river_output;
    struct river_layer_shell_output_v1 *river_layer_shell_output;
    struct wl_list link;

    int x;
    int y;

    int width;
    int height;

    // Usable area left after subtracting layer-shell exclusive zones (bars,
    // panels, etc.), in global coordinates. Defaults to the full output
    // area until a non_exclusive_area event says otherwise.
    int nex_x;
    int nex_y;
    int nex_w;
    int nex_h;

    int nmaster[TAG_COUNT];
    float mfact[TAG_COUNT];

    uint32_t seltag;
    uint32_t tagmask;

    Window *last_focused[TAG_COUNT];

    int tiled_count;

    // For idle.c's display-off timeout only - unrelated to river's own
    // river_output_v1, obtained via the river_output_v1.wl_output event.
    struct wl_output *wl_output;
    struct zwlr_output_power_v1 *power;

    // For bar.c only.
    struct wl_surface *bar_surface;
    struct zwlr_layer_surface_v1 *bar_layer_surface;
    struct wl_buffer *bar_buffer;
    int bar_configured_w, bar_configured_h;
    int bar_buf_w;
    int bar_buf_h;
    uint32_t bar_last_occupied; // cached, to skip redundant redraws
    uint32_t bar_last_seltag;
    uint32_t bar_last_status_epoch;
    bool bar_last_passthrough; // cached, so entering/leaving passthrough always redraws

    // Tiling layout, per-tag like nmaster/mfact above. LAYOUT_TILE
    // (default, and what a fresh calloc'd Output gets - LAYOUT_TILE == 0)
    // is the usual master/stack. LAYOUT_TABBED gives every tiled window
    // the same full-size slot (no gaps/borders) and shows an i3-style tab
    // strip instead - see tabbar.c. Toggled per-tag with togglelayout.
    Layout layout[TAG_COUNT];

    // tabbar.c's own layer-shell surface - separate from bar_surface
    // above, only reserved/shown while the current tag's layout is
    // LAYOUT_TABBED and there's more than one tiled window to tab
    // between.
    struct wl_surface *tab_surface;
    struct zwlr_layer_surface_v1 *tab_layer_surface;
    struct wl_buffer *tab_buffer;
    int tab_configured_w, tab_configured_h;
    int tab_buf_w, tab_buf_h;
    bool tab_last_layout;
    uint32_t tab_last_hash; // cheap signature of what's drawn, to skip redundant redraws
};

// Index into an Output's per-tag nmaster/mfact/layout arrays for its
// currently selected tag. o->seltag is expected to be a single bit (see
// toggleview's "leftmost viewed tag" logic in actions.c) - defensively
// falls back to tag 0 if it's ever 0 or out of TAG_COUNT.
static inline int tagidx(Output *o) {
    if(o == NULL || o->seltag == 0) return 0;
    int i = __builtin_ctz(o->seltag);
    return (i >= 0 && i < TAG_COUNT) ? i : 0;
}

struct Seat {
    struct river_seat_v1 *river_seat;
    struct river_layer_shell_seat_v1 *river_layer_shell_seat;
    struct wl_list link;

    Window *focused;
    Window *hovered;

    struct wl_list keys;
    struct wl_list buttons;

    // State for an in-progress interactive move/resize (op_start_pointer).
    Window *op_window;
    int op_mode; // 0 = move, 1 = resize
    int op_orig_x, op_orig_y, op_orig_w, op_orig_h;
    bool op_ending;

    int op_last_x, op_last_y, op_last_w, op_last_h;

    bool op_move_x, op_move_y;
    int pointer_x, pointer_y;
    bool pending_warp;

    // For idle.c only - obtained via the river_seat_v1.wl_seat event.
    struct wl_seat *wl_seat;
    bool idle_setup_done;
    struct wl_list idle_watchers;
    struct ext_idle_notification_v1 *display_notification;
};

typedef struct {
    struct wl_list windows;
    struct wl_list outputs;
    struct wl_list seats;
    struct wl_list keyboards;
    struct wl_list libinput_devices;
} WindowManager;

typedef union {
    int i;
    void *v;
    float f;
    uint32_t u;
} Arg;

typedef struct {
    struct river_xkb_binding_v1 *river_xkb_binding;
    struct wl_list link;

    Seat *seat;

    void (*func)(Seat *seat, Arg *arg);
    void (*release_func)(Seat *seat, Arg *arg);
    Arg *arg;
} Key;

typedef struct {
    struct river_pointer_binding_v1 *river_pointer_binding;
    struct wl_list link;

    Seat *seat;

    bool pressed;

    void (*func)(Seat *seat, Arg *arg);
    Arg *arg;
} Button;

typedef struct {
    uint32_t mods;
    xkb_keysym_t key;
    void (*func)(Seat *seat, Arg *arg);
    Arg arg;
} Keys;

typedef struct {
    uint32_t mods;
    uint32_t button;
    void (*func)(Seat *seat, Arg *arg);
    Arg arg;
} Mousebinds;

// A window rule matched by app_id (exact) and/or title (POSIX extended
// regex, case-insensitive) - either may be NULL to mean "don't care".
// floating is tri-state: -1 = unchanged, 0 = force tiled, 1 = force
// floating. tag/monitor: -1 = unchanged. float_width/float_height, if
// > 0, are fractions (0.0-1.0] of the monitor's usable area used as the
// window's default floating size the first time it floats.
typedef struct {
    const char *app_id;
    const char *title;
    int floating;
    int tag;
    int monitor;
    float float_width;
    float float_height;
} Rule;

// A single idle-timeout entry: run `command` once the seat has been
// inactive for `timeout_ms`, run `resume_command` (may be NULL) when
// activity resumes.
typedef struct {
    int timeout_ms;
    const char *command;
    const char *resume_command;
} IdleTimeout;

// ---------------------------------------------------------------------
// Global state (defined once in main.c)
// ---------------------------------------------------------------------
extern WindowManager axe;
extern Output *selmon;
extern bool passthrough;

extern struct xkb_context *xkb_context;
extern struct river_xkb_config_v1 *xkb_config;
extern struct river_xkb_keymap_v1 *xkb_keymap;
extern struct river_xkb_bindings_v1 *xkb_bindings;
extern struct river_input_manager_v1 *input_manager;
extern struct river_window_manager_v1 *window_manager;
extern struct river_layer_shell_v1 *layer_shell;
extern struct river_libinput_config_v1 *libinput_config;
extern struct ext_idle_notifier_v1 *idle_notifier;
extern struct zwlr_output_power_manager_v1 *power_manager;
extern struct wl_compositor *compositor;
extern struct wl_shm *shm;
extern struct zwlr_layer_shell_v1 *wlr_layer_shell;

// ---------------------------------------------------------------------
// Listener tables (each defined in the .c file that owns that interface)
// ---------------------------------------------------------------------
extern const struct river_output_v1_listener output_listener;
extern const struct river_layer_shell_output_v1_listener layer_shell_output_listener;
extern const struct river_window_v1_listener window_listener;
extern const struct river_xkb_binding_v1_listener xkb_binding_listener;
extern const struct river_pointer_binding_v1_listener pointer_binding_listener;
extern const struct river_seat_v1_listener seat_listener;
extern const struct river_layer_shell_seat_v1_listener layer_shell_seat_listener;
extern const struct river_window_manager_v1_listener window_manager_listener;
extern const struct river_input_manager_v1_listener input_manager_listener;
extern const struct river_input_device_v1_listener input_device_listener;
extern const struct river_libinput_result_v1_listener libinput_result_listener;
extern const struct river_libinput_device_v1_listener libinput_device_listener;
extern const struct river_libinput_config_v1_listener libinput_config_listener;
extern const struct river_xkb_keyboard_v1_listener xkb_keyboard_listener;
extern const struct river_xkb_config_v1_listener xkb_config_listener;
extern const struct river_xkb_keymap_v1_listener xkb_keymap_listener;
extern const struct wl_registry_listener registry_listener;

// ---------------------------------------------------------------------
// actions.c - keybind/mousebind actions + small shared tiling helpers
// ---------------------------------------------------------------------
void list_swap(struct wl_list *x, struct wl_list *y);
Window *adjacent_tiled(Window *w, int dir);
Window *adjacent_visible(Window *w, int dir);
void set_focus(Seat *seat, Window *window);
void float_default_geometry(Window *w);
void clamp_float_geometry(Window *w, Output *mon);
void run_autostart(void);

void destroy_window(Seat *seat, Arg *arg);
void togglebar(Seat *seat, Arg *arg);
void select_next_mon(Seat *seat, Arg *arg);
void select_prev_mon(Seat *seat, Arg *arg);
void movemon(Seat *seat, Arg *arg);
void focus_next(Seat *seat, Arg *arg);
void focus_prev(Seat *seat, Arg *arg);
void incnmaster(Seat *seat, Arg *arg);
void setmfact(Seat *seat, Arg *arg);
void exit_session(Seat *seat, Arg *arg);
void spawn(Seat *seat, Arg *arg);
void view(Seat *seat, Arg *arg);
void toggleview(Seat *seat, Arg *arg);
void tag(Seat *seat, Arg *arg);
void toggletag(Seat *seat, Arg *arg);

void movestack(Seat *seat, Arg *arg);
void zoom(Seat *seat, Arg *arg);
void togglefloating(Seat *seat, Arg *arg);
void togglefullscreen(Seat *seat, Arg *arg);

void movewin(Seat *seat, Arg *arg);
void resizewin(Seat *seat, Arg *arg);
void togglesticky(Seat *seat, Arg *arg);
void togglelayout(Seat *seat, Arg *arg);

// ---------------------------------------------------------------------
// restart.c - self-exec restart with saved window/output state
// ---------------------------------------------------------------------
extern int saved_argc;
extern char **saved_argv;

void load_restart_state(void);
void apply_saved_output_state(Output *output);
void apply_saved_window_state(Window *window);
void restart_axe(Seat *seat, Arg *arg);

// ---------------------------------------------------------------------
// window.c - river_window_v1 handling
// ---------------------------------------------------------------------
Output *output_by_index(int idx);
void rules_init(void);
void apply_rules(Window *window);
void window_set_position(Window *window, int x, int y);
void window_set_dimensions(Window *window, int width, int height);
void render_window(Window *window);

// Exported so tabbar.c can draw tab labels with the exact same font/
// rendering path as the status bar, without loading its own fonts.
void fill_rect(uint8_t *buf, int w, int h, int x0, int y0, int x1, int y1, const uint8_t color[4]);
int measure_text_width(const char *s);
void draw_text(uint8_t *buf, int w, int h, int x, const char *s, const uint8_t color[4]);

// ---------------------------------------------------------------------
// output.c - river_output_v1 / layer-shell output handling
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// seat.c - river_seat_v1 / layer-shell seat handling
// ---------------------------------------------------------------------
void manage_seat(Seat *seat);
void render_seat(Seat *seat);

// ---------------------------------------------------------------------
// bindings.c - xkb key bindings + pointer button bindings
// ---------------------------------------------------------------------
void xkb_binding_create(Seat *seat, uint32_t modifiers, xkb_keysym_t keysym, void (*func)(Seat *seat, Arg *arg), Arg *arg);
void xkb_hold_binding_create(Seat *seat, uint32_t modifiers, xkb_keysym_t keysym,
                             void (*press_func)(Seat *seat, Arg *arg),
                             void (*release_func)(Seat *seat, Arg *arg), Arg *arg);
void xkb_binding_destroy(Key *key);
void pointer_binding_create(Seat *seat, uint32_t modifiers, uint32_t ibutton, void (*func)(Seat *seat, Arg *arg), Arg *arg);
void pointer_binding_destroy(Button *button);

// ---------------------------------------------------------------------
// wm.c - river_window_manager_v1 handling (the tiling engine loop)
// ---------------------------------------------------------------------
int count_tiled_windows(Output *output);

// ---------------------------------------------------------------------
// keyboard.c - xkb keymap/config handling (untouched logic, just moved)
// ---------------------------------------------------------------------
struct river_xkb_keymap_v1 *create_keymap(void);

// ---------------------------------------------------------------------
// idle.c - idle-timeout commands + display power off
// ---------------------------------------------------------------------
void idle_track_wl_seat(struct wl_registry *registry, uint32_t name);
void idle_track_wl_output(struct wl_registry *registry, uint32_t name);
void idle_teardown_seat(Seat *seat);
void idle_attach_wl_seat(Seat *seat, uint32_t name);
void idle_attach_wl_output(Output *output, uint32_t name);
void idle_notifier_ready(void);
void idle_power_manager_ready(void);
void idle_registry_global_remove(uint32_t name);

// ---------------------------------------------------------------------
// bar.c - status bar (tags + one-shot status command) via wlr-layer-shell
// ---------------------------------------------------------------------
void bar_init(void);
void bar_output_ready(Output *output);
void bar_manager_ready(void);
void bar_redraw_all(void);
void bar_destroy(Output *output);
void bar_set_visible(bool visible);
void bar_toggle(void);
void bar_setup_seat_autohide(Seat *seat);
int bar_status_fd(void);
void bar_status_readable(void);
void bar_kill_status(void);

// ---------------------------------------------------------------------
// tabbar.c - i3-style tab strip for LAYOUT_TABBED outputs
// ---------------------------------------------------------------------
void tabbar_manager_ready(void);
void tabbar_output_ready(Output *output);
void tabbar_redraw_all(void);
void tabbar_destroy(Output *output);
void tabbar_init(void);
void tabbar_set_visible(bool visible);
void tabselect(Seat *seat, Arg *arg);
void togglepassthrough(Seat *seat, Arg *arg);

#endif /* AXEH */
