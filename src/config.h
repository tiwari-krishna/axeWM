#include "anvl.h"

#define CONTROL RIVER_SEAT_V1_MODIFIERS_CTRL
#define SUPER   RIVER_SEAT_V1_MODIFIERS_MOD4
#define SHIFT   RIVER_SEAT_V1_MODIFIERS_SHIFT

/* layout */
static const int nmaster = 1;    /* initial number of windows in the master area */
static const float mfact = 0.5; /* initial master area size [0.05 .. 0.95] */

/* borders (r, g, b, a; 0-255 each) */
static const unsigned int borderpx = 2;
static const uint8_t bordercolor_focus[4]  = { 0xd8, 0xde, 0xe9, 0xff };
static const uint8_t bordercolor_normal[4] = { 0x4c, 0x56, 0x6a, 0xff };

#define TAGKEY(KEY,TAG) \
	{SUPER,               KEY, view,       { .u = 1 << TAG } }, \
	{SUPER|CONTROL,       KEY, toggleview, { .u = 1 << TAG } }, \
	{SUPER|SHIFT,         KEY, tag,        { .u = 1 << TAG } }, \
	{SUPER|SHIFT|CONTROL, KEY, toggletag,  { .u = 1 << TAG } },

static const char *termcmd[] = { "foot", NULL };
static const char *launchercmd[] = { "fuzzel", NULL };

Keys keybinds[] = {
  {SUPER,         XKB_KEY_period, select_next_mon, {0} },
  {SUPER,         XKB_KEY_comma,  select_prev_mon, {0} },

  /* move focus up/down the stack, without moving windows */
  {SUPER,         XKB_KEY_j,      focus_next,      {0} },
  {SUPER,         XKB_KEY_k,      focus_prev,      {0} },

  /* move the focused window's position in the stack */
  {SUPER|SHIFT,   XKB_KEY_j,      movestack,       { .i = +1 } },
  {SUPER|SHIFT,   XKB_KEY_k,      movestack,       { .i = -1 } },

  /* master area size */
  {SUPER,         XKB_KEY_i,      incnmaster,      { .i = +1 } },
  {SUPER,         XKB_KEY_d,      incnmaster,      { .i = -1 } },
  {SUPER,         XKB_KEY_h,      setmfact,        { .f = -0.05 } },
  {SUPER,         XKB_KEY_l,      setmfact,        { .f = +0.05 } },

  {SUPER,         XKB_KEY_Return, spawn,           { .v = termcmd } },
  {SUPER,         XKB_KEY_p,      spawn,           { .v = launchercmd } },
  {SUPER|SHIFT,   XKB_KEY_Return, zoom,            {0} },
  {SUPER|SHIFT,   XKB_KEY_space,  togglefloating,  {0} },
  {SUPER,         XKB_KEY_f,      togglefullscreen,{0} },

  {SUPER|SHIFT,   XKB_KEY_c,      destroy_window,  {0} },
  {SUPER|SHIFT,   XKB_KEY_q,      exit_session,    {0} },

  TAGKEY(XKB_KEY_1, 0)
  TAGKEY(XKB_KEY_2, 1)
  TAGKEY(XKB_KEY_3, 2)
  TAGKEY(XKB_KEY_4, 3)
  TAGKEY(XKB_KEY_5, 4)
  TAGKEY(XKB_KEY_6, 5)
  TAGKEY(XKB_KEY_7, 6)
  TAGKEY(XKB_KEY_8, 7)
  TAGKEY(XKB_KEY_9, 8)
};
