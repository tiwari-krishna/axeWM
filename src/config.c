#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "axe.h"
#include "config.h"


#define CONTROL RIVER_SEAT_V1_MODIFIERS_CTRL
#define SUPER   RIVER_SEAT_V1_MODIFIERS_MOD4
#define ALT     RIVER_SEAT_V1_MODIFIERS_MOD1
#define SHIFT   RIVER_SEAT_V1_MODIFIERS_SHIFT

size_t n_keybinds = 0;
Keys *final_keybinds = NULL;


typedef enum { ARG_NONE, ARG_FLOAT, ARG_INT, ARG_UINT, ARG_SPAWN } thisArgKind;

typedef struct {
    const char *name;
    void(*func)(Seat *seat, Arg *arg);
    thisArgKind kind;
} ActionEntry;


static void test_focus_next(Seat *seat, Arg *a) {(void)a; (void)seat; printf(" -> focus_next()\n");}

static void test_spawn(Seat *seat, Arg *a) {
    (void)seat;
    char **argv = (char **)a->v;
    printf("-> spawn(");
    for (int i = 0; argv[i]; i++)
        printf("%s\"%s\"", i ? ", " : "", argv[i]);
    printf(")\n");
}

static const ActionEntry action_table[] = {
    {"test_focus", test_focus_next, ARG_NONE},
    {"test_spawn", test_spawn, ARG_SPAWN},
    {"spawn", spawn, ARG_SPAWN},
    {"destroy_window", destroy_window, ARG_NONE},
    {"setmfact",setmfact, ARG_NONE},
    {"incnmaster",incnmaster, ARG_NONE},
    {"focus_prev",focus_prev, ARG_NONE},
    {"focus_next",focus_next, ARG_NONE},
    {"movemon",movemon, ARG_NONE},
    {"select_prev_mon",select_prev_mon, ARG_NONE},
    {"select_next_mon",select_next_mon, ARG_NONE},
    {"togglebar",togglebar, ARG_NONE},
    {"exit_session", exit_session, ARG_NONE},
    {NULL, NULL, ARG_NONE}
};

static const ActionEntry *find_action(const char *name) {
    for (int i = 0; action_table[i].name; i++)
        if(strcmp(action_table[i].name, name) == 0) return &action_table[i];
    return NULL;
}

static void *build_argv(lua_State *L, int idx, bool *ok) {
    *ok = true;
    
    if (lua_isstring(L, idx)) {
//        return SHCMD(lua_tostring(L, idx));
        char **argv = malloc(4 * sizeof(char *));
        argv[0] = strdup("/bin/sh");
        argv[1] = strdup("-c");
        argv[2] = strdup(lua_tostring(L, idx));
        argv[3] = NULL;
        return argv;
    }

    if (lua_istable(L, idx)) {
        int n = (int)lua_rawlen(L, idx);
        char **argv = malloc((size_t)(n + 1) * sizeof(char *));
        for (int i = 0; i < n; i++) {
            lua_rawgeti(L, idx, i + 1);
            argv[i] = strdup(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
            lua_pop(L, 1);
        }
        argv[n] = NULL;
        return argv;
    }
    *ok = false;
    return NULL;
}
 
static Keys *load_keybinds(lua_State *L, int *count) {
    lua_getfield(L, -1, "keybinds");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); *count = 0; return NULL; }
 
    int n = (int)lua_rawlen(L, -1);
    Keys *binds = calloc((size_t)n, sizeof(Key));
    int out = 0;
 
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i); /* push row (could be ANYTHING, even nil) */
 
        if (!lua_istable(L, -1)) {
            fprintf(stderr, "config: keybinds[%d] is not a table, skipping\n", i);
            lua_pop(L, 1);
            continue;
        }
 
        lua_rawgeti(L, -1, 1);
        if (!lua_isnumber(L, -1)) {
            fprintf(stderr, "config: keybinds[%d] mods missing/not a number, skipping\n", i);
            lua_pop(L, 2);
            continue;
        }
        uint32_t mods = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
 
        lua_rawgeti(L, -1, 2);
        if (!lua_isnumber(L, -1)) {
            fprintf(stderr, "config: keybinds[%d] key missing/not a number, skipping\n", i);
            lua_pop(L, 2);
            continue;
        }
        xkb_keysym_t key = (xkb_keysym_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
 
        lua_rawgeti(L, -1, 3);
        if (!lua_isstring(L, -1)) {
            fprintf(stderr, "config: keybinds[%d] action name missing/not a string, skipping\n", i);
            lua_pop(L, 2);
            continue;
        }
        const char *aname = lua_tostring(L, -1);
 
        const ActionEntry *act = find_action(aname);
        if (!act) {
            fprintf(stderr, "config: keybinds[%d] unknown action '%s', skipping\n", i, aname);
            lua_pop(L, 2);
            continue;
        }
        lua_pop(L, 1); /* action name string */
 
        Arg arg = {0};
        bool arg_ok = true;
        switch (act->kind) {
            case ARG_NONE:
                break;
            case ARG_INT:
                lua_rawgeti(L, -1, 4);
                if (lua_isnumber(L, -1)) arg.i = (int)lua_tointeger(L, -1);
                else { fprintf(stderr, "config: keybinds[%d] '%s' needs an int arg\n", i, aname); arg_ok = false; }
                lua_pop(L, 1);
                break;
            case ARG_UINT:
                lua_rawgeti(L, -1, 4);
                if (lua_isnumber(L, -1)) arg.u = (uint32_t)lua_tointeger(L, -1);
                else { fprintf(stderr, "config: keybinds[%d] '%s' needs a uint arg\n", i, aname); arg_ok = false; }
                lua_pop(L, 1);
                break;
            case ARG_FLOAT:
                lua_rawgeti(L, -1, 4);
                if (lua_isnumber(L, -1)) arg.f = (float)lua_tonumber(L, -1);
                else { fprintf(stderr, "config: keybinds[%d] '%s' needs a float arg\n", i, aname); arg_ok = false; }
                lua_pop(L, 1);
                break;
            case ARG_SPAWN: {
                lua_rawgeti(L, -1, 4);
                bool built_ok;
                arg.v = build_argv(L, -1, &built_ok);
                if (!built_ok) { fprintf(stderr, "config: keybinds[%d] '%s' needs string or table arg\n", i, aname); arg_ok = false; }
                lua_pop(L, 1);
                break;
            }
        }
 
        if (!arg_ok) { lua_pop(L, 1); continue; } /* pop row table */
 
        binds[out].mods = mods;
        binds[out].key  = key;
        binds[out].func = act->func;
        binds[out].arg  = arg;
        out++;
 
        lua_pop(L, 1); /* row table */
    }
    lua_pop(L, 1); /* keybinds table */
    *count = out;
    return binds;
}
 

static const luaL_Reg allowed_libs[] = {
    {"_G",     luaopen_base},
    {"math",   luaopen_math},
    {"string", luaopen_string},
    {"table",  luaopen_table},
    {NULL, NULL}
};

static void open_restricted_libs(lua_State *L) {
    for (const luaL_Reg *lib = allowed_libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
}

static int lua_key(lua_State *L){
    const char *name = luaL_checkstring(L, 1);
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    if(sym == XKB_KEY_NoSymbol)
        return luaL_error(L, "key(): unknown key name '%s'", name);
    lua_pushinteger(L, (lua_Integer)sym);
    return 1;
}


static bool run_sandboxed(lua_State *L, const char *path){
    lua_newtable(L);

    lua_pushinteger(L, CONTROL); lua_setfield(L, -2, "CONTROL");
    lua_pushinteger(L, SUPER); lua_setfield(L, -2, "SUPER");
    lua_pushinteger(L, ALT); lua_setfield(L, -2, "ALT");
    lua_pushinteger(L, SHIFT); lua_setfield(L, -2, "SHIFT");

    lua_pushcfunction(L, lua_key);
    lua_setfield(L, -2, "key");

    lua_pushglobaltable(L);
    lua_pushnil(L);

    while(lua_next(L, -2) != 0){
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_settable(L, -5);
    }

    lua_pop(L, 1);

    if(luaL_loadfile(L, path) != LUA_OK){
        fprintf(stderr, "load error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 2);
        return false;
    }

    lua_pushvalue(L, -2);
    lua_setupvalue(L, -2, 1);

    if(lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "run error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 2);
        return false;
    }

    return true;
}
static bool same_bind(uint32_t mods, xkb_keysym_t key, const Keys *b) {
    return b->mods == mods && b->key == key;
}

/* combines: lua_binds (n_lua of them) override static defaults with the
 * same mods+key; anything in defaults NOT overridden is kept as-is. */
static Keys *merge_keybinds(const Keys *defaults, size_t n_defaults,
                            Keys *lua_binds, int n_lua, size_t *out_count) {
    /* worst case: all defaults kept + all lua binds added */
    Keys *merged = calloc(n_defaults + (size_t)n_lua, sizeof(Key));
    size_t out = 0;

    for (size_t i = 0; i < n_defaults; i++) {
        bool overridden = false;
        for (int j = 0; j < n_lua; j++) {
            if (same_bind(lua_binds[j].mods, lua_binds[j].key, &defaults[i])) {
                overridden = true;
                break;
            }
        }
        if (!overridden) merged[out++] = defaults[i];
    }

    for (int j = 0; j < n_lua; j++)
        merged[out++] = lua_binds[j];

    *out_count = out;
    return merged;
}

int config_loder(){
        
    lua_State *L = luaL_newstate();
    open_restricted_libs(L);
 
    if (!run_sandboxed(L, "config.lua")) {
        lua_close(L);
        final_keybinds = keybinds;
        return 1;
    }

    int n = 0;
    Keys *lua_binds = load_keybinds(L, &n);
    lua_pop(L, 1);
    lua_close(L);

    final_keybinds = merge_keybinds(keybinds, LENGTH(keybinds), lua_binds, n, &n_keybinds);
  
    free(lua_binds);
    lua_binds = NULL;

    return 0;
} 
