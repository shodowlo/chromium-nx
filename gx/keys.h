/*
 * keys - the keyboard and mouse mode's bindings, shared by the launcher
 * (gx/launcher/main.c, where they are changed) and the browser (host_nx.c,
 * which acts on them, and overlay.c, which shows them).
 *
 * What a pad input gives is a key by its Linux number; the mouse's buttons
 * are 0x110 and up as in Linux, and the wheel has two numbers of ours.
 */
#ifndef GX_KEYS_H
#define GX_KEYS_H

#define GX_WHEEL_UP 0x150
#define GX_WHEEL_DOWN 0x151

struct gx_key {
	int code;
	const char *name;
};

static const struct gx_key gx_keys[] = {
	{ 0, "None" },
	{ 0x110, "Left click" }, { 0x111, "Right click" }, { 0x112, "Middle click" },
	{ GX_WHEEL_UP, "Wheel up" }, { GX_WHEEL_DOWN, "Wheel down" },
	{ 57, "Space" }, { 28, "Enter" }, { 1, "Escape" }, { 15, "Tab" }, { 14, "Backspace" },
	{ 42, "Shift" }, { 29, "Ctrl" }, { 56, "Alt" },
	{ 103, "Up" }, { 108, "Down" }, { 105, "Left" }, { 106, "Right" },
	{ 30, "A" }, { 48, "B" }, { 46, "C" }, { 32, "D" }, { 18, "E" }, { 33, "F" }, { 34, "G" },
	{ 35, "H" }, { 23, "I" }, { 36, "J" }, { 37, "K" }, { 38, "L" }, { 50, "M" }, { 49, "N" },
	{ 24, "O" }, { 25, "P" }, { 16, "Q" }, { 19, "R" }, { 31, "S" }, { 20, "T" }, { 22, "U" },
	{ 47, "V" }, { 17, "W" }, { 45, "X" }, { 21, "Y" }, { 44, "Z" },
	{ 2, "1" }, { 3, "2" }, { 4, "3" }, { 5, "4" }, { 6, "5" }, { 7, "6" }, { 8, "7" },
	{ 9, "8" }, { 10, "9" }, { 11, "0" },
	{ 59, "F1" }, { 60, "F2" }, { 61, "F3" }, { 62, "F4" }, { 63, "F5" }, { 64, "F6" },
	{ 65, "F7" }, { 66, "F8" }, { 67, "F9" }, { 68, "F10" }, { 87, "F11" }, { 88, "F12" },
	{ 12, "-" }, { 13, "=" }, { 26, "[" }, { 27, "]" }, { 39, ";" }, { 40, "'" },
	{ 41, "`" }, { 43, "\\" }, { 51, "," }, { 52, "." }, { 53, "/" },
};
#define GX_KEY_COUNT ((int)(sizeof(gx_keys) / sizeof(gx_keys[0])))

static inline const char *gx_key_name(int code)
{
	for (int i = 0; i < GX_KEY_COUNT; i++)
		if (gx_keys[i].code == code)
			return gx_keys[i].name;
	return "?";
}

/* The pad's inputs that can be bound, in the order of the launcher's list
 * and of gx's -K option; + is the overlay's and the right stick the mouse's. */
enum {
	GX_IN_LS_UP, GX_IN_LS_DOWN, GX_IN_LS_LEFT, GX_IN_LS_RIGHT,
	GX_IN_A, GX_IN_B, GX_IN_X, GX_IN_Y,
	GX_IN_L, GX_IN_R, GX_IN_ZL, GX_IN_ZR,
	GX_IN_UP, GX_IN_DOWN, GX_IN_LEFT, GX_IN_RIGHT,
	GX_IN_MINUS, GX_IN_L3, GX_IN_R3,
	GX_INPUTS
};

static const struct {
	const char *name;      /* as the launcher and the overlay show it */
	const char *setting;   /* its name in settings.ini */
	int default_key;
} gx_inputs[GX_INPUTS] = {
	{ "L stick up", "ls_up", 17 },       /* W */
	{ "L stick down", "ls_down", 31 },   /* S */
	{ "L stick left", "ls_left", 30 },   /* A */
	{ "L stick right", "ls_right", 32 }, /* D */
	{ "A", "a", 57 },                    /* space */
	{ "B", "b", 42 },                    /* shift */
	{ "X", "x", 48 },                    /* B */
	{ "Y", "y", 18 },                    /* E */
	{ "L", "l", GX_WHEEL_UP },
	{ "R", "r", GX_WHEEL_DOWN },
	{ "ZL", "zl", 0x111 },
	{ "ZR", "zr", 0x110 },
	{ "D-pad up", "up", 103 },
	{ "D-pad down", "down", 108 },
	{ "D-pad left", "left", 105 },
	{ "D-pad right", "right", 106 },
	{ "-", "minus", 1 },                 /* Escape */
	{ "L3", "l3", 29 },                  /* Ctrl */
	{ "R3", "r3", 20 },                  /* T */
};

#endif
