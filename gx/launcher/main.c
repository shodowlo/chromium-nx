/*
 * The launcher: what the Chromium NX tile opens. A home screen in the manner
 * of Android's (a clock, a search bar, a row of apps) and the settings; what
 * is chosen becomes the browser's command line, written to launch.txt, and
 * the Homebrew loader is asked to load this same program again, as the
 * browser (--browser, host_nx.c), once the launcher has ended. Nothing of the
 * launcher is left when the browser starts: the program is loaded into the
 * process afresh, with all of its memory.
 *
 * It also looks for new versions when it starts (update.c).
 *
 * Drawn by the processor into the screen's own buffers, with the console's
 * system font (FreeType) and the app's icon (libjpeg-turbo).
 */
#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <turbojpeg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../keys.h"
#include "../version.h"
#include "update.h"

#define W 1280
#define H 720
#define DIR "sdmc:/switch/chromium-nx"
#define SELF DIR "/" UPDATE_PROGRAM

/* The browser's command line, as gx has it (host_nx.c). */
const char *gx_default_command(void);

extern const unsigned char icon_jpg[];
extern const unsigned int icon_jpg_len;

/* ---------- drawing ---------- */

static u32 *fbuf;
static u32 fstride;   /* in pixels */

/* RGBA8888 in memory: R first. */
#define RGB(r, g, b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))
#define C_R(c) ((c) & 0xFF)
#define C_G(c) (((c) >> 8) & 0xFF)
#define C_B(c) (((c) >> 16) & 0xFF)

static const u32 BG_TOP = RGB(0x1A, 0x1D, 0x26), BG_BOTTOM = RGB(0x0E, 0x10, 0x15);
static const u32 SURFACE = RGB(0x24, 0x28, 0x33), SURFACE_HI = RGB(0x2E, 0x33, 0x40);
static const u32 TEXT = RGB(0xE8, 0xEA, 0xF0), TEXT_DIM = RGB(0x9A, 0xA0, 0xAE);
static const u32 ACCENT = RGB(0x8A, 0xB4, 0xF8), ACCENT_DARK = RGB(0x1F, 0x3B, 0x63);

static inline void blend(int x, int y, u32 c, int a)
{
	u32 *d;

	if (x < 0 || y < 0 || x >= W || y >= H || a <= 0)
		return;
	d = &fbuf[y * fstride + x];
	if (a >= 255) {
		*d = c;
		return;
	}
	*d = RGB((C_R(c) * a + C_R(*d) * (255 - a)) / 255, (C_G(c) * a + C_G(*d) * (255 - a)) / 255,
		 (C_B(c) * a + C_B(*d) * (255 - a)) / 255);
}

static void background_band(int y0, int y1)
{
	for (int y = y0; y < y1; y++) {
		int t = y * 255 / (H - 1);
		u32 c = RGB((C_R(BG_TOP) * (255 - t) + C_R(BG_BOTTOM) * t) / 255,
			    (C_G(BG_TOP) * (255 - t) + C_G(BG_BOTTOM) * t) / 255,
			    (C_B(BG_TOP) * (255 - t) + C_B(BG_BOTTOM) * t) / 255);
		u32 *row = &fbuf[y * fstride];

		for (int x = 0; x < W; x++)
			row[x] = c;
	}
}

static void background(void)
{
	background_band(0, H);
}

/* A rectangle with round corners, edges smoothed. */
static void rrect(int x, int y, int w, int h, int r, u32 c)
{
	if (r * 2 > h)
		r = h / 2;
	if (r * 2 > w)
		r = w / 2;
	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			int dx = i < r ? r - i : i >= w - r ? i - (w - r - 1) : 0;
			int dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
			int a = 255;

			if (dx && dy) {
				float d = sqrtf((float)((dx - 0.5f) * (dx - 0.5f) + (dy - 0.5f) * (dy - 0.5f)));

				if (d > r)
					continue;
				if (d > r - 1)
					a = (int)((r - d) * 255);
			}
			blend(x + i, y + j, c, a);
		}
	}
}

static void circle(float cx, float cy, float r, u32 c)
{
	for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
		for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
			float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));

			if (d < r + 0.5f)
				blend(x, y, c, d < r - 0.5f ? 255 : (int)((r + 0.5f - d) * 255));
		}
}

/* A stroke from (x0,y0) to (x1,y1), t thick, round ends. */
static void line(float x0, float y0, float x1, float y1, float t, u32 c)
{
	float minx = fminf(x0, x1) - t, maxx = fmaxf(x0, x1) + t;
	float miny = fminf(y0, y1) - t, maxy = fmaxf(y0, y1) + t;
	float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;

	for (int y = (int)miny; y <= (int)maxy; y++)
		for (int x = (int)minx; x <= (int)maxx; x++) {
			float px = x + 0.5f - x0, py = y + 0.5f - y0;
			float k = len2 > 0 ? fmaxf(0, fminf(1, (px * vx + py * vy) / len2)) : 0;
			float dx = px - k * vx, dy = py - k * vy, d = sqrtf(dx * dx + dy * dy);

			if (d < t / 2 + 0.5f)
				blend(x, y, c, d < t / 2 - 0.5f ? 255 : (int)((t / 2 + 0.5f - d) * 255));
		}
}

/* ---------- text ---------- */

static FT_Library ft;
static FT_Face face;

struct glyph {
	int size;
	u32 cp;
	int w, h, left, top, adv;
	unsigned char *bits;
};
#define GLYPHS 1024
static struct glyph glyphs[GLYPHS];

static struct glyph *glyph(u32 cp, int size)
{
	unsigned h = (cp * 31 + size * 131) % GLYPHS;

	for (int i = 0; i < GLYPHS; i++, h = (h + 1) % GLYPHS) {
		struct glyph *g = &glyphs[h];

		if (g->size == size && g->cp == cp)
			return g;
		if (!g->size) {
			FT_Set_Pixel_Sizes(face, 0, size);
			if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
				return NULL;
			g->size = size;
			g->cp = cp;
			g->w = face->glyph->bitmap.width;
			g->h = face->glyph->bitmap.rows;
			g->left = face->glyph->bitmap_left;
			g->top = face->glyph->bitmap_top;
			g->adv = face->glyph->advance.x >> 6;
			g->bits = malloc(g->w * g->h + 1);
			for (int y = 0; y < g->h; y++)
				memcpy(g->bits + y * g->w, face->glyph->bitmap.buffer +
				       y * face->glyph->bitmap.pitch, g->w);
			return g;
		}
	}
	return NULL;
}

static u32 next_cp(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	u32 cp = *p++;

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
static int text(int x, int y, int size, u32 c, int align, const char *s)
{
	int w = text_width(s, size);

	if (align == CENTER)
		x -= w / 2;
	else if (align == RIGHT)
		x -= w;
	while (*s) {
		struct glyph *g = glyph(next_cp(&s), size);

		if (!g)
			continue;
		for (int j = 0; j < g->h; j++)
			for (int i = 0; i < g->w; i++)
				blend(x + g->left + i, y - g->top + j, c, g->bits[j * g->w + i]);
		x += g->adv;
	}
	return w;
}

/* ---------- the app's icon ---------- */

#define ICON 112
static u32 icon[ICON * ICON];
static int have_icon;

static void load_icon(void)
{
	tjhandle tj = tjInitDecompress();
	int w, h, sub, cs;
	unsigned char *rgb;

	if (!tj)
		return;
	if (!tjDecompressHeader3(tj, icon_jpg, icon_jpg_len, &w, &h, &sub, &cs) &&
	    (rgb = malloc(w * h * 3)) != NULL) {
		if (!tjDecompress2(tj, icon_jpg, icon_jpg_len, rgb, w, 0, h, TJPF_RGB, 0)) {
			/* Shrunk to ICON by averaging the pixels that fall in each. */
			for (int y = 0; y < ICON; y++)
				for (int x = 0; x < ICON; x++) {
					int x0 = x * w / ICON, x1 = (x + 1) * w / ICON;
					int y0 = y * h / ICON, y1 = (y + 1) * h / ICON;
					unsigned r = 0, g = 0, b = 0, n = 0;

					for (int yy = y0; yy < y1; yy++)
						for (int xx = x0; xx < x1; xx++, n++) {
							unsigned char *p = rgb + (yy * w + xx) * 3;

							r += p[0];
							g += p[1];
							b += p[2];
						}
					if (!n)
						n = 1;
					icon[y * ICON + x] = RGB(r / n, g / n, b / n);
				}
			have_icon = 1;
		}
		free(rgb);
	}
	tjDestroy(tj);
}

/* ---------- settings ---------- */

static struct {
	int diagnostics;     /* -p */
	int async_disk;      /* not -xd */
	int priorities;      /* not -xp */
	int keys[GX_INPUTS]; /* -K: the keyboard and mouse mode's, by input of keys.h */
	int check_updates;   /* update.c, when the launcher starts */
} settings = { 0, 1, 1, { 0 }, 1 };

static void load_settings(void)
{
	FILE *f = fopen(DIR "/settings.ini", "r");
	char line[600];

	for (int i = 0; i < GX_INPUTS; i++)
		settings.keys[i] = gx_inputs[i].default_key;
	while (f && fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '=');

		if (!v)
			continue;
		*v++ = 0;
		v[strcspn(v, "\r\n")] = 0;
		if (!strcmp(line, "diagnostics"))
			settings.diagnostics = !!atoi(v);
		else if (!strcmp(line, "async_disk"))
			settings.async_disk = !!atoi(v);
		else if (!strcmp(line, "priorities"))
			settings.priorities = !!atoi(v);
		else if (!strcmp(line, "check_updates"))
			settings.check_updates = !!atoi(v);
		else if (!strncmp(line, "key_", 4))
			for (int i = 0; i < GX_INPUTS; i++)
				if (!strcmp(line + 4, gx_inputs[i].setting))
					settings.keys[i] = (int)strtol(v, NULL, 0);
	}
	if (f)
		fclose(f);
}

static void save_settings(void)
{
	FILE *f = fopen(DIR "/settings.ini", "w");

	if (!f)
		return;
	fprintf(f, "diagnostics=%d\nasync_disk=%d\npriorities=%d\ncheck_updates=%d\n",
		settings.diagnostics, settings.async_disk, settings.priorities, settings.check_updates);
	for (int i = 0; i < GX_INPUTS; i++)
		fprintf(f, "key_%s=%d\n", gx_inputs[i].setting, settings.keys[i]);
	fclose(f);
}

/* ---------- the system keyboard ---------- */

static int ask(const char *header, const char *hint, const char *initial, char *out, size_t len)
{
	SwkbdConfig kbd;
	Result rc;

	if (R_FAILED(swkbdCreate(&kbd, 0)))
		return 0;
	swkbdConfigMakePresetDefault(&kbd);
	swkbdConfigSetType(&kbd, SwkbdType_Normal);
	swkbdConfigSetHeaderText(&kbd, header);
	swkbdConfigSetGuideText(&kbd, hint);
	if (initial)
		swkbdConfigSetInitialText(&kbd, initial);
	rc = swkbdShow(&kbd, out, len);
	swkbdClose(&kbd);
	return R_SUCCEEDED(rc) && out[0];
}

/* What was typed in the search bar: an address, or words to search for. */
static void to_url(const char *in, char *out, size_t len)
{
	int address = !strchr(in, ' ') && strchr(in, '.') && in[strlen(in) - 1] != '.';

	if (address) {
		snprintf(out, len, "%s%s", strstr(in, "://") ? "" : "https://", in);
		return;
	}
	size_t n = snprintf(out, len, "https://www.google.com/search?q=");

	for (const unsigned char *p = (const unsigned char *)in; *p && n + 4 < len; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		    strchr("-_.~", *p))
			out[n++] = *p;
		else if (*p == ' ')
			out[n++] = '+';
		else
			n += snprintf(out + n, len - n, "%%%02X", *p);
	}
	out[n] = 0;
}

/* ---------- launching ---------- */

static char launch_error[160];

/* This program again, as the browser: what launch.txt says is run. */
static int start_browser(void)
{
	if (R_FAILED(envSetNextLoad(SELF, SELF " --browser"))) {
		remove(DIR "/launch.txt");
		snprintf(launch_error, sizeof(launch_error), "the loader cannot start the browser");
		return -1;
	}
	return 0;
}

/* The browser's command line is gx's own; the settings change what goes in
 * front of it and its last word, the page to open (none: Chromium's own start
 * page). */
static int launch(const char *url, int tv)
{
	static char cmd[4096], out[4600];
	char *p, *last, opts[256] = "";
	FILE *f;

	snprintf(cmd, sizeof(cmd), "%s", gx_default_command());
	p = cmd;
	last = strrchr(p, ' ');
	if (last && (!strncmp(last + 1, "http://", 7) || !strncmp(last + 1, "https://", 8)))
		*last = 0;
	if (settings.diagnostics)
		strcat(opts, "-p ");
	if (tv)
		strcat(opts, "-T ");   /* YouTube's TV page, full screen */
	for (int i = 0; i < GX_INPUTS; i++)
		if (settings.keys[i] != gx_inputs[i].default_key) {
			/* The keys, all of them, when one is not as gx has it. */
			strcat(opts, "-K");
			for (int j = 0; j < GX_INPUTS; j++)
				snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), "%s%d", j ? "," : "",
					 settings.keys[j]);
			strcat(opts, " ");
			break;
		}
	if (!settings.async_disk || !settings.priorities) {
		strcat(opts, "-x");
		if (!settings.async_disk)
			strcat(opts, "d");
		if (!settings.priorities)
			strcat(opts, "p");
		strcat(opts, " ");
	}
	snprintf(out, sizeof(out), "%s%s%s%s\n", opts, p, url[0] ? " " : "", url);
	f = fopen(DIR "/launch.txt", "w");
	if (!f || fputs(out, f) < 0) {
		if (f)
			fclose(f);
		snprintf(launch_error, sizeof(launch_error), "cannot write launch.txt");
		return -1;
	}
	fclose(f);
	return start_browser();
}

/* ---------- screens ---------- */

enum { HOME, SETTINGS, KEYS };
static int screen = HOME, focus = 1, dialog, dialog_focus;
/* The key picker, over KEYS: open, and the key it has the focus on (gx_keys). */
static int picking, pick_focus;
static char toast[120];
static u64 toast_until;

static void say(const char *s)
{
	snprintf(toast, sizeof(toast), "%s", s);
	toast_until = armGetSystemTick() + armNsToTicks(2500000000ULL);
}

static void status_bar(void)
{
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	char s[32];
	u32 pct = 0;
	PsmChargerType charger = PsmChargerType_Unconnected;

	/* The home screen has a clock of its own. */
	snprintf(s, sizeof(s), "%02d:%02d", tm->tm_hour, tm->tm_min);
	if (screen != HOME)
		text(32, 34, 22, TEXT, LEFT, s);
	psmGetBatteryChargePercentage(&pct);
	psmGetChargerType(&charger);
	snprintf(s, sizeof(s), "%u%%", pct);
	/* A battery: outline, level, tip. */
	int bx = W - 74, by = 17, bw = 34, bh = 18;

	rrect(bx, by, bw, bh, 4, TEXT_DIM);
	rrect(bx + 2, by + 2, bw - 4, bh - 4, 3, BG_TOP);
	rrect(bx + 3, by + 3, (bw - 6) * (int)pct / 100, bh - 6, 2,
	      pct <= 15 ? RGB(0xF2, 0x8B, 0x82) : charger != PsmChargerType_Unconnected ? RGB(0x81, 0xC9, 0x95) : TEXT);
	rrect(bx + bw, by + 5, 3, bh - 10, 1, TEXT_DIM);
	text(bx - 10, 34, 22, TEXT, RIGHT, s);
}

/* The places that can be touched or focused, by screen. */
struct spot { int x, y, w, h; };

#define APPS 5
static const char *app_names[APPS] = { "Chromium", "YouTube", "Google", "Chromium settings", "Settings" };

static struct spot home_spot(int i)
{
	if (i == 0)
		return (struct spot){ 240, 300, 800, 72 };
	int span = 200, x0 = W / 2 - APPS * span / 2;

	return (struct spot){ x0 + (i - 1) * span + 30, 440, 140, 170 };
}

static void gear(int cx, int cy, float s, u32 color, u32 hole)
{
	for (int k = 0; k < 8; k++) {
		float a = k * (float)M_PI / 4;

		line(cx + cosf(a) * 18 * s, cy + sinf(a) * 18 * s, cx + cosf(a) * 30 * s, cy + sinf(a) * 30 * s,
		     11 * s, color);
	}
	circle(cx, cy, 22 * s, color);
	circle(cx, cy, 9 * s, hole);
}

static void draw_app_icon(int i, int cx, int cy)
{
	switch (i) {
	case 3:   /* Chromium's settings: its icon, a gear on it */
		draw_app_icon(0, cx, cy);
		circle(cx + 32, cy + 32, 24, RGB(0x5F, 0x63, 0x68));
		gear(cx + 32, cy + 32, 0.6f, TEXT, RGB(0x5F, 0x63, 0x68));
		break;
	case 0:   /* Chromium: the app's own icon, round */
		if (have_icon) {
			for (int y = 0; y < ICON; y++)
				for (int x = 0; x < ICON; x++) {
					float d = sqrtf((x - ICON / 2 + 0.5f) * (x - ICON / 2 + 0.5f) +
							(y - ICON / 2 + 0.5f) * (y - ICON / 2 + 0.5f));
					float r = 50;

					if (d < r + 0.5f)
						blend(cx - ICON / 2 + x, cy - ICON / 2 + y, icon[y * ICON + x],
						      d < r - 0.5f ? 255 : (int)((r + 0.5f - d) * 255));
				}
		} else {
			circle(cx, cy, 50, ACCENT);
		}
		break;
	case 1:   /* YouTube: red, a white triangle */
		circle(cx, cy, 50, RGB(0xFF, 0x00, 0x00));
		/* The triangle, 32 wide and 36 high, pointing right. */
		for (int y = -18; y <= 18; y++)
			for (int x = -12; x <= 20; x++)
				if (fabsf((float)y) <= 18 * (1 - (x + 12) / 32.0f))
					blend(cx + x, cy + y, RGB(0xFF, 0xFF, 0xFF), 255);
		break;
	case 2:   /* Google: white, a blue G */
		circle(cx, cy, 50, RGB(0xFF, 0xFF, 0xFF));
		text(cx, cy + 20, 58, RGB(0x42, 0x85, 0xF4), CENTER, "G");
		break;
	case 4:   /* Settings: a gear */
		circle(cx, cy, 50, RGB(0x5F, 0x63, 0x68));
		gear(cx, cy, 1, TEXT, RGB(0x5F, 0x63, 0x68));
		break;
	}
}

static void draw_home(void)
{
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *months[] = { "January", "February", "March", "April", "May", "June", "July",
					"August", "September", "October", "November", "December" };
	char s[64];

	/* The clock, as Android's home screen has. */
	snprintf(s, sizeof(s), "%02d:%02d", tm->tm_hour, tm->tm_min);
	text(W / 2, 190, 110, TEXT, CENTER, s);
	snprintf(s, sizeof(s), "%s, %s %d", days[tm->tm_wday], months[tm->tm_mon], tm->tm_mday);
	text(W / 2, 240, 28, TEXT_DIM, CENTER, s);

	/* The search bar. */
	struct spot b = home_spot(0);

	if (focus == 0)
		rrect(b.x - 4, b.y - 4, b.w + 8, b.h + 8, (b.h + 8) / 2, ACCENT);
	rrect(b.x, b.y, b.w, b.h, b.h / 2, SURFACE_HI);
	circle(b.x + 44, b.y + 32, 12, TEXT_DIM);
	circle(b.x + 44, b.y + 32, 8, SURFACE_HI);
	line(b.x + 52, b.y + 40, b.x + 62, b.y + 50, 5, TEXT_DIM);
	text(b.x + 84, b.y + 46, 26, TEXT_DIM, LEFT, "Search or type a web address");

	/* The apps. */
	for (int i = 0; i < APPS; i++) {
		struct spot a = home_spot(i + 1);
		int cx = a.x + a.w / 2, cy = a.y + 60;

		if (focus == i + 1)
			rrect(a.x - 6, a.y - 6, a.w + 12, a.h + 12, 28, SURFACE_HI);
		draw_app_icon(i, cx, cy);
		int size = 24;

		while (size > 16 && text_width(app_names[i], size) > 190)
			size--;
		text(cx, a.y + 150, size, focus == i + 1 ? TEXT : TEXT_DIM, CENTER, app_names[i]);
	}
}

/* Settings: rows, some under a heading. */
enum { S_KEYS, S_UPDATES, S_CHECK, S_DIAG, S_DISK, S_PRIO, S_CLEAR, ROWS };

/* Where the rows are on the page of the settings, which is taller than the
 * screen: it scrolls so that the row with the focus is seen. */
static const int settings_top[ROWS] = { 156, 288, 368, 500, 580, 660, 792 };
static int settings_scroll;

static int row_y(int i)
{
	return settings_top[i] - settings_scroll;
}

static struct spot settings_spot(int i)
{
	return (struct spot){ 160, row_y(i), 960, 78 };
}

/* What is below the title and above the hints: the rows are seen there. */
#define SETTINGS_TOP 120
#define SETTINGS_BOTTOM 676

static void background_band(int y0, int y1);

static void switch_widget(int x, int y, int on)
{
	rrect(x, y, 64, 36, 18, on ? ACCENT : SURFACE_HI);
	if (!on)
		rrect(x + 2, y + 2, 60, 32, 16, RGB(0x3A, 0x3F, 0x4C));
	circle(on ? x + 46 : x + 18, y + 18, on ? 13 : 10, on ? ACCENT_DARK : TEXT_DIM);
}

static void row(int i, const char *title, const char *sub)
{
	struct spot r = settings_spot(i);

	if (focus == i)
		rrect(r.x, r.y, r.w, r.h, 20, SURFACE_HI);
	text(r.x + 32, r.y + (sub ? 34 : 48), 26, TEXT, LEFT, title);
	if (sub)
		text(r.x + 32, r.y + 63, 20, TEXT_DIM, LEFT, sub);
}

static void draw_settings(void)
{
	/* The bar at the top: back, title. */
	line(64, 88, 82, 70, 5, TEXT);
	line(64, 88, 82, 106, 5, TEXT);
	text(104, 100, 36, TEXT, LEFT, "Settings");
	if (settings_top[focus] + 78 - settings_scroll > SETTINGS_BOTTOM)
		settings_scroll = settings_top[focus] + 78 - SETTINGS_BOTTOM;
	if (settings_top[focus] - 40 - settings_scroll < SETTINGS_TOP)
		settings_scroll = settings_top[focus] - 40 - SETTINGS_TOP > 0 ? settings_top[focus] - 40 - SETTINGS_TOP : 0;

	text(192, row_y(S_KEYS) - 14, 20, ACCENT, LEFT, "Controls");
	row(S_KEYS, "Keyboard & mouse keys", "What the pad's buttons are in that mode of the + menu");
	line(1064, 182, 1078, 196, 4, TEXT_DIM);
	line(1064, 210, 1078, 196, 4, TEXT_DIM);

	text(192, row_y(S_UPDATES) - 14, 20, ACCENT, LEFT, "Updates");
	row(S_UPDATES, "Check for updates", "When the launcher starts, on github.com/" UPDATE_REPO);
	switch_widget(1024, row_y(S_UPDATES) + 21, settings.check_updates);
	row(S_CHECK, "Check now", "Chromium NX " GX_VERSION);

	text(192, row_y(S_DIAG) - 14, 20, ACCENT, LEFT, "Advanced");
	row(S_DIAG, "Diagnostics", "Profiler and network errors in chromium-nx.log");
	switch_widget(1024, row_y(S_DIAG) + 21, settings.diagnostics);
	row(S_DISK, "Background disk reads", "The SD card is read without stopping other work");
	switch_widget(1024, row_y(S_DISK) + 21, settings.async_disk);
	row(S_PRIO, "Thread priorities", "Display, video and sound go before page scripts");
	switch_widget(1024, row_y(S_PRIO) + 21, settings.priorities);

	text(192, row_y(S_CLEAR) - 14, 20, ACCENT, LEFT, "Data");
	row(S_CLEAR, "Clear browsing data", "Cookies, history, cache and logins");

	/* The rows that went under the title or the hints are covered. */
	background_band(0, SETTINGS_TOP);
	background_band(SETTINGS_BOTTOM, H);
	status_bar();
	line(64, 88, 82, 70, 5, TEXT);
	line(64, 88, 82, 106, 5, TEXT);
	text(104, 100, 36, TEXT, LEFT, "Settings");
	text(W - 40, 98, 20, TEXT_DIM, RIGHT,
	     "Chromium NX " GX_VERSION "  \xC2\xB7  Chromium 156 on Linux (LKL)");
}

/* The keys screen: the inputs in two columns of ten. */
static struct spot key_spot(int i)
{
	return (struct spot){ 150 + i / 10 * 500, 130 + i % 10 * 52, 480, 46 };
}

static void draw_keys(void)
{
	line(64, 88, 82, 70, 5, TEXT);
	line(64, 88, 82, 106, 5, TEXT);
	text(104, 100, 36, TEXT, LEFT, "Keyboard & mouse keys");
	for (int i = 0; i < GX_INPUTS; i++) {
		struct spot s = key_spot(i);
		int changed = settings.keys[i] != gx_inputs[i].default_key;

		if (focus == i && !picking)
			rrect(s.x, s.y, s.w, s.h, 16, SURFACE_HI);
		text(s.x + 24, s.y + 31, 22, TEXT_DIM, LEFT, gx_inputs[i].name);
		text(s.x + s.w - 24, s.y + 31, 22, changed ? ACCENT : TEXT, RIGHT, gx_key_name(settings.keys[i]));
	}
	text(W / 2, 666, 18, TEXT_DIM, CENTER, "The right stick is the mouse and + opens the menu: those stay.");
}

/* The key picker: every key of keys.h in a grid. */
#define PICK_COLUMNS 8

static struct spot pick_spot(int i)
{
	return (struct spot){ 116 + i % PICK_COLUMNS * 132, 166 + i / PICK_COLUMNS * 46, 124, 40 };
}

static void draw_picker(void)
{
	char title[80];

	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			blend(x, y, RGB(0, 0, 0), 140);
	rrect(84, 70, W - 168, 590, 28, SURFACE);
	snprintf(title, sizeof(title), "Key for %s", gx_inputs[focus].name);
	text(116, 130, 30, TEXT, LEFT, title);
	for (int i = 0; i < GX_KEY_COUNT; i++) {
		struct spot s = pick_spot(i);
		int current = gx_keys[i].code == settings.keys[focus];

		rrect(s.x, s.y, s.w, s.h, 14, i == pick_focus ? ACCENT : SURFACE_HI);
		text(s.x + s.w / 2, s.y + 27, strlen(gx_keys[i].name) > 5 ? 17 : 20,
		     i == pick_focus ? ACCENT_DARK : current ? ACCENT : TEXT, CENTER, gx_keys[i].name);
	}
}

static struct spot dialog_spot(int i)
{
	return (struct spot){ i ? 650 : 450, 430, 180, 56 };
}

static void draw_dialog(void)
{
	/* Darken what is under it. */
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			blend(x, y, RGB(0, 0, 0), 140);
	rrect(340, 230, 600, 290, 28, SURFACE);
	text(W / 2, 300, 32, TEXT, CENTER, "Clear browsing data?");
	text(W / 2, 350, 22, TEXT_DIM, CENTER, "Cookies, history, cache and logins are deleted.");
	text(W / 2, 382, 22, TEXT_DIM, CENTER, "This cannot be undone.");
	for (int i = 0; i < 2; i++) {
		struct spot b = dialog_spot(i);

		rrect(b.x, b.y, b.w, b.h, 28, dialog_focus == i ? (i ? RGB(0xF2, 0x8B, 0x82) : ACCENT) : SURFACE_HI);
		text(b.x + b.w / 2, b.y + 37, 24, dialog_focus == i ? ACCENT_DARK : TEXT, CENTER,
		     i ? "Clear" : "Cancel");
	}
}

/* ---------- updates (update.c) ---------- */

enum { UD_NONE, UD_UPDATE, UD_IMAGE, UD_PROGRESS, UD_ERROR };
static int upd, upd_focus = 1, upd_prompted, upd_manual, upd_missing;
static struct update_status us;

static const char *megabytes(uint64_t bytes)
{
	static char s[2][24];
	static int which;

	which ^= 1;
	if (bytes < (100ULL << 20))
		snprintf(s[which], sizeof(s[which]), "%.1f MB", bytes / 1048576.0);
	else
		snprintf(s[which], sizeof(s[which]), "%llu MB", (unsigned long long)(bytes >> 20));
	return s[which];
}

/* The dialog's buttons: two (0 on the left), or one. */
static int upd_buttons(void)
{
	return upd == UD_UPDATE || (upd == UD_IMAGE && us.image_bytes) ? 2 : 1;
}

static struct spot upd_spot(int i)
{
	if (upd_buttons() == 1)
		return (struct spot){ 550, 450, 180, 56 };
	return (struct spot){ i ? 660 : 440, 450, 180, 56 };
}

static void show_image_dialog(void)
{
	upd_missing = access(DIR "/" UPDATE_IMAGE, F_OK) != 0;
	upd = UD_IMAGE;
	upd_focus = 1;
}

static void upd_choose(int i)
{
	if (upd_buttons() == 1)
		i = 1;
	switch (upd) {
	case UD_UPDATE:
		if (i) {
			update_install_latest();
			upd = UD_PROGRESS;
		} else if (us.image == IMAGE_MISSING || us.image == IMAGE_OTHER) {
			/* Later, but the browser cannot start without its image. */
			show_image_dialog();
		} else {
			upd = UD_NONE;
		}
		break;
	case UD_IMAGE:
		if (i && us.image_bytes) {
			update_install_image();
			upd = UD_PROGRESS;
		} else {
			upd = UD_NONE;
		}
		break;
	case UD_PROGRESS:
		update_cancel();
		break;
	case UD_ERROR:
		upd = UD_NONE;
		break;
	}
	upd_focus = 1;
}

/* Each frame: what update.c has come to. 1 when the program was replaced
 * and is to be started again. */
static int update_tick(void)
{
	update_status(&us);
	if (us.state == UPDATE_CHECKED && !upd_prompted) {
		upd_prompted = 1;
		if (us.newer) {
			upd = UD_UPDATE;
			upd_focus = 1;
		} else if (us.image == IMAGE_MISSING || us.image == IMAGE_OTHER) {
			show_image_dialog();
		} else if (upd_manual) {
			char s[200];

			snprintf(s, sizeof(s), "No release found (%s)", us.error[0] ? us.error : "no connection");
			say(us.latest[0] ? "Chromium NX is up to date" : s);
		}
	}
	if (upd == UD_PROGRESS) {
		if (us.state == UPDATE_INSTALLED)
			return 1;
		if (us.state == UPDATE_IMAGE_DONE) {
			upd = UD_NONE;
			say("Chromium's image is ready");
		} else if (us.state == UPDATE_FAILED) {
			upd = UD_ERROR;
		}
	}
	return 0;
}

static void check_now(void)
{
	if (us.state == UPDATE_DOWNLOADING)
		return;
	if (us.state == UPDATE_CHECKING) {
		/* The check of the start is still going: its answer is said. */
		upd_manual = 1;
		say("Checking for updates...");
		return;
	}
	upd_manual = 1;
	upd_prompted = 0;
	say("Checking for updates...");
	update_check();
}

static void upd_button(int i, const char *label)
{
	struct spot b = upd_spot(i);
	int on = upd_focus == i || upd_buttons() == 1;

	rrect(b.x, b.y, b.w, b.h, 28, on ? ACCENT : SURFACE_HI);
	text(b.x + b.w / 2, b.y + 37, 24, on ? ACCENT_DARK : TEXT, CENTER, label);
}

static void draw_update(void)
{
	char s[200];

	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			blend(x, y, RGB(0, 0, 0), 140);
	rrect(220, 190, 840, 350, 28, SURFACE);
	switch (upd) {
	case UD_UPDATE:
		snprintf(s, sizeof(s), "Chromium NX %s is available", us.latest);
		text(W / 2, 262, 32, TEXT, CENTER, s);
		snprintf(s, sizeof(s), "You have %s. To download: %s.", GX_VERSION, megabytes(us.update_bytes));
		text(W / 2, 318, 22, TEXT_DIM, CENTER, s);
		text(W / 2, 352, 22, TEXT_DIM, CENTER, "Your profile, settings and downloads are kept.");
		upd_button(0, "Later");
		upd_button(1, "Update");
		break;
	case UD_IMAGE:
		text(W / 2, 262, 32, TEXT, CENTER,
		     upd_missing ? "Chromium's image is missing" : "Chromium's image is not this version's");
		text(W / 2, 312, 22, TEXT_DIM, CENTER,
		     "Chromium NX " GX_VERSION " needs " UPDATE_IMAGE " from its release:");
		text(W / 2, 344, 22, TEXT, CENTER, "github.com/" UPDATE_REPO "/releases/tag/v" GX_VERSION);
		if (us.image_bytes) {
			snprintf(s, sizeof(s), "Download it now (%s), or put it in /switch/chromium-nx.",
				 megabytes(us.image_bytes));
			text(W / 2, 384, 20, TEXT_DIM, CENTER, s);
			upd_button(0, "Later");
			upd_button(1, "Download");
		} else {
			text(W / 2, 384, 20, TEXT_DIM, CENTER, "Put it in /switch/chromium-nx on the SD card.");
			upd_button(1, "OK");
		}
		break;
	case UD_PROGRESS: {
		uint64_t total = us.total ? us.total : 1;

		text(W / 2, 262, 32, TEXT, CENTER, us.state != UPDATE_DOWNLOADING ? "Getting ready" :
						   us.total && us.done >= us.total ? "Checking" : "Downloading");
		text(W / 2, 306, 22, TEXT_DIM, CENTER, us.file);
		rrect(320, 340, 640, 16, 8, SURFACE_HI);
		if (us.done)
			rrect(320, 340, (int)(640 * (us.done > total ? total : us.done) / total) < 16 ? 16 :
					(int)(640 * (us.done > total ? total : us.done) / total), 16, 8, ACCENT);
		snprintf(s, sizeof(s), "%s of %s", megabytes(us.done), megabytes(us.total));
		text(W / 2, 400, 22, TEXT_DIM, CENTER, s);
		upd_button(1, "Stop");
		break;
	}
	case UD_ERROR:
		text(W / 2, 262, 32, TEXT, CENTER, "Not updated");
		text(W / 2, 320, 22, TEXT_DIM, CENTER, us.error);
		upd_button(1, "OK");
		break;
	}
}

static void draw_hints(void)
{
	const char *h = dialog ? "A  Select     B  Cancel" :
			picking ? "A  Choose     B  Cancel" :
			screen == HOME ? "A  Open     X  Settings     +  Exit" :
			screen == KEYS ? "A  Change     Y  Reset all     B  Back" : "A  Change     B  Back     +  Exit";

	text(W / 2, H - 18, 20, TEXT_DIM, CENTER, h);
}

static void draw_toast(void)
{
	if (!toast[0] || armGetSystemTick() > toast_until)
		return;
	int w = text_width(toast, 22) + 64;

	rrect(W / 2 - w / 2, 610, w, 52, 26, RGB(0xE8, 0xEA, 0xF0));
	text(W / 2, 644, 22, RGB(0x20, 0x22, 0x28), CENTER, toast);
}

static void draw_starting(void)
{
	background();
	draw_app_icon(0, W / 2, H / 2 - 40);
	text(W / 2, H / 2 + 70, 28, TEXT, CENTER, "Starting Chromium...");
}

/* ---------- input ---------- */

static int inside(struct spot s, int x, int y)
{
	return x >= s.x && y >= s.y && x < s.x + s.w && y < s.y + s.h;
}

enum { NONE, QUIT, GO };
static char go_url[1024];
static int go_tv;   /* YouTube's TV page: gx's -T */

static void clear_data(void)
{
	/* The profile image; the browser makes a new one when it starts. A
	 * profile folder from an older version, and the list of its links (the
	 * browser's own symbolic links, which the card cannot hold), go too. */
	FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
	int cleared = 0;

	if (fs && R_SUCCEEDED(fsFsDeleteDirectoryRecursively(fs, "/switch/chromium-nx/profile")))
		cleared = 1;
	if (fs)
		fsFsDeleteFile(fs, "/switch/chromium-nx/.gx-links");
	if (!remove(DIR "/profile.ext4"))
		cleared = 1;
	say(cleared ? "Browsing data cleared" : "Nothing to clear");
}

static int activate(void)
{
	char in[512] = "";

	if (dialog) {
		if (dialog_focus)
			clear_data();
		dialog = 0;
		return NONE;
	}
	if (picking) {
		settings.keys[focus] = gx_keys[pick_focus].code;
		picking = 0;
		save_settings();
		return NONE;
	}
	if (screen == KEYS) {
		picking = 1;
		pick_focus = 0;
		for (int i = 0; i < GX_KEY_COUNT; i++)
			if (gx_keys[i].code == settings.keys[focus])
				pick_focus = i;
		return NONE;
	}
	if (screen == HOME) {
		go_tv = focus == 2;
		switch (focus) {
		case 0:
			if (ask("Search", "Search or type a web address", NULL, in, sizeof(in))) {
				to_url(in, go_url, sizeof(go_url));
				return GO;
			}
			return NONE;
		case 1:
			go_url[0] = 0;
			return GO;
		case 2:
			snprintf(go_url, sizeof(go_url), "https://www.youtube.com/tv");
			return GO;
		case 3:
			snprintf(go_url, sizeof(go_url), "https://www.google.com/");
			return GO;
		case 4:
			snprintf(go_url, sizeof(go_url), "chrome://settings/");
			return GO;
		case 5:
			screen = SETTINGS;
			focus = 0;
			return NONE;
		}
		return NONE;
	}
	switch (focus) {
	case S_KEYS:
		screen = KEYS;
		focus = 0;
		return NONE;
	case S_UPDATES:
		settings.check_updates = !settings.check_updates;
		break;
	case S_CHECK:
		check_now();
		return NONE;
	case S_DIAG:
		settings.diagnostics = !settings.diagnostics;
		break;
	case S_DISK:
		settings.async_disk = !settings.async_disk;
		break;
	case S_PRIO:
		settings.priorities = !settings.priorities;
		break;
	case S_CLEAR:
		dialog = 1;
		dialog_focus = 0;
		return NONE;
	}
	save_settings();
	return NONE;
}

static void back(void)
{
	if (dialog)
		dialog = 0;
	else if (picking)
		picking = 0;
	else if (screen == KEYS) {
		screen = SETTINGS;
		focus = S_KEYS;
	} else if (screen == SETTINGS) {
		screen = HOME;
		focus = APPS;
	}
}

static void move(int dx, int dy)
{
	if (dialog) {
		if (dx)
			dialog_focus = dx > 0;
		return;
	}
	if (picking) {
		int to = pick_focus + dx + dy * PICK_COLUMNS;

		if (to >= 0 && to < GX_KEY_COUNT && (dy || to / PICK_COLUMNS == pick_focus / PICK_COLUMNS))
			pick_focus = to;
		return;
	}
	if (screen == KEYS) {
		int to = focus + dy + dx * 10;

		if (to >= 0 && to < GX_INPUTS && (dx || to / 10 == focus / 10))
			focus = to;
		return;
	}
	if (screen == HOME) {
		if (dy < 0)
			focus = 0;
		else if (dy > 0 && focus == 0)
			focus = 1;
		else if (dx && focus > 0)
			focus = focus + dx < 1 ? 1 : focus + dx > APPS ? APPS : focus + dx;
		return;
	}
	if (dy)
		focus = focus + dy < 0 ? 0 : focus + dy >= ROWS ? ROWS - 1 : focus + dy;
}

/* A tap: what was under it. */
static int tap(int x, int y)
{
	if (dialog) {
		for (int i = 0; i < 2; i++)
			if (inside(dialog_spot(i), x, y)) {
				dialog_focus = i;
				return activate();
			}
		return NONE;
	}
	if (picking) {
		for (int i = 0; i < GX_KEY_COUNT; i++)
			if (inside(pick_spot(i), x, y)) {
				pick_focus = i;
				return activate();
			}
		if (!inside((struct spot){ 84, 70, W - 168, 590 }, x, y))
			picking = 0;
		return NONE;
	}
	if (screen == KEYS) {
		if (x < 140 && y < 150) {
			back();
			return NONE;
		}
		for (int i = 0; i < GX_INPUTS; i++)
			if (inside(key_spot(i), x, y)) {
				focus = i;
				return activate();
			}
		return NONE;
	}
	if (screen == HOME) {
		for (int i = 0; i <= APPS; i++)
			if (inside(home_spot(i), x, y)) {
				focus = i;
				return activate();
			}
		return NONE;
	}
	if (x < 140 && y < 150) {
		back();
		return NONE;
	}
	for (int i = 0; i < ROWS && y >= SETTINGS_TOP && y < SETTINGS_BOTTOM; i++)
		if (inside(settings_spot(i), x, y)) {
			focus = i;
			return activate();
		}
	return NONE;
}

/* ---------- main ---------- */

int launcher_main(int argc, char **argv)
{
	Framebuffer fb;
	PadState pad;
	PlFontData font;
	int touching = 0, result = NONE;
	FILE *old;

	/* Left by an older version, when the browser was a program of its own
	 * and its command line a file the build wrote (not DEV=1's menu). */
	remove(DIR "/gx.nro");
	if ((old = fopen(DIR "/programs.txt", "r"))) {
		char first[32] = "";
		int built = fgets(first, sizeof(first), old) && !strncmp(first, "[Chromium NX]", 13);

		fclose(old);
		if (built)
			remove(DIR "/programs.txt");
	}
	/* A launch asked for and not carried out (a test writes one): it is. */
	if (!access(DIR "/launch.txt", F_OK) && !start_browser())
		return 0;

	plInitialize(PlServiceType_User);
	psmInitialize();
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	hidInitializeTouchScreen();
	load_settings();

	if (FT_Init_FreeType(&ft) || R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) ||
	    FT_New_Memory_Face(ft, font.address, font.size, 0, &face)) {
		/* No font: straight to the browser. */
		result = launch("", 0) ? QUIT : GO;
		goto out;
	}
	load_icon();
	framebufferCreate(&fb, nwindowGetDefault(), W, H, PIXEL_FORMAT_RGBA_8888, 2);
	framebufferMakeLinear(&fb);
	if (settings.check_updates)
		update_check();
	else if (access(DIR "/" UPDATE_IMAGE, F_OK))
		show_image_dialog();

	while (appletMainLoop()) {
		HidTouchScreenState touch = { 0 };
		u64 down;

		padUpdate(&pad);
		down = padGetButtonsDown(&pad);
		if (update_tick()) {
			/* The new program, in this one's place. */
			envSetNextLoad(SELF, SELF);
			result = QUIT;
			break;
		}
		if ((down & HidNpadButton_Plus) && upd != UD_PROGRESS) {
			result = QUIT;
			break;
		}
		if (upd) {
			/* The update's dialog has the pad and the screen. */
			if (down & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight))
				upd_focus = !!(down & HidNpadButton_AnyRight);
			if (down & HidNpadButton_A)
				upd_choose(upd_focus);
			else if (down & HidNpadButton_B)
				upd_choose(0);
			if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
				for (int i = 0; i < 2 && !touching; i++)
					if (inside(upd_spot(i), touch.touches[0].x, touch.touches[0].y))
						upd_choose(i);
				touching = 1;
			} else {
				touching = 0;
			}
			down = 0;
		}
		if (down & (HidNpadButton_AnyUp | HidNpadButton_StickLUp))
			move(0, -1);
		if (down & (HidNpadButton_AnyDown | HidNpadButton_StickLDown))
			move(0, 1);
		if (down & (HidNpadButton_AnyLeft | HidNpadButton_StickLLeft))
			move(-1, 0);
		if (down & (HidNpadButton_AnyRight | HidNpadButton_StickLRight))
			move(1, 0);
		if (down & HidNpadButton_B)
			back();
		if ((down & HidNpadButton_Y) && screen == KEYS && !picking) {
			for (int i = 0; i < GX_INPUTS; i++)
				settings.keys[i] = gx_inputs[i].default_key;
			save_settings();
			say("Keys reset");
		}
		if ((down & HidNpadButton_X) && screen == HOME && !dialog) {
			screen = SETTINGS;
			focus = 0;
		}
		if (down & HidNpadButton_A)
			result = activate();
		if (!upd && hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
			if (!touching)
				result = tap(touch.touches[0].x, touch.touches[0].y);
			touching = 1;
		} else if (!upd) {
			touching = 0;
		}
		/* No browser without its image: what to do about it instead. */
		if (result == GO && access(DIR "/" UPDATE_IMAGE, F_OK)) {
			show_image_dialog();
			result = NONE;
		}

		fbuf = framebufferBegin(&fb, &fstride);
		fstride /= 4;
		if (result == GO) {
			draw_starting();
			framebufferEnd(&fb);
			if (!launch(go_url, go_tv))
				break;
			say(launch_error);
			result = NONE;
			continue;
		}
		background();
		status_bar();
		if (screen == HOME)
			draw_home();
		else if (screen == KEYS)
			draw_keys();
		else
			draw_settings();
		if (picking)
			draw_picker();
		draw_hints();
		if (dialog)
			draw_dialog();
		if (upd)
			draw_update();
		draw_toast();
		framebufferEnd(&fb);
	}
	framebufferClose(&fb);
out:
	/* Everything given back: the browser is loaded into this process next,
	 * by the Homebrew loader, which wants the heap as it gave it. */
	update_exit();
	for (int i = 0; i < GLYPHS; i++)
		free(glyphs[i].bits);
	if (face)
		FT_Done_Face(face);
	if (ft)
		FT_Done_FreeType(ft);
	psmExit();
	plExit();
	return 0;
}
