#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "axe.h"

int saved_argc;
char **saved_argv;

// XDG_RUNTIME_DIR is per-user, tmpfs-backed on basically every system;
// /tmp is the fallback for anything unusual.
static const char *state_path(void) {
    static char path[256];
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if(dir == NULL) dir = "/tmp";
    // snprintf(path, sizeof(path), "%s/axe-restart-state", dir);
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    snprintf(path, sizeof(path), "%s/axe-restart-state-%s", dir, wayland_display ? wayland_display : "default");
    return path;
}

typedef struct {
    char identifier[33];
    uint32_t tagmask;
    bool floating;
    int floatx, floaty, floatw, floath;
    int mon_index;
    struct wl_list link;
} SavedWindow;

typedef struct {
    int index;
    int nmaster[TAG_COUNT];
    float mfact[TAG_COUNT];
    int layout[TAG_COUNT];
    uint32_t seltag;
    uint32_t tagmask;
    struct wl_list link;
} SavedOutput;

static struct wl_list saved_windows;
static struct wl_list saved_outputs;
static bool state_loaded = false;

// axe.outputs is built by wl_list_insert() prepending each output as it's
// announced, so "index" here just means "position in the list right now" -
// not arrival order. That's fine: both save and restore walk the same
// list the same way, so positions line up across a restart as long as
// nothing physically changed (no monitor hotplug mid-restart).
static int output_index_of(Output *target) {
    int i = 0;
    Output *o;
    wl_list_for_each(o, &axe.outputs, link) {
        if(o == target) return i;
        i++;
    }
    return -1;
}

void load_restart_state(void) {
    wl_list_init(&saved_windows);
    wl_list_init(&saved_outputs);
    state_loaded = true;

    FILE *f = fopen(state_path(), "r");
    if(f == NULL) return; // no previous state - normal first launch

    char kind[2];
    while(fscanf(f, " %1s", kind) == 1) {
        if(kind[0] == 'O') {
            SavedOutput *o = calloc(1, sizeof(SavedOutput));
            // FIX: the "O" line grew from 5 scalar fields to 3 scalars
            // plus three TAG_COUNT-sized arrays (per-tag nmaster/mfact/
            // layout). A state file written by an older binary won't
            // match this shape - `ok` catches that cleanly and we just
            // discard the (partially read) entry and stop, same as any
            // other malformed line below. That means the very first
            // restart after upgrading to per-tag layout starts fresh
            // instead of migrating old state - expected and harmless,
            // not a crash or corruption risk.
            bool ok = fscanf(f, "%d %u %u", &o->index, &o->seltag, &o->tagmask) == 3;
            for(int t = 0; ok && t < TAG_COUNT; t++) ok = fscanf(f, "%d", &o->nmaster[t]) == 1;
            for(int t = 0; ok && t < TAG_COUNT; t++) ok = fscanf(f, "%f", &o->mfact[t]) == 1;
            for(int t = 0; ok && t < TAG_COUNT; t++) ok = fscanf(f, "%d", &o->layout[t]) == 1;
            if(!ok) {
                free(o);
                break;
            }
            wl_list_insert(&saved_outputs, &o->link);
        } else if(kind[0] == 'W') {
            SavedWindow *w = calloc(1, sizeof(SavedWindow));
            int floating_int;
            if(fscanf(f, "%32s %u %d %d %d %d %d %d",
                      w->identifier, &w->tagmask, &floating_int,
                      &w->floatx, &w->floaty, &w->floatw, &w->floath, &w->mon_index) != 8) {
                free(w);
                break;
            }
            w->floating = floating_int != 0;
            wl_list_insert(&saved_windows, &w->link);
        } else {
            break; // malformed - stop rather than loop forever on garbage
        }
    }

    fclose(f);
    // Not unlinked: next restart just overwrites it. If axe ever crashes
    // outside the restart keybind, the next manual launch will pick up
    // whatever state was saved last time - stale, but better than nothing.
}

void apply_saved_output_state(Output *output) {
    if(!state_loaded) return;

    int index = output_index_of(output);
    SavedOutput *o;
    wl_list_for_each(o, &saved_outputs, link) {
        if(o->index != index) continue;
        for(int t = 0; t < TAG_COUNT; t++) {
            output->nmaster[t] = o->nmaster[t];
            CLAMP(output->nmaster[t], 0, (1 << 16));
            output->mfact[t] = o->mfact[t];
            CLAMP(output->mfact[t], 0, 1);
            output->layout[t] = (o->layout[t] == LAYOUT_TABBED) ? LAYOUT_TABBED : LAYOUT_TILE;
        }
        output->seltag = o->seltag;
        output->tagmask = o->tagmask;
        wl_list_remove(&o->link);
        free(o);
        return;
    }
}

void apply_saved_window_state(Window *window) {
    if(!state_loaded) return;

    SavedWindow *w;
    wl_list_for_each(w, &saved_windows, link) {
        if(strcmp(w->identifier, window->identifier) != 0) continue;

        window->tagmask = w->tagmask;
        window->floating = w->floating;
        window->floatx = w->floatx;
        window->floaty = w->floaty;
        window->floatw = w->floatw;
        window->floath = w->floath;
        window->got_real_dimensions = true;

        Output *mon = output_by_index(w->mon_index);
        if(mon != NULL) window->mon = mon;

        clamp_float_geometry(window, window->mon);

        // Consumed - drop it rather than holding it for the rest of the
        // process's life (and so a duplicate identifier can't reapply it
        // to a second window later).
        wl_list_remove(&w->link);
        free(w);
        return;
    }
}

void restart_axe(Seat *seat, Arg *arg) {
    FILE *f = fopen(state_path(), "w");
    if(f == NULL) {
        fprintf(stderr, "restart: failed to open %s for writing, restarting without saved state\n", state_path());
    } else {
        Output *output;
        wl_list_for_each(output, &axe.outputs, link) {
            fprintf(f, "O %d %u %u", output_index_of(output), output->seltag, output->tagmask);
            for(int t = 0; t < TAG_COUNT; t++) fprintf(f, " %d", output->nmaster[t]);
            for(int t = 0; t < TAG_COUNT; t++) fprintf(f, " %f", output->mfact[t]);
            for(int t = 0; t < TAG_COUNT; t++) fprintf(f, " %d", (int) output->layout[t]);
            fprintf(f, "\n");
        }

        Window *window;
        wl_list_for_each(window, &axe.windows, link) {
            if(window->identifier[0] == '\0') continue; // never got one yet

            int mon_index = output_index_of(window->mon);
            fprintf(f, "W %s %u %d %d %d %d %d %d\n",
                    window->identifier, window->tagmask, window->floating ? 1 : 0,
                    window->floatx, window->floaty, window->floatw, window->floath, mon_index);
        }

        fclose(f);
    }

    // execvp(saved_argv[0], saved_argv);
    bool already_flagged = false;
    for(int i = 1; i < saved_argc; i++) {
        if(strcmp(saved_argv[i], "--no-autostart") == 0) {
            already_flagged = true;
            break;
        }
    }

    char **exec_argv = saved_argv;
    if(!already_flagged) {
        exec_argv = calloc(saved_argc + 2, sizeof(char *));
        for(int i = 0; i < saved_argc; i++) exec_argv[i] = saved_argv[i];
        exec_argv[saved_argc] = "--no-autostart";
        exec_argv[saved_argc + 1] = NULL;
    }

    bar_kill_status();
    execvp(exec_argv[0], exec_argv);

    // Only reached if execvp itself failed (binary missing, bad perms).
    fprintf(stderr, "restart: execvp failed, exiting instead\n");
    exit(1);
}
