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
    int nmaster;
    float mfact;
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
            if(fscanf(f, "%d %d %f %u %u", &o->index, &o->nmaster, &o->mfact, &o->seltag, &o->tagmask) != 5) {
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
        output->nmaster = o->nmaster;
        output->mfact = o->mfact;
        output->seltag = o->seltag;
        output->tagmask = o->tagmask;
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

        Output *mon = output_by_index(w->mon_index);
        if(mon != NULL) window->mon = mon;

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
            fprintf(f, "O %d %d %f %u %u\n", output_index_of(output), output->nmaster, output->mfact, output->seltag, output->tagmask);
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

    execvp(exec_argv[0], exec_argv);

    // Only reached if execvp itself failed (binary missing, bad perms).
    fprintf(stderr, "restart: execvp failed, exiting instead\n");
    exit(1);
}
