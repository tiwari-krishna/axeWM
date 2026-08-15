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
    bool sticky;
    bool fullscreen;
    bool floating_explicit;
    int floatx, floaty, floatw, floath;
    int mon_index;
    int pre_fullscreen_mon_index;
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

// One non-empty axe.marks[] slot - identifier-keyed like SavedWindow, so
// it's restored the same way: consumed in apply_saved_window_state() the
// moment a window with a matching identifier re-announces itself.
typedef struct {
    int slot;
    char identifier[33];
    struct wl_list link;
} SavedMark;

static struct wl_list saved_windows;
static struct wl_list saved_outputs;
static struct wl_list saved_marks;
static bool state_loaded = false;

// -1 = no saved value (old-format file, or selmon was NULL at save time -
// e.g. no monitor ever got a pointer_position event). Consumed as each
// Output registers, in apply_saved_output_state() below - never matches
// a real output_index_of() result (always >= 0 for a registered output),
// so it's a safe default with no extra guard needed at the call site.
static int saved_selmon_index = -1;

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
    wl_list_init(&saved_marks);
    saved_selmon_index = -1;
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
            int floating_int, sticky_int, fullscreen_int, floating_explicit_int;
            // Same "widened, older files just get discarded cleanly"
            // approach as the O-line comment above - sticky/fullscreen/
            // floating_explicit/pre_fullscreen_mon_index are new fields
            // appended at the end, so a pre-upgrade state file's W line
            // runs short on this fscanf and `!= 12` catches it exactly
            // like a truncated line always has here.
            if(fscanf(f, "%32s %u %d %d %d %d %d %d %d %d %d %d",
                      w->identifier, &w->tagmask, &floating_int,
                      &w->floatx, &w->floaty, &w->floatw, &w->floath, &w->mon_index,
                      &sticky_int, &fullscreen_int, &floating_explicit_int,
                      &w->pre_fullscreen_mon_index) != 12) {
                free(w);
                break;
            }
            w->floating = floating_int != 0;
            w->sticky = sticky_int != 0;
            w->fullscreen = fullscreen_int != 0;
            w->floating_explicit = floating_explicit_int != 0;
            wl_list_insert(&saved_windows, &w->link);
        } else if(kind[0] == 'M') {
            SavedMark *m = calloc(1, sizeof(SavedMark));
            if(fscanf(f, "%d %32s", &m->slot, m->identifier) != 2 || m->slot < 0 || m->slot >= MARK_COUNT) {
                free(m);
                break;
            }
            wl_list_insert(&saved_marks, &m->link);
        } else if(kind[0] == 'S') {
            if(fscanf(f, "%d", &saved_selmon_index) != 1) break;
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

    // Independent of whether this output also has a SavedOutput entry
    // below (tag/layout state) - the two are unrelated, so this must
    // not live inside that loop or be skipped if that loop finds nothing.
    if(index == saved_selmon_index) selmon = output;

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
        window->sticky = w->sticky;
        window->fullscreen = w->fullscreen;
        // Locks the restored `floating` value against the "has a dialog
        // parent -> auto-float" heuristic in river_window_v1_parent
        // (window.c) re-triggering post-restart - same flag togglefloating
        // and a matching config.h rule already set for the same reason.
        window->floating_explicit = w->floating_explicit;
        window->floatx = w->floatx;
        window->floaty = w->floaty;
        window->floatw = w->floatw;
        window->floath = w->floath;
        window->got_real_dimensions = true;

        Output *mon = output_by_index(w->mon_index);
        if(mon != NULL) window->mon = mon;

        // Only meaningful alongside ->fullscreen - mirrors what
        // river_window_v1_fullscreen_requested (window.c) sets live: the
        // output to return to on exit_fullscreen_requested. -1 means
        // there wasn't one (not fullscreen, or fullscreen with no output
        // override), matching pre_fullscreen_mon's live default of NULL.
        if(w->pre_fullscreen_mon_index >= 0) {
            Output *pf = output_by_index(w->pre_fullscreen_mon_index);
            if(pf != NULL) window->pre_fullscreen_mon = pf;
        }

        clamp_float_geometry(window, window->mon);

        // Consumed - drop it rather than holding it for the rest of the
        // process's life (and so a duplicate identifier can't reapply it
        // to a second window later).
        wl_list_remove(&w->link);
        free(w);
        break;
    }

    // Same identifier-match, independent list - a window can have both a
    // SavedWindow entry and a SavedMark entry (or just one, or neither).
    SavedMark *m, *m_tmp;
    wl_list_for_each_safe(m, m_tmp, &saved_marks, link) {
        if(strcmp(m->identifier, window->identifier) != 0) continue;

        axe.marks[m->slot] = window;
        wl_list_remove(&m->link);
        free(m);
    }
}

void restart_axe(Seat *seat, Arg *arg) {
    FILE *f = fopen(state_path(), "w");
    if(f == NULL) {
        fprintf(stderr, "restart: failed to open %s for writing, restarting without saved state\n", state_path());
    } else {
        if(selmon != NULL) fprintf(f, "S %d\n", output_index_of(selmon));

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
            int pre_fs_mon_index = window->pre_fullscreen_mon ? output_index_of(window->pre_fullscreen_mon) : -1;
            fprintf(f, "W %s %u %d %d %d %d %d %d %d %d %d %d\n",
                    window->identifier, window->tagmask, window->floating ? 1 : 0,
                    window->floatx, window->floaty, window->floatw, window->floath, mon_index,
                    window->sticky ? 1 : 0, window->fullscreen ? 1 : 0, window->floating_explicit ? 1 : 0,
                    pre_fs_mon_index);
        }

        // Marks are identifier-keyed just like windows above - a mark on
        // a window that never got an identifier (closed before one
        // arrived) can't be restored, same reasoning as the `continue`
        // above, so it's silently dropped rather than saved as garbage.
        for(int i = 0; i < MARK_COUNT; i++) {
            if(axe.marks[i] == NULL || axe.marks[i]->identifier[0] == '\0') continue;
            fprintf(f, "M %d %s\n", i, axe.marks[i]->identifier);
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
