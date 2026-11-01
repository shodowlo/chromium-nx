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
#include "overlay.h"
#include "keys.h"

#define W 1280
#define H 720

/* RGBA in memory, R first; colours multiplied by their opacity. */
#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

static const uint32_t SURFACE = RGB(0x24, 0x28, 0x33), SURFACE_HI = RGB(0x2E, 0x33, 0x40);
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

static int text_width(const char *s, int size)
{
	int w = 0;

	for (; *s; s++) {
		struct glyph *g = glyph((unsigned char)*s, size);

		if (g)
			w += g->adv;
	}
	return w;
}

enum { LEFT, CENTER, RIGHT };

/* y is the baseline. Latin-1 only, which is all there is here. */
static void text(int x, int y, int size, uint32_t c, int align, const char *s)
{
	if (align != LEFT)
		x -= align == CENTER ? text_width(s, size) / 2 : text_width(s, size);
	for (; *s; s++) {
		struct glyph *g = glyph((unsigned char)*s, size);

		if (!g)
			continue;
		for (int j = 0; j < g->h; j++)
			for (int i = 0; i < g->w; i++)
				blend(x + g->left + i, y - g->top + j, c, g->bits[j * g->w + i]);
		x += g->adv;
	}
}

/* ---------- the panel ---------- */

enum { R_FULL_SCREEN, R_CONTROLS, R_KEYBOARD, R_QUIT, ROWS };

#define PANEL_W 780
#define TITLE_H 80
#define ROW_H 78
#define HELP_COLUMNS 3
#define HELP_LINE 27
#define FOOT_H 60

static int open_now, focus, full_screen, mode;
static int bindings[GX_INPUTS];

static const char *const controller_help[] = {
	"Pages see the pad as a game controller, Xbox style: B, A, Y and X",
	"are its A, B, X and Y. Hold + to open this menu (a short press is Start).",
};

/* The key and mouse help: the right stick first, then what can be bound. */
#define HELP_ITEMS (GX_INPUTS + 1)

static int help_h(void)
{
	if (mode == OVERLAY_KEYBOARD_MOUSE)
		return 50 + (HELP_ITEMS + HELP_COLUMNS - 1) / HELP_COLUMNS * HELP_LINE;
	if (mode == OVERLAY_CONTROLLER)
		return 50 + 2 * HELP_LINE;
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
	text(x + 44, y + 32, 25, TEXT, LEFT, title);
	text(x + 44, y + 58, 18, TEXT_DIM, LEFT, sub);
}

static void draw(void)
{
	int x = panel_x(), y = panel_y(), h = panel_h();

	pixels = buffers[shown == buffers[0]];
	/* The page under it, dimmed: black at 60%. */
	for (int i = 0; i < W * H; i++)
		pixels[i] = 0x99000000u;
	rrect(x, y, PANEL_W, h, 28, SURFACE);
	text(x + 44, y + 54, 30, TEXT, LEFT, "Chromium NX");

	row(R_FULL_SCREEN, "Full screen", full_screen ? "The page fills the screen" : "With the address bar");
	switch_widget(x + PANEL_W - 44 - 64, row_y(R_FULL_SCREEN) + 17, full_screen);
	static const char *const mode_text[][2] = {
		{ "Touch, and the pad for links", "< Browser >" },
		{ "The pad is a keyboard and a mouse", "< Keyboard & mouse >" },
		{ "The pad is a game controller for the page", "< Controller >" },
	};

	row(R_CONTROLS, "Controls", mode_text[mode][0]);
	text(x + PANEL_W - 44, row_y(R_CONTROLS) + 44, 22, ACCENT, RIGHT, mode_text[mode][1]);
	row(R_KEYBOARD, "Keyboard", "Type text into the page");
	row(R_QUIT, "Quit", "Close Chromium NX");

	if (mode == OVERLAY_KEYBOARD_MOUSE) {
		int gy = row_y(ROWS) + 24;

		text(x + 44, gy, 20, ACCENT, LEFT, "Keyboard & mouse (change them in the launcher's settings)");
		for (int i = 0; i < HELP_ITEMS; i++) {
			int cx = x + 40 + i % HELP_COLUMNS * 240, cy = gy + 34 + i / HELP_COLUMNS * HELP_LINE;

			text(cx, cy, 17, TEXT_DIM, LEFT, i ? gx_inputs[i - 1].name : "R stick");
			text(cx + 122, cy, 17, TEXT, LEFT, i ? gx_key_name(bindings[i - 1]) : "Mouse");
		}
	} else if (mode == OVERLAY_CONTROLLER) {
		int gy = row_y(ROWS) + 24;

		text(x + 44, gy, 20, ACCENT, LEFT, "Controller");
		for (int i = 0; i < 2; i++)
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
		buffers[0] = malloc(W * H * 4);
		buffers[1] = malloc(W * H * 4);
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

void overlay_close(void)
{
	open_now = 0;
	__atomic_store_n(&shown, NULL, __ATOMIC_SEQ_CST);
}

int overlay_is_open(void)
{
	return open_now;
}

int overlay_mode(void)
{
	return mode;
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
	case R_CONTROLS:
		mode = (mode + 3 + step) % 3;
		draw();
		return OVERLAY_NONE;
	case R_KEYBOARD:
		return OVERLAY_KEYBOARD;
	case R_QUIT:
		return OVERLAY_QUIT;
	}
	return OVERLAY_NONE;
}

int overlay_pad(uint64_t down)
{
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
	} else if ((down & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) && focus == R_CONTROLS) {
		return choose(R_CONTROLS, (down & HidNpadButton_AnyLeft) ? -1 : 1);
	}
	return OVERLAY_NONE;
}

int overlay_touch(int x, int y)
{
	if (!open_now)
		return OVERLAY_NONE;
	if (x < panel_x() || x >= panel_x() + PANEL_W || y < panel_y() || y >= panel_y() + panel_h())
		return OVERLAY_CLOSE;
	for (int i = 0; i < ROWS; i++)
		if (y >= row_y(i) && y < row_y(i) + ROW_H) {
			focus = i;
			draw();
			return choose(i, 1);
		}
	return OVERLAY_NONE;
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
