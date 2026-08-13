#define SHCMD(cmd) \
{ .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

#include "axe.h"

#define CONTROL RIVER_SEAT_V1_MODIFIERS_CTRL
#define SUPER   RIVER_SEAT_V1_MODIFIERS_MOD4
#define ALT     RIVER_SEAT_V1_MODIFIERS_MOD1
#define SHIFT   RIVER_SEAT_V1_MODIFIERS_SHIFT

/* layout */
static const int nmaster = 1;    /* initial number of windows in the master area */
static const float mfact = 0.5; /* initial master area size [0.05 .. 0.95] */

/* borders (r, g, b, a; 0-255 each) */
static const unsigned int borderpx = 2;
static const uint8_t bordercolor_focus[4]  = { 0xd8, 0xde, 0xe9, 0xff };
static const uint8_t bordercolor_normal[4] = { 0x4c, 0x56, 0x6a, 0xff };
static const uint8_t bordercolor_float[4]  = { 0xd8, 0xa8, 0x4c, 0xff };

/* status bar */
static const bool bar_at_bottom = true; /* false = top of screen, true = bottom */
static const int bar_height = 18;
static const char *bar_font_name = "monospace";
static const char *bar_emoji_font_name = "Noto Color Emoji";
static const int bar_font_size = 16;
static const uint8_t bar_bg_color[4]     = { 0x1a, 0x1a, 0x1a, 0xff }; /* whole bar background */
static const uint8_t bar_tag_fg_color[4] = { 0xd8, 0xde, 0xe9, 0xff }; /* occupied-tag text + status text */
static const uint8_t bar_sel_bg_color[4] = { 0x4c, 0x56, 0x6a, 0xff }; /* highlight behind the current tag */

/* takes over the whole bar background/text while passthrough mode is on
 * (see togglepassthrough in actions.c) - impossible to miss, so you
 * don't forget every bind but the toggle itself is currently disabled. */
static const uint8_t bar_passthrough_bg_color[4] = { 0x7a, 0x1f, 0x1f, 0xff };
static const uint8_t bar_passthrough_fg_color[4] = { 0xff, 0xe8, 0xe8, 0xff };

/* gaps (px). outer_gap: between tiled windows and the screen edge.
 * inner_gap: between adjacent tiled windows. Both are independent of
 * borderpx - they stack with it, not replace it. smart_gap: when true,
 * a lone tiled window on an output gets no gap (or border) at all,
 * same idea as the existing smart-border behavior. */
static const bool smart_gap  = false;
static const int outer_gap   = 10;
static const int inner_gap   = 5;

/* tabbed layout (Super+Tab), per-tag: gaps/borders are suppressed for
 * that tag and every tiled window shares one full-size slot; an i3-style
 * tab strip lists them across the top or bottom (tab_at_bottom, separate
 * from bar_at_bottom), drawn with the same font/rendering path as the
 * status bar above. Only shown once there's more than one tiled window
 * to switch between. */
static const bool tab_at_bottom = false;
static const int tab_height = 20;
static const uint8_t tab_bg_color[4]     = { 0x1a, 0x1a, 0x1a, 0xff };
static const uint8_t tab_fg_color[4]     = { 0xd8, 0xde, 0xe9, 0xff };
static const uint8_t tab_sel_bg_color[4] = { 0x4c, 0x56, 0x6a, 0xff };
static const uint8_t tab_sel_fg_color[4] = { 0xff, 0xff, 0xff, 0xff };

/* nvHopper-style marks picker (marksui.c) - a small centered overlay
 * listing all MARK_COUNT (axe.h) slots. Toggled with togglemarksui
 * below; while open, j/k move the highlighted row, shift+j/shift+k swap
 * the highlighted slot's mark with its neighbor (reordering which
 * window a given MARKKEY jumps to), Return jumps to the highlighted
 * mark and closes, Escape closes without jumping, d clears the
 * highlighted slot. None of those five are rebindable here - only via
 * marksui_setup_seat() in marksui.c, since they're deliberately fixed
 * "modal" keys rather than regular global binds (see that file's top
 + * comment for why). Its own font is loaded once at startup, separate
 * from bar_font_size/bar_font_name, at marksui_font_scale times the
 * size (see bar_init() in bar.c) - not just a bigger row, an actually
 * bigger typeface. */
static const int marksui_font_scale = 2;
static const int marksui_width = 820;
static const int marksui_row_height = 56;
static const uint8_t marksui_bg_color[4]     = { 0x1a, 0x1a, 0x1a, 0xf0 };
static const uint8_t marksui_fg_color[4]     = { 0xd8, 0xde, 0xe9, 0xff };
static const uint8_t marksui_sel_bg_color[4] = { 0x4c, 0x56, 0x6a, 0xff };
static const uint8_t marksui_sel_fg_color[4] = { 0xff, 0xff, 0xff, 0xff };

/* spawned once at startup and left running; each newline it writes to
 * stdout becomes the new status text (no polling - update on your own
 * schedule, e.g. via a signal to your own script). NULL to disable. */
static const char *bar_status_cmd = "stat-sway";

/* show the bar only while the Super key is held, hiding (and reclaiming
 * its screen space) on release. When false, use the togglebar keybind
 * below instead for a manual, persistent toggle. */
static const bool bar_autohide = true;

/* same idea as bar_autohide, but for the tab strip - independent switch,
 * so you can e.g. autohide the bar but always keep tabs visible, or vice
 * versa. Shares the same Super-hold gesture as the bar (see
 * bar_setup_seat_autohide in bar.c) rather than a second one, so holding
 * Super reveals whichever of the two have autohide enabled. */
static const bool tab_autohide = true;

/* keyboard layout - comma-separated, matches xkb_rule_names.layout/options */
static const char *xkb_layout  = "us,np";
static const char *xkb_options = "grp:shift_caps_toggle";
/* static const char *xkb_options = "grp:sclk_toggle,caps:swapescape"; */
static const bool numlock_default_on = true;

/* keyboard repeat, applied to every keyboard device */
static const int repeat_rate  = 35;
static const int repeat_delay = 300;

/* pointer acceleration applied to every pointer device (mice + touchpads) */
static const int pointer_accel_profile = RIVER_LIBINPUT_DEVICE_V1_ACCEL_PROFILE_FLAT;
static const double pointer_accel_speed = 0.0; /* [-1 .. 1], 0 = device default */

/* touchpad-only settings (device is treated as a touchpad if it reports
 * tap-to-click support at all - libinput doesn't otherwise label devices) */
static const bool touchpad_natural_scroll  = false;
static const bool touchpad_dwt             = true;  /* disable-while-typing */
static const bool touchpad_tap             = true;
static const bool touchpad_middle_emulation = false;
static const int  touchpad_click_method    = RIVER_LIBINPUT_DEVICE_V1_CLICK_METHOD_CLICKFINGER;
static const int  touchpad_scroll_method   = RIVER_LIBINPUT_DEVICE_V1_SCROLL_METHOD_TWO_FINGER;

/* commands run once at startup */
static const char *autostart[][8] = {
    // { "waybar", NULL },
    { "mpd", NULL },
    { "axe-trayd.py", NULL },
    { "foot", "-s", NULL },
    { "waybg", NULL },
    { "systemctl", "--user", "import-environment", "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", NULL },
    { "dbus-update-activation-environment", "--systemd", "DISPLAY", "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP=river", NULL },
    { "nightcolor", NULL },
    { "transmission-daemon", NULL },
    { "batteryAlert", NULL },
    { "wl-paste", "--type", "text", "--watch", "cliphist", "store", NULL },
    { "wl-paste", "--type", "image", "--watch", "cliphist", "store", NULL },
    { NULL },
};

/* per-window rules, matched by app_id (exact) and/or title (POSIX
 * extended regex, case-insensitive) - either may be NULL for "any".
 * floating: -1 unchanged, 0 force tiled, 1 force floating.
 * tag/monitor: -1 = unchanged.
 * float_width/float_height: fraction (0.0-1.0] of the monitor's usable
 * area to use as the default floating size instead of the client's own
 * requested size - 0 to just use the client's size, as before. Only
 * takes effect the first time the window floats (won't override a
 * remembered/restored geometry). */
static const Rule rules[] = {
    /* app_id                     title regex          floating tag monitor float_w float_h */
    { "xdg-desktop-portal-gtk",   NULL,                   1,    -1,  -1,     0,      0 },
    { NULL,                       "^(Open|Save) File",    1,    -1,  -1,     0.5,    0.5 },
    // { "pavucontrol",              NULL,                   1,    -1,  -1,     0,      0 },
    { "mpv",                      NULL,                   1,    -1,  -1,     0,      0 },
    // { "galculator",               NULL,                   1,    -1,  -1,     0,      0 },
    { "galculator",               NULL,                   1,    -1,  -1,     0.4,    0.7 },
    { "ncmpcpp",                  NULL,                   1,    -1,  -1,     0.75,   0.75 },
    { "file_progress",            NULL,                   1,    -1,  -1,     0,      0 },
    { "float",                    NULL,                   1,    -1,  -1,     0,      0 },
};

/* idle timeouts: after this many ms of seat inactivity, run `command`;
 * on activity resuming, run `resume_command` (NULL to skip). */
static const IdleTimeout idle_timeouts[] = {
    { 1200*1000, "systemctl suspend", NULL }, // Started using magic numbers but its easier
    // { 180000, "brightnessctl -s set 10%", "brightnessctl -r" },
};

/* turn every output off via wlr-output-power-management-unstable-v1
 * after this many ms idle, back on on resume. 0 or negative disables. */
static const int display_off_timeout_ms = 60*1000;

#define TAGKEY(KEY,TAG) \
{SUPER,               KEY, view,       { .u = 1 << TAG } }, \
    {SUPER|CONTROL,       KEY, toggleview, { .u = 1 << TAG } }, \
    {SUPER|SHIFT,         KEY, tag,        { .u = 1 << TAG } }, \
    {SUPER|SHIFT|CONTROL, KEY, toggletag,  { .u = 1 << TAG } },

// Jump straight to the Nth tab (1-9)
// TAGKEY above (toggleview), SUPER+ALT
#define TABKEY(KEY,N) \
{CONTROL, KEY, tabselect, { .i = N } },

#define MARKKEY(KEY,N) \
{ALT,       KEY, gotomark,   { .i = N } }, \
    {ALT|SHIFT, KEY, markwindow, { .i = N } },

static const char *termcmd[] = { "footclient", NULL };
static const char *launchercmd[] = { "fuzzel", NULL };

static Keys keybinds[] = {
    {SUPER,         XKB_KEY_period, select_next_mon, {0} },
    {SUPER,         XKB_KEY_comma,  select_prev_mon, {0} },
    {SUPER|SHIFT,   XKB_KEY_period, movemon,         { .i = +1 } },
    {SUPER|SHIFT,   XKB_KEY_comma,  movemon,         { .i = -1 } },

    /* move focus up/down the stack, without moving windows */
    {SUPER,         XKB_KEY_j,      focus_next,      {0} },
    {SUPER,         XKB_KEY_k,      focus_prev,      {0} },

    /* move the focused window's position in the stack */
    {SUPER|SHIFT,   XKB_KEY_j,      movestack,       { .i = +1 } },
    {SUPER|SHIFT,   XKB_KEY_k,      movestack,       { .i = -1 } },

    /* master area size */
    {SUPER,         XKB_KEY_o,      incnmaster,      { .i = +1 } },
    {SUPER|SHIFT,   XKB_KEY_o,      incnmaster,      { .i = -1 } }, /* was SUPER|SUPER (== SUPER), a duplicate of the line above */
    {SUPER,         XKB_KEY_h,      setmfact,        { .f = -0.05 } },
    {SUPER,         XKB_KEY_l,      setmfact,        { .f = +0.05 } },

    {SUPER|CONTROL, XKB_KEY_Return, zoom,            {0} },
    {SUPER,         XKB_KEY_f,  togglefloating,  {0} },
    {SUPER|SHIFT,   XKB_KEY_f,      togglefullscreen,{0} },
    {SUPER,         XKB_KEY_g,      togglesticky,    {0} },
    {SUPER,         XKB_KEY_Tab,      togglelayout,    {0} },

    {SUPER,         XKB_KEY_q,      destroy_window,  {0} },
    {SUPER|SHIFT,   XKB_KEY_q,      exit_session,    {0} },
    {SUPER|CONTROL, XKB_KEY_q,      restart_axe,     {0} },
    {SUPER|ALT,     XKB_KEY_b,      togglebar,       {0} },
    {SUPER|CONTROL, XKB_KEY_p,      togglepassthrough, {0} },
    {SUPER|SHIFT,   XKB_KEY_g,      togglemarksui,   {0} },

    // {SUPER|SHIFT,   XKB_KEY_Return, spawn,           { .v = termcmd } },
    // {SUPER,         XKB_KEY_Return, spawn,           SHCMD("$TERMINAL -e $(tmux attach || tmux new -s nonSense)") },
    // {SUPER,         XKB_KEY_w,      spawn,           SHCMD("$BROWSER")},
    // {ALT|SHIFT,     XKB_KEY_space,  spawn,           SHCMD("mpc toggle")},
    // {SUPER,         XKB_KEY_space,  spawn,           { .v = launchercmd } },
    {SUPER,                 XKB_KEY_b,              spawn,          SHCMD("kill -USR1 $(cat /tmp/sway-status.pid)")},
    /* Clipboard & Notes */
    { SUPER|SHIFT,          XKB_KEY_v,              spawn,          SHCMD("clip2Note") },
    { SUPER,                XKB_KEY_z,              spawn,          SHCMD("cliphist list | fuzzel --dmenu | cliphist decode | wl-copy") },
    { SUPER,                XKB_KEY_Return,         spawn,          SHCMD("$TERMINAL -e sh -c \"tmux attach || tmux new -s nonSense\"") },
    { SUPER,                XKB_KEY_space,          spawn,          { .v = launchercmd } },
    { SUPER,                XKB_KEY_y,              spawn,          SHCMD("$TERMINAL -e tSess") }, /* was $term (unset var) */
    // { SUPER|SHIFT,       XKB_KEY_space,          spawn,          { .v = launchercmd } },
    { SUPER|SHIFT,          XKB_KEY_Return,         spawn,          { .v = termcmd } },
    // { ALT,               XKB_KEY_Return,         spawn,          SHCMD("$TERMINAL -e openInVim") },
    { SUPER,                XKB_KEY_d,              spawn,          SHCMD("$TERMINAL -e vid-grab") },

    /* Brightness */
    { SUPER|ALT,            XKB_KEY_equal,          spawn,          SHCMD("brightnessctl -e set 3%+") },
    { SUPER|ALT,            XKB_KEY_minus,          spawn,          SHCMD("brightnessctl -e set 3%-") },

    /* Utilities */
    // { SUPER|CONTROL,     XKB_KEY_v,              spawn,          SHCMD("pavucontrol") },
    { SUPER,                XKB_KEY_c,              spawn,          SHCMD("galculator") },
    // { ALT,                XKB_KEY_c,              spawn,          SHCMD("galculator") },

    /* Music Player */
    { SUPER|ALT,            XKB_KEY_Return,         spawn,          SHCMD("$TERMINAL -a ncmpcpp -e ncmpcpp") },

    /* Screenshots */
    { CONTROL|SHIFT,        XKB_KEY_s,              spawn,          SHCMD("scrshot cpy") },
    { SUPER,                XKB_KEY_s,              spawn,          SHCMD("scrshot fullscr") },
    { SUPER|SHIFT,          XKB_KEY_s,              spawn,          SHCMD("scrshot sel") },
    { 0,                    XKB_KEY_Print,          spawn,          SHCMD("scrshot fullscr") },
    { SHIFT,                XKB_KEY_Print,          spawn,          SHCMD("scrshot sel") },

    /* Misc Tools */
    { SUPER|SHIFT,          XKB_KEY_y,              spawn,          SHCMD("emoji-picker") },
    { SUPER,                XKB_KEY_t,              spawn,          SHCMD("axe-tray-menu.py") },

    /* XF86 / Hardware Keys */
    { 0,                    XKB_KEY_XF86MonBrightnessUp,   spawn,   SHCMD("brightnessctl -e set 2%+") },
    { 0,                    XKB_KEY_XF86MonBrightnessDown, spawn,   SHCMD("brightnessctl -e set 2%-") },
    { 0,                    XKB_KEY_XF86AudioLowerVolume,  spawn,   SHCMD("wpctl set-volume @DEFAULT_AUDIO_SINK@ 3%-") },
    { 0,                    XKB_KEY_XF86AudioRaiseVolume,  spawn,   SHCMD("wpctl set-volume @DEFAULT_AUDIO_SINK@ 3%+") },
    { 0,                    XKB_KEY_XF86AudioMute,         spawn,   SHCMD("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle") },
    { 0,                    XKB_KEY_XF86AudioMicMute,      spawn,   SHCMD("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle") },
    { 0,                    XKB_KEY_XF86AudioPrev,         spawn,   SHCMD("mpc prev") },
    { 0,                    XKB_KEY_XF86AudioNext,         spawn,   SHCMD("mpc next") },
    { 0,                    XKB_KEY_XF86AudioPlay,         spawn,   SHCMD("mpc toggle") },
    { 0,                    XKB_KEY_XF86HomePage,          spawn,   SHCMD("librewolf") },
    { 0,                    XKB_KEY_XF86Calculator,        spawn,   SHCMD("galculator") },
    { 0,                    XKB_KEY_XF86Favorites,         spawn,   SHCMD("emoji-picker") },
    { 0,                    XKB_KEY_XF86Phone,             spawn,   SHCMD("brightnessctl -e set 2%+") },
    { 0,                    XKB_KEY_XF86HangupPhone,       spawn,   SHCMD("brightnessctl -e set 2%-") },
    { 0,                    XKB_KEY_Help,                  spawn,   SHCMD("$BROWSER") },

    /* Media with Alt (Mod1) */
    { ALT,                  XKB_KEY_Up,             spawn,          SHCMD("wpctl set-volume @DEFAULT_AUDIO_SINK@ 3%+") },
    { ALT,                  XKB_KEY_Down,           spawn,          SHCMD("wpctl set-volume @DEFAULT_AUDIO_SINK@ 3%-") },
    { ALT|SHIFT,            XKB_KEY_space,          spawn,          SHCMD("mpc toggle") },
    { ALT|SHIFT,            XKB_KEY_Right,          spawn,          SHCMD("mpc next") },
    { ALT|SHIFT,            XKB_KEY_Left,           spawn,          SHCMD("mpc prev") },
    { ALT,                  XKB_KEY_bracketleft,    spawn,          SHCMD("mpc seek -10") },
    { ALT,                  XKB_KEY_bracketright,   spawn,          SHCMD("mpc seek +10") },
    { ALT|SHIFT,            XKB_KEY_bracketleft,    spawn,          SHCMD("mpc seek -60") },
    { ALT|SHIFT,            XKB_KEY_bracketright,   spawn,          SHCMD("mpc seek +60") },
    // { ALT,               XKB_KEY_equal,          spawn,          SHCMD("mpc vol +5") },
    // { ALT,               XKB_KEY_minus,          spawn,          SHCMD("mpc vol -5") },
    { ALT|CONTROL,          XKB_KEY_space,          spawn,          SHCMD("mpc single") },
    { ALT,                  XKB_KEY_apostrophe,     spawn,          SHCMD("mpc seek 0%") },

    /* Wallpaper & Power */
    { SUPER|SHIFT,          XKB_KEY_w,              spawn,          SHCMD("set-wallpaper") },
    { ALT|CONTROL,          XKB_KEY_w,              spawn,          SHCMD("randomize-wall") },
    // { SUPER,             XKB_KEY_grave,          spawn,          SHCMD("alacritty") },

    { SUPER,                XKB_KEY_BackSpace,      spawn,          SHCMD("power") },
    { SUPER|SHIFT,          XKB_KEY_d,              spawn,          SHCMD("pcmanfm") },
    { SUPER|SHIFT,          XKB_KEY_e,              spawn,          SHCMD("$TERMINAL -e htop") },
    { SUPER,                XKB_KEY_w,              spawn,          SHCMD("$BROWSER") },

    { SUPER,                XKB_KEY_v,              spawn,          SHCMD("$TERMINAL -e transg-tui") },
    // { SUPER,             XKB_KEY_x,              spawn,          SHCMD("$TERMINAL -e ytfzf") },
    // { SUPER|SHIFT,       XKB_KEY_N,              spawn,          SHCMD("$TERMINAL -e tmux new") },
    { SUPER,                XKB_KEY_slash,          spawn,          SHCMD("mount-drives") },
    { SUPER|CONTROL,        XKB_KEY_slash,          spawn,          SHCMD("mount-and") },
    { SUPER|SHIFT,          XKB_KEY_slash,          spawn,          SHCMD("umount-drives") },
    { SUPER|CONTROL,        XKB_KEY_r,              spawn,          SHCMD("webcam-show") },
    { SUPER,                XKB_KEY_e,              spawn,          SHCMD("qr-gen") },
    { SUPER|SHIFT,          XKB_KEY_r,              spawn,          SHCMD("qr-Reader") },
    { SUPER,                XKB_KEY_m,              spawn,          SHCMD("movie-watch") },
    { SUPER|SHIFT,          XKB_KEY_b,              spawn,          SHCMD("read-book") },

    /* Gammastep / Nightcolor */
    { SUPER,                XKB_KEY_u,              spawn,          SHCMD("nightcolor 1") },
    { SUPER|SHIFT,          XKB_KEY_u,              spawn,          SHCMD("nightcolor") },

    /* Network / Media / Bar Controls */
    // { ALT,               XKB_KEY_w,              spawn,          SHCMD("$TERMINAL -e nmtui") },
    { SUPER|SHIFT,          XKB_KEY_m,              spawn,          SHCMD("mpv-play") },
    // { SUPER,             XKB_KEY_b,              spawn,          SHCMD("killall -SIGUSR1 waybar") },
    // { SUPER,             XKB_KEY_b,              spawn,          SHCMD("bar mode toggle") },
    { SUPER,                XKB_KEY_a,              spawn,          SHCMD("$TERMINAL -e lf") },
    // { SUPER|SHIFT,       XKB_KEY_Return,         spawn,          SHCMD("$TERMINAL -e openInVim") },
    { SUPER,                XKB_KEY_p,              spawn,          SHCMD("tmux new-window -n \"Nvim\" \"fzf-proj\"") },
    { SUPER,                XKB_KEY_n,              spawn,          SHCMD("$TERMINAL -e take-notes") },
    { SUPER,                XKB_KEY_i,              spawn,          SHCMD("drawing") },
    { CONTROL|SHIFT,        XKB_KEY_x,              spawn,          SHCMD("killTask") },

    { SUPER,                XKB_KEY_semicolon,      spawn,          SHCMD("spellchk") },

    TAGKEY(XKB_KEY_1, 0)
    TAGKEY(XKB_KEY_2, 1)
    TAGKEY(XKB_KEY_3, 2)
    TAGKEY(XKB_KEY_4, 3)
    TAGKEY(XKB_KEY_5, 4)
    TAGKEY(XKB_KEY_6, 5)
    TAGKEY(XKB_KEY_7, 6)
    TAGKEY(XKB_KEY_8, 7)
    TAGKEY(XKB_KEY_9, 8)

    TABKEY(XKB_KEY_1, 1)
    TABKEY(XKB_KEY_2, 2)
    TABKEY(XKB_KEY_3, 3)
    TABKEY(XKB_KEY_4, 4)
    TABKEY(XKB_KEY_5, 5)
    TABKEY(XKB_KEY_6, 6)
    TABKEY(XKB_KEY_7, 7)
    TABKEY(XKB_KEY_8, 8)
    TABKEY(XKB_KEY_9, 9)

    MARKKEY(XKB_KEY_q, 0)
    MARKKEY(XKB_KEY_w, 1)
    MARKKEY(XKB_KEY_e, 2)
    MARKKEY(XKB_KEY_r, 3)
    MARKKEY(XKB_KEY_t, 4)
};

static Mousebinds mousebinds[] = {
    {SUPER, BTN_LEFT,  movewin,   {0} },
    {SUPER, BTN_RIGHT, resizewin, {0} },
};
