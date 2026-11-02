/*
 * overlay - the panel + opens over the browser.
 *
 * Drawn by the processor, with the console's system font (FreeType), into a
 * picture the size of the screen: the page dimmed, and a panel in the middle
 * with what can be done. glwin.c lays that picture over the windows each time
 * it draws the screen, and the page is made to draw itself at every frame
 * while the panel is open (cdp_repaint()), or a page that does not change
 * would never show it.
 *
 * Everything here but overlay_picture() runs on the thread that reads the pad
 * (wl.c's). The picture is drawn into one of two buffers and then handed
 * over, so that the thread that draws the screen does not take one half
 * drawn.
 */
#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "overlay.h"
#include "keys.h"
#include "wl.h"

/* The screen as the page has it: landscape or portrait (wl.h). The buffers
 * hold either, the same number of pixels. */
#define W wl_width
#define H wl_height
#define PIXELS (WL_SCREEN_WIDTH * WL_SCREEN_HEIGHT)

/* RGBA in memory, R first; colours multiplied by their opacity. */
#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

static const uint32_t SURFACE = RGB(0x24, 0x28, 0x33), SURFACE_HI = RGB(0x2E, 0x33, 0x40);
/* A button not chosen: seen on a row, chosen (SURFACE_HI) or not. */
static const uint32_t BUTTON = RGB(0x40, 0x46, 0x56);
static const uint32_t TEXT = RGB(0xE8, 0xEA, 0xF0), TEXT_DIM = RGB(0x9A, 0xA0, 0xAE);
static const uint32_t ACCENT = RGB(0x8A, 0xB4, 0xF8), ACCENT_DARK = RGB(0x1F, 0x3B, 0x63);

static uint32_t *buffers[2], *pixels;
static uint32_t *shown;            /* what overlay_picture() hands out; NULL when closed */
static unsigned serial;

/* ---------- drawing ---------- */

static inline void blend(int x, int y, uint32_t c, int a)
{
	uint32_t d, out = 0;

	if (x < 0 || y < 0 || x >= W || y >= H || a <= 0)
		return;
	if (a >= 255) {
		pixels[y * W + x] = c;
		return;
	}
	d = pixels[y * W + x];
	for (int s = 0; s < 32; s += 8)
		out |= ((((c >> s) & 0xFF) * a + ((d >> s) & 0xFF) * (255 - a)) / 255) << s;
	pixels[y * W + x] = out;
}

static void rrect(int x, int y, int w, int h, int r, uint32_t c)
{
	if (r * 2 > h)
		r = h / 2;
	if (r * 2 > w)
		r = w / 2;
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			int dx = i < r ? r - i : i >= w - r ? i - (w - r - 1) : 0;
			int dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
			int a = 255;

			if (dx && dy) {
				float d = sqrtf((dx - 0.5f) * (dx - 0.5f) + (dy - 0.5f) * (dy - 0.5f));

				if (d > r)
					continue;
				if (d > r - 1)
					a = (int)((r - d) * 255);
			}
			blend(x + i, y + j, c, a);
		}
}

static void circle(float cx, float cy, float r, uint32_t c)
{
	for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
		for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
			float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));

			if (d < r + 0.5f)
				blend(x, y, c, d < r - 0.5f ? 255 : (int)((r + 0.5f - d) * 255));
		}
}

static void switch_widget(int x, int y, int on)
{
	rrect(x, y, 64, 36, 18, on ? ACCENT : SURFACE_HI);
	if (!on)
		rrect(x + 2, y + 2, 60, 32, 16, RGB(0x3A, 0x3F, 0x4C));
	circle(on ? x + 46 : x + 18, y + 18, on ? 13 : 10, on ? ACCENT_DARK : TEXT_DIM);
}

/* ---------- text ---------- */

static FT_Library ft;
static FT_Face face;
static int have_font = -1;   /* not tried yet */

struct glyph {
	int size;
	uint32_t cp;
	int w, h, left, top, adv;
	unsigned char *bits;
};
#define GLYPHS 512
static struct glyph glyphs[GLYPHS];

static int font(void)
{
	PlFontData data;

	if (have_font < 0)
		have_font = R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
			    R_SUCCEEDED(plGetSharedFontByType(&data, PlSharedFontType_Standard)) &&
			    !FT_Init_FreeType(&ft) && !FT_New_Memory_Face(ft, data.address, data.size, 0, &face);
	return have_font;
}

static struct glyph *glyph(uint32_t cp, int size)
{
	unsigned h = (cp * 31 + size * 131) % GLYPHS;

	if (!font())
		return NULL;
	for (int i = 0; i < GLYPHS; i++, h = (h + 1) % GLYPHS) {
		struct glyph *g = &glyphs[h];

		if (g->size == size && g->cp == cp)
			return g;
		if (!g->size) {
			FT_Set_Pixel_Sizes(face, 0, size);
			if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
				return NULL;
			g->bits = malloc(face->glyph->bitmap.width * face->glyph->bitmap.rows + 1);
			if (!g->bits)
				return NULL;
			g->size = size;
			g->cp = cp;
			g->w = face->glyph->bitmap.width;
			g->h = face->glyph->bitmap.rows;
			g->left = face->glyph->bitmap_left;
			g->top = face->glyph->bitmap_top;
			g->adv = face->glyph->advance.x >> 6;
			for (int y = 0; y < g->h; y++)
				memcpy(g->bits + y * g->w, face->glyph->bitmap.buffer + y * face->glyph->bitmap.pitch,
				       g->w);
			return g;
		}
	}
	return NULL;
}

/* The next character of UTF-8 (file names have any). */
static uint32_t next_cp(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t cp = *p++;

	if (cp >= 0xF0 && p[0] && p[1] && p[2]) {
		cp = ((cp & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
		p += 3;
	} else if (cp >= 0xE0 && p[0] && p[1]) {
		cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
		p += 2;
	} else if (cp >= 0xC0 && p[0]) {
		cp = ((cp & 31) << 6) | (p[0] & 63);
		p++;
	}
	*s = (const char *)p;
	return cp;
}

static int text_width(const char *s, int size)
{
	int w = 0;

	while (*s) {
		struct glyph *g = glyph(next_cp(&s), size);

		if (g)
			w += g->adv;
	}
	return w;
}

enum { LEFT, CENTER, RIGHT };

/* y is the baseline. */
static void text(int x, int y, int size, uint32_t c, int align, const char *s)
{
	if (align != LEFT)
		x -= align == CENTER ? text_width(s, size) / 2 : text_width(s, size);
	while (*s) {
		struct glyph *g = glyph(next_cp(&s), size);

		if (!g)
			continue;
		for (int j = 0; j < g->h; j++)
			for (int i = 0; i < g->w; i++)
				blend(x + g->left + i, y - g->top + j, c, g->bits[j * g->w + i]);
		x += g->adv;
	}
}

/* ---------- the panel ---------- */

enum { R_FULL_SCREEN, R_SCREEN, R_CONTROLS, R_KEYBOARD, R_CLIPBOARD, R_QUIT, ROWS };

#define PANEL_W (W < 900 ? W - 40 : 1000)
#define TITLE_H 80
#define ROW_H 66
#define HELP_COLUMNS (W < 900 ? 2 : 4)
#define HELP_LINE 25
#define FOOT_H 60

static int open_now, focus, full_screen, mode;
static int clip_focus;   /* the Clipboard row's button: 0 Copy, 1 Paste */
static int quit_focus = 1;   /* the last row's: 0 the launcher, 1 Quit */
static int bindings[GX_INPUTS];

static const char *const controller_help[] = {
	"Pages see the pad as a game controller, Xbox style:",
	"B, A, Y and X are its A, B, X and Y.",
	"Hold + to open this menu (a short press is Start).",
};
#define CONTROLLER_LINES 3

/* The key and mouse help: the right stick first, then what can be bound. */
#define HELP_ITEMS (GX_INPUTS + 1)

static int help_h(void)
{
	if (mode == OVERLAY_KEYBOARD_MOUSE)
		return 50 + (HELP_ITEMS + HELP_COLUMNS - 1) / HELP_COLUMNS * HELP_LINE;
	if (mode == OVERLAY_CONTROLLER)
		return 50 + CONTROLLER_LINES * HELP_LINE;
	return 0;
}

static int panel_h(void)
{
	return TITLE_H + ROWS * ROW_H + help_h() + FOOT_H;
}

static int panel_x(void)
{
	return (W - PANEL_W) / 2;
}

static int panel_y(void)
{
	return (H - panel_h()) / 2;
}

static int row_y(int i)
{
	return panel_y() + TITLE_H + i * ROW_H;
}

static void row(int i, const char *title, const char *sub)
{
	int x = panel_x(), y = row_y(i);

	if (focus == i)
		rrect(x + 16, y, PANEL_W - 32, ROW_H - 8, 18, SURFACE_HI);
	text(x + 44, y + 29, 25, TEXT, LEFT, title);
	text(x + 44, y + 51, 18, TEXT_DIM, LEFT, sub);
}

static void draw(void)
{
	int x = panel_x(), y = panel_y(), h = panel_h();

	pixels = buffers[shown == buffers[0]];
	/* The page under it, dimmed: black at 60%. */
	for (int i = 0; i < PIXELS; i++)
		pixels[i] = 0x99000000u;
	rrect(x, y, PANEL_W, h, 28, SURFACE);
	text(x + 44, y + 54, 30, TEXT, LEFT, "Chromium NX");

	row(R_FULL_SCREEN, "Full screen", full_screen ? "The page fills the screen" : "With the address bar");
	switch_widget(x + PANEL_W - 44 - 64, row_y(R_FULL_SCREEN) + 11, full_screen);
	row(R_SCREEN, "Screen", wl_portrait() ? "Hold the console turned, its left side up" : "As the console is held");
	text(x + PANEL_W - 44, row_y(R_SCREEN) + 38, 22, ACCENT, RIGHT, wl_portrait() ? "< Portrait >" : "< Landscape >");
	static const char *const mode_text[][2] = {
		{ "Touch, and the pad for links", "< Browser >" },
		{ "The pad is a keyboard and a mouse", "< Keyboard & mouse >" },
		{ "The pad is a game controller for the page", "< Controller >" },
	};

	row(R_CONTROLS, "Controls", mode_text[mode][0]);
	text(x + PANEL_W - 44, row_y(R_CONTROLS) + 38, 22, ACCENT, RIGHT, mode_text[mode][1]);
	row(R_KEYBOARD, "Keyboard", "Type text into the page");
	/* Shorter when the panel is narrow (portrait): its buttons take room. */
	row(R_CLIPBOARD, "Clipboard",
	    W < 900 ? "Copy, or paste at the cursor" : "Copy what is selected, paste where the text cursor is");
	for (int i = 0; i < 2; i++) {
		int bx = x + PANEL_W - 44 - (2 - i) * 120 + 10, by = row_y(R_CLIPBOARD) + 12;
		int on = focus == R_CLIPBOARD && clip_focus == i;

		rrect(bx, by, 110, 36, 18, on ? ACCENT : BUTTON);
		text(bx + 55, by + 25, 20, on ? ACCENT_DARK : TEXT, CENTER, i ? "Paste" : "Copy");
	}
	row(R_QUIT, "Leave", W < 900 ? "To the launcher, or close" : "Back to the launcher, or close Chromium NX");
	for (int i = 0; i < 2; i++) {
		int bx = x + PANEL_W - 44 - (2 - i) * 120 + 10, by = row_y(R_QUIT) + 12;
		int on = focus == R_QUIT && quit_focus == i;

		rrect(bx, by, 110, 36, 18, on ? ACCENT : BUTTON);
		text(bx + 55, by + 25, 20, on ? ACCENT_DARK : TEXT, CENTER, i ? "Quit" : "Launcher");
	}

	if (mode == OVERLAY_KEYBOARD_MOUSE) {
		int gy = row_y(ROWS) + 24;

		text(x + 44, gy, 20, ACCENT, LEFT, W < 900 ? "Keyboard & mouse (launcher's settings)" :
						 "Keyboard & mouse (change them in the launcher's settings)");
		for (int i = 0; i < HELP_ITEMS; i++) {
			int cx = x + 40 + i % HELP_COLUMNS * (W < 900 ? 300 : 236);
			int cy = gy + 34 + i / HELP_COLUMNS * HELP_LINE;

			text(cx, cy, 16, TEXT_DIM, LEFT, i ? gx_inputs[i - 1].name : "R stick");
			text(cx + 110, cy, 16, TEXT, LEFT, i ? gx_key_name(bindings[i - 1]) : "Mouse");
		}
	} else if (mode == OVERLAY_CONTROLLER) {
		int gy = row_y(ROWS) + 24;

		text(x + 44, gy, 20, ACCENT, LEFT, "Controller");
		for (int i = 0; i < CONTROLLER_LINES; i++)
			text(x + 44, gy + 34 + i * HELP_LINE, 18, TEXT_DIM, LEFT, controller_help[i]);
	}
	text(x + PANEL_W / 2, y + h - 24, 18, TEXT_DIM, CENTER, "A  Select        B / +  Close");

	/* Handed over: the other buffer is the next to draw in. */
	__atomic_store_n(&shown, pixels, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&serial, 1, __ATOMIC_SEQ_CST);
}

void overlay_open(int now_full_screen)
{
	if (!buffers[0]) {
		buffers[0] = malloc(PIXELS * 4);
		buffers[1] = malloc(PIXELS * 4);
		if (!buffers[0] || !buffers[1]) {
			free(buffers[0]);
			free(buffers[1]);
			buffers[0] = buffers[1] = NULL;
			return;
		}
	}
	open_now = 1;
	focus = 0;
	full_screen = now_full_screen;
	draw();
}

static int picker_open;

void overlay_close(void)
{
	open_now = 0;
	picker_open = 0;
	__atomic_store_n(&shown, NULL, __ATOMIC_SEQ_CST);
}

int overlay_is_open(void)
{
	return open_now || picker_open;
}

int overlay_mode(void)
{
	return mode;
}

void overlay_set_mode(int m)
{
	if (m >= OVERLAY_BROWSER && m <= OVERLAY_CONTROLLER)
		mode = m;
}

void overlay_bindings(const int *keys)
{
	memcpy(bindings, keys, sizeof(bindings));
}

void overlay_full_screen(int now)
{
	if (open_now && !!now != full_screen) {
		full_screen = !!now;
		draw();
	}
}

/* step: which way the Controls row turns (left -1, else 1). */
static int choose(int i, int step)
{
	switch (i) {
	case R_FULL_SCREEN:
		return OVERLAY_FULL_SCREEN;
	case R_SCREEN:
		return OVERLAY_ORIENTATION;
	case R_CLIPBOARD:
		return clip_focus ? OVERLAY_PASTE : OVERLAY_COPY;
	case R_CONTROLS:
		mode = (mode + 3 + step) % 3;
		draw();
		return OVERLAY_NONE;
	case R_KEYBOARD:
		return OVERLAY_KEYBOARD;
	case R_QUIT:
		return quit_focus ? OVERLAY_QUIT : OVERLAY_LAUNCHER;
	}
	return OVERLAY_NONE;
}

/* ---------- the file picker ---------- */

#define MAX_ENTRIES 512
#define PICK_ROW 56

static struct entry {
	char name[256];
	int folder;
	long long size;
} entries[MAX_ENTRIES];
static int entry_count, pick_focus, pick_scroll;
static char pick_dir[768] = "sdmc:/switch/chromium-nx/Downloads", picked[1024];

static int at_root(void)
{
	return !strcmp(pick_dir, "sdmc:/");
}

static int entry_order(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (x->folder != y->folder)
		return y->folder - x->folder;
	return strcasecmp(x->name, y->name);
}

/* The folder's entries: ".." first unless at the root, folders, then files. */
static int load_entries(void)
{
	DIR *d = opendir(pick_dir);
	struct dirent *e;

	if (!d)
		return -1;
	entry_count = 0;
	if (!at_root()) {
		strcpy(entries[0].name, "..");
		entries[0].folder = 1;
		entry_count = 1;
	}
	while ((e = readdir(d)) && entry_count < MAX_ENTRIES) {
		char path[1100];
		struct stat st;
		struct entry *n = &entries[entry_count];

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s%s%s", pick_dir, at_root() ? "" : "/", e->d_name);
		if (stat(path, &st))
			continue;
		snprintf(n->name, sizeof(n->name), "%s", e->d_name);
		n->folder = S_ISDIR(st.st_mode);
		n->size = st.st_size;
		entry_count++;
	}
	closedir(d);
	qsort(entries + !at_root(), entry_count - !at_root(), sizeof(entries[0]), entry_order);
	pick_focus = pick_scroll = 0;
	return 0;
}

static int picker_rows(void)
{
	return (H - 80 - 110 - 70) / PICK_ROW;
}

static const char *fit(const char *s, int size, int width)
{
	static char out[300];
	size_t n = strlen(s);

	snprintf(out, sizeof(out), "%s", s);
	while (n > 1 && text_width(out, size) > width) {
		while (n > 1 && ((unsigned char)s[n - 1] & 0xC0) == 0x80)
			n--;
		n--;
		snprintf(out, sizeof(out), "%.*s...", (int)n, s);
	}
	return out;
}

static void draw_picker(void)
{
	int x = 40, y = 40, w = W - 80, h = H - 80, rows = picker_rows();
	char size[32];

	pixels = buffers[shown == buffers[0]];
	for (int i = 0; i < PIXELS; i++)
		pixels[i] = 0x99000000u;
	rrect(x, y, w, h, 28, SURFACE);
	text(x + 36, y + 52, 30, TEXT, LEFT, "Choose a file");
	text(x + 36, y + 86, 18, TEXT_DIM, LEFT, fit(pick_dir + 5, 18, w - 72));
	if (pick_focus < pick_scroll)
		pick_scroll = pick_focus;
	if (pick_focus >= pick_scroll + rows)
		pick_scroll = pick_focus - rows + 1;
	if (!entry_count)
		text(x + w / 2, y + 200, 22, TEXT_DIM, CENTER, "This folder is empty");
	for (int i = pick_scroll; i < entry_count && i < pick_scroll + rows; i++) {
		int ry = y + 110 + (i - pick_scroll) * PICK_ROW;
		const struct entry *e = &entries[i];

		if (i == pick_focus)
			rrect(x + 16, ry, w - 32, PICK_ROW - 6, 14, SURFACE_HI);
		/* A folder: a tab and a body; a file: a page. */
		if (e->folder) {
			rrect(x + 36, ry + 15, 14, 8, 3, RGB(0xF2, 0xC9, 0x4C));
			rrect(x + 36, ry + 19, 28, 20, 4, RGB(0xF2, 0xC9, 0x4C));
		} else {
			rrect(x + 40, ry + 12, 20, 28, 3, TEXT_DIM);
		}
		text(x + 80, ry + 34, 22, TEXT, LEFT,
		     fit(strcmp(e->name, "..") ? e->name : "Up a folder", 22, w - (e->folder ? 140 : 260)));
		if (!e->folder) {
			if (e->size < 1024)
				snprintf(size, sizeof(size), "%lld B", e->size);
			else if (e->size < (1LL << 20))
				snprintf(size, sizeof(size), "%.0f KB", e->size / 1024.0);
			else
				snprintf(size, sizeof(size), "%.1f MB", e->size / 1048576.0);
			text(x + w - 40, ry + 34, 18, TEXT_DIM, RIGHT, size);
		}
	}
	text(x + w / 2, y + h - 24, 18, TEXT_DIM, CENTER, "A  Open        B  Up a folder        +  Cancel");
	__atomic_store_n(&shown, pixels, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&serial, 1, __ATOMIC_SEQ_CST);
}

void overlay_picker_open(void)
{
	if (!buffers[0]) {
		buffers[0] = malloc(PIXELS * 4);
		buffers[1] = malloc(PIXELS * 4);
		if (!buffers[0] || !buffers[1]) {
			free(buffers[0]);
			free(buffers[1]);
			buffers[0] = buffers[1] = NULL;
			return;
		}
	}
	open_now = 0;
	if (load_entries()) {
		strcpy(pick_dir, "sdmc:/");
		load_entries();
	}
	picker_open = 1;
	draw_picker();
}

const char *overlay_picked(void)
{
	return picked;
}

static void picker_up(void)
{
	char *slash = strrchr(pick_dir, '/'), came_from[256];

	if (at_root() || !slash)
		return;
	snprintf(came_from, sizeof(came_from), "%s", slash + 1);
	if (slash - pick_dir <= 5)   /* "sdmc:/x" goes to "sdmc:/" */
		slash[1] = 0;
	else
		*slash = 0;
	load_entries();
	for (int i = 0; i < entry_count; i++)
		if (!strcmp(entries[i].name, came_from))
			pick_focus = i;
}

static int picker_choose(int i)
{
	const struct entry *e;

	if (i < 0 || i >= entry_count)
		return OVERLAY_NONE;
	e = &entries[i];
	if (!strcmp(e->name, "..")) {
		picker_up();
	} else if (e->folder) {
		char was[768];

		snprintf(was, sizeof(was), "%s", pick_dir);
		snprintf(pick_dir + strlen(pick_dir), sizeof(pick_dir) - strlen(pick_dir), "%s%s",
			 at_root() ? "" : "/", e->name);
		if (load_entries()) {
			strcpy(pick_dir, was);
			load_entries();
		}
	} else {
		snprintf(picked, sizeof(picked), "%s%s%s", pick_dir, at_root() ? "" : "/", e->name);
		picker_open = 0;
		__atomic_store_n(&shown, NULL, __ATOMIC_SEQ_CST);
		return OVERLAY_PICKED;
	}
	draw_picker();
	return OVERLAY_NONE;
}

static int picker_pad(uint64_t down)
{
	int rows = picker_rows();

	if (down & HidNpadButton_Plus)
		return OVERLAY_CLOSE;
	if (down & HidNpadButton_A)
		return picker_choose(pick_focus);
	if (down & HidNpadButton_B) {
		if (at_root())
			return OVERLAY_CLOSE;
		picker_up();
	} else if (down & (HidNpadButton_AnyUp | HidNpadButton_StickRUp)) {
		pick_focus = pick_focus > 0 ? pick_focus - 1 : 0;
	} else if (down & (HidNpadButton_AnyDown | HidNpadButton_StickRDown)) {
		pick_focus = pick_focus + 1 < entry_count ? pick_focus + 1 : pick_focus;
	} else if (down & (HidNpadButton_L | HidNpadButton_AnyLeft)) {
		pick_focus = pick_focus > rows ? pick_focus - rows : 0;
	} else if (down & (HidNpadButton_R | HidNpadButton_AnyRight)) {
		pick_focus = pick_focus + rows < entry_count ? pick_focus + rows : (entry_count ? entry_count - 1 : 0);
	} else {
		return OVERLAY_NONE;
	}
	draw_picker();
	return OVERLAY_NONE;
}

static int picker_touch(int x, int y)
{
	int rows = picker_rows(), top = 40 + 110;

	if (y >= top && y < top + rows * PICK_ROW && x >= 40 && x < W - 40) {
		int i = pick_scroll + (y - top) / PICK_ROW;

		if (i < entry_count) {
			pick_focus = i;
			return picker_choose(i);
		}
	}
	return OVERLAY_NONE;
}

int overlay_pad(uint64_t down)
{
	if (picker_open)
		return picker_pad(down);
	if (!open_now)
		return OVERLAY_NONE;
	if (down & (HidNpadButton_B | HidNpadButton_Plus))
		return OVERLAY_CLOSE;
	if (down & HidNpadButton_A)
		return choose(focus, 1);
	if (down & (HidNpadButton_AnyUp | HidNpadButton_StickRUp)) {
		focus = (focus + ROWS - 1) % ROWS;
		draw();
	} else if (down & (HidNpadButton_AnyDown | HidNpadButton_StickRDown)) {
		focus = (focus + 1) % ROWS;
		draw();
	} else if ((down & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) && focus == R_CLIPBOARD) {
		clip_focus = !!(down & HidNpadButton_AnyRight);
		draw();
	} else if ((down & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) && focus == R_QUIT) {
		quit_focus = !!(down & HidNpadButton_AnyRight);
		draw();
	} else if ((down & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) && focus == R_SCREEN) {
		return OVERLAY_ORIENTATION;
	} else if ((down & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) && focus == R_CONTROLS) {
		return choose(R_CONTROLS, (down & HidNpadButton_AnyLeft) ? -1 : 1);
	}
	return OVERLAY_NONE;
}

int overlay_touch(int x, int y)
{
	if (picker_open)
		return picker_touch(x, y);
	if (!open_now)
		return OVERLAY_NONE;
	if (x < panel_x() || x >= panel_x() + PANEL_W || y < panel_y() || y >= panel_y() + panel_h())
		return OVERLAY_CLOSE;
	for (int i = 0; i < ROWS; i++)
		if (y >= row_y(i) && y < row_y(i) + ROW_H) {
			focus = i;
			if (i == R_CLIPBOARD || i == R_QUIT) {
				/* Their two buttons; the rest of the row does nothing. */
				int first = panel_x() + PANEL_W - 44 - 240 + 10;

				if (x < first) {
					draw();
					return OVERLAY_NONE;
				}
				if (i == R_CLIPBOARD)
					clip_focus = x >= first + 120;
				else
					quit_focus = x >= first + 120;
			}
			draw();
			return choose(i, 1);
		}
	return OVERLAY_NONE;
}

/* An arrow, white with a black edge, drawn once. */
#define CURSOR_W 20
#define CURSOR_H 30

const uint32_t *overlay_cursor(int *width, int *height)
{
	static uint32_t arrow[CURSOR_W * CURSOR_H];
	static int made;
	static const float pts[][2] = { { 1, 1 }, { 1, 24 }, { 6.5f, 19 }, { 10.5f, 28 }, { 14, 26.5f },
					{ 10, 17.5f }, { 17, 17.5f } };
	const int n = sizeof(pts) / sizeof(pts[0]);

	if (!made) {
		for (int y = 0; y < CURSOR_H; y++)
			for (int x = 0; x < CURSOR_W; x++) {
				float px = x + 0.5f, py = y + 0.5f, d = 1e9f;
				int in = 0;

				for (int i = 0, j = n - 1; i < n; j = i++) {
					float ax = pts[j][0], ay = pts[j][1], bx = pts[i][0], by = pts[i][1];
					float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
					float k = fmaxf(0, fminf(1, ((px - ax) * vx + (py - ay) * vy) / len2));
					float dx = px - ax - k * vx, dy = py - ay - k * vy;

					d = fminf(d, sqrtf(dx * dx + dy * dy));
					if ((ay > py) != (by > py) && px < ax + (py - ay) * vx / vy)
						in = !in;
				}
				if (in)
					arrow[y * CURSOR_W + x] = d < 1.3f ? 0xFF000000u : 0xFFFFFFFFu;
				else if (d < 1.0f)
					arrow[y * CURSOR_W + x] = (uint32_t)((1.0f - d) * 255) << 24;
			}
		made = 1;
	}
	*width = CURSOR_W;
	*height = CURSOR_H;
	return arrow;
}

const uint32_t *overlay_picture(int *x, int *y, int *width, int *height, unsigned *serial_out)
{
	const uint32_t *picture = __atomic_load_n(&shown, __ATOMIC_SEQ_CST);

	*x = *y = 0;
	*width = W;
	*height = H;
	*serial_out = __atomic_load_n(&serial, __ATOMIC_SEQ_CST);
	return picture;
}
