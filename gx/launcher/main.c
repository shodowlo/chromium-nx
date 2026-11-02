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
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include "../keys.h"
#include "../version.h"
#include "update.h"

#define W 1280
#define H 720
#define APP_DIR "sdmc:/switch/chromium-nx"
#define SELF APP_DIR "/" UPDATE_PROGRAM

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
	int hires;           /* 1080p on a TV; not: gx's -l */
} settings = { 0, 1, 1, { 0 }, 1, 1 };

static void load_settings(void)
{
	FILE *f = fopen(APP_DIR "/settings.ini", "r");
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
		else if (!strcmp(line, "hires"))
			settings.hires = !!atoi(v);
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
	FILE *f = fopen(APP_DIR "/settings.ini", "w");

	if (!f)
		return;
	fprintf(f, "diagnostics=%d\nasync_disk=%d\npriorities=%d\ncheck_updates=%d\nhires=%d\n",
		settings.diagnostics, settings.async_disk, settings.priorities, settings.check_updates,
		settings.hires);
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
static int go_full, go_mode, go_portrait;   /* the site being opened: full screen, the pad's mode, portrait (-F, -M, -R) */

/* This program again, as the browser: what launch.txt says is run. */
static int start_browser(void)
{
	if (R_FAILED(envSetNextLoad(SELF, SELF " --browser"))) {
		remove(APP_DIR "/launch.txt");
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
	if (go_full)
		strcat(opts, "-F ");   /* a site full screen */
	if (!settings.hires)
		strcat(opts, "-l ");   /* 720p on a TV too */
	if (go_mode)
		snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), "-M%d ", go_mode);
	if (go_portrait)
		strcat(opts, "-R ");   /* a site in portrait */
	/* Chromium opens no chrome:// page from its command line (on Linux): gx
	 * takes it there once it has started (-N). */
	if (!strncmp(url, "chrome://", 9) && strlen(url) < 128) {
		snprintf(opts + strlen(opts), sizeof(opts) - strlen(opts), "-N%s ", url);
		url = "";
	}
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
	f = fopen(APP_DIR "/launch.txt", "w");
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

enum { HOME, SETTINGS, KEYS, DOWNLOADS, SITES, SITE_EDIT, HELP };
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

/* The dialog over the screen, if any: what it asks. */
enum { DIALOG_NONE, DIALOG_CLEAR, DIALOG_DELETE, DIALOG_DELETE_SITE, DIALOG_IMPORT, DIALOG_EXPORT, DIALOG_RESET };

/* Export and import (Settings, Data): the profile, the settings and the
 * sites, copied to a folder of the SD card or back. */
#define BACKUP_DIR APP_DIR "/Backup"
#define BACKUP_SHOWN "/switch/chromium-nx/Backup"

/* Lists that scroll follow the focus, unless a finger moved them (main()). */
static int follow_focus = 1;

/* ---------- the downloads (the browser's, on the SD card) ---------- */

#define DOWNLOADS_DIR APP_DIR "/Downloads"
#define MAX_DOWNLOADS 256
#define DOWNLOAD_ROW 64
#define DOWNLOAD_TOP 126
#define DOWNLOAD_ROWS 8

static struct download {
	char name[256];
	long long size;
} downloads[MAX_DOWNLOADS];
static int download_count, download_scroll;

static int by_name(const void *a, const void *b)
{
	return strcasecmp(((const struct download *)a)->name, ((const struct download *)b)->name);
}

static void load_downloads(void)
{
	DIR *d = opendir(DOWNLOADS_DIR);
	struct dirent *e;

	download_count = 0;
	download_scroll = 0;
	while (d && (e = readdir(d)) && download_count < MAX_DOWNLOADS) {
		char path[512];
		struct stat st;

		snprintf(path, sizeof(path), DOWNLOADS_DIR "/%s", e->d_name);
		if (e->d_name[0] == '.' || stat(path, &st) || !S_ISREG(st.st_mode))
			continue;
		snprintf(downloads[download_count].name, sizeof(downloads[0].name), "%s", e->d_name);
		downloads[download_count++].size = st.st_size;
	}
	if (d)
		closedir(d);
	qsort(downloads, download_count, sizeof(downloads[0]), by_name);
}

static void delete_download(int i)
{
	char path[512];

	if (i < 0 || i >= download_count)
		return;
	snprintf(path, sizeof(path), DOWNLOADS_DIR "/%s", downloads[i].name);
	say(remove(path) ? "Could not delete it" : "Deleted");
	load_downloads();
	if (focus >= download_count)
		focus = download_count ? download_count - 1 : 0;
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

#define APPS 8
enum { APP_CHROMIUM, APP_YOUTUBE, APP_SITES, APP_GITHUB, APP_DOWNLOADS, APP_HELP, APP_CHROMIUM_SETTINGS,
       APP_SETTINGS };
static const char *app_names[APPS] = { "Chromium", "YouTube", "My sites", "GitHub", "Downloads", "Help",
				       "Chromium settings", "Settings" };
#define SPAN 156

static struct spot home_spot(int i)
{
	if (i == 0)
		return (struct spot){ 240, 300, 800, 72 };
	int x0 = W / 2 - APPS * SPAN / 2;

	return (struct spot){ x0 + (i - 1) * SPAN + 8, 440, 140, 170 };
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
	case APP_SITES: {   /* My sites: orange, a white star */
		float px[10], py[10];

		circle(cx, cy, 50, RGB(0xF2, 0x99, 0x1F));
		/* A star of five points: ten corners, out and in by turns. */
		for (int k = 0; k < 10; k++) {
			float a = -(float)M_PI / 2 + k * (float)M_PI / 5, r = k % 2 ? 11.5f : 27;

			px[k] = cx + cosf(a) * r;
			py[k] = cy + 3 + sinf(a) * r;
		}
		for (int y = cy - 28; y <= cy + 30; y++)
			for (int x = cx - 28; x <= cx + 28; x++) {
				int in = 0;

				for (int k = 0, j = 9; k < 10; j = k++)
					if ((py[k] > y + 0.5f) != (py[j] > y + 0.5f) &&
					    x + 0.5f < px[k] + (y + 0.5f - py[k]) * (px[j] - px[k]) / (py[j] - py[k]))
						in = !in;
				if (in)
					blend(x, y, TEXT, 255);
			}
		break;
	}
	case APP_HELP:   /* Help: teal, a question mark */
		circle(cx, cy, 50, RGB(0x12, 0x8C, 0x7E));
		text(cx, cy + 20, 58, TEXT, CENTER, "?");
		break;
	case APP_GITHUB: {   /* GitHub: dark, a branch of white */
		u32 dark = RGB(0x24, 0x29, 0x2F);

		circle(cx, cy, 50, dark);
		line(cx - 12, cy - 22, cx - 12, cy + 22, 6, TEXT);
		line(cx + 14, cy - 10, cx - 12, cy + 12, 6, TEXT);
		circle(cx - 12, cy - 22, 9, TEXT);
		circle(cx - 12, cy - 22, 4, dark);
		circle(cx - 12, cy + 24, 9, TEXT);
		circle(cx - 12, cy + 24, 4, dark);
		circle(cx + 14, cy - 14, 9, TEXT);
		circle(cx + 14, cy - 14, 4, dark);
		break;
	}
	case APP_DOWNLOADS:   /* Downloads: blue, an arrow down onto a line */
		circle(cx, cy, 50, RGB(0x1A, 0x73, 0xE8));
		line(cx, cy - 24, cx, cy + 10, 7, TEXT);
		line(cx - 15, cy - 4, cx, cy + 12, 7, TEXT);
		line(cx + 15, cy - 4, cx, cy + 12, 7, TEXT);
		line(cx - 20, cy + 26, cx + 20, cy + 26, 6, TEXT);
		break;
	case APP_CHROMIUM_SETTINGS:   /* Chromium's settings: its icon, a gear on it */
		draw_app_icon(APP_CHROMIUM, cx, cy);
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
	case APP_SETTINGS:   /* Settings: a gear */
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

		while (size > 16 && text_width(app_names[i], size) > SPAN - 6)
			size--;
		text(cx, a.y + 150, size, focus == i + 1 ? TEXT : TEXT_DIM, CENTER, app_names[i]);
	}
}

/* Settings: rows, some under a heading. */
enum { S_KEYS, S_UPDATES, S_CHECK, S_HIRES, S_DIAG, S_DISK, S_PRIO, S_CLEAR, S_EXPORT, S_IMPORT, S_RESET, ROWS };

/* Where the rows are on the page of the settings, which is taller than the
 * screen: it scrolls so that the row with the focus is seen. */
static const int settings_top[ROWS] = { 156, 288, 368, 500, 632, 712, 792, 924, 1004, 1084, 1164 };
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
	if (follow_focus && settings_top[focus] + 78 - settings_scroll > SETTINGS_BOTTOM)
		settings_scroll = settings_top[focus] + 78 - SETTINGS_BOTTOM;
	if (follow_focus && settings_top[focus] - 40 - settings_scroll < SETTINGS_TOP)
		settings_scroll = settings_top[focus] - 40 - SETTINGS_TOP > 0 ? settings_top[focus] - 40 - SETTINGS_TOP : 0;

	text(192, row_y(S_KEYS) - 14, 20, ACCENT, LEFT, "Controls");
	row(S_KEYS, "Keyboard & mouse keys", "What the pad's buttons are in that mode of the + menu");
	line(1064, row_y(S_KEYS) + 26, 1078, row_y(S_KEYS) + 40, 4, TEXT_DIM);
	line(1064, row_y(S_KEYS) + 54, 1078, row_y(S_KEYS) + 40, 4, TEXT_DIM);

	text(192, row_y(S_UPDATES) - 14, 20, ACCENT, LEFT, "Updates");
	row(S_UPDATES, "Check for updates", "When the launcher starts, on github.com/" UPDATE_REPO);
	switch_widget(1024, row_y(S_UPDATES) + 21, settings.check_updates);
	row(S_CHECK, "Check now", "Chromium NX " GX_VERSION);

	text(192, row_y(S_HIRES) - 14, 20, ACCENT, LEFT, "Screen");
	row(S_HIRES, "1080p on a TV", "Sharper in the dock; 720p is faster");
	switch_widget(1024, row_y(S_HIRES) + 21, settings.hires);

	text(192, row_y(S_DIAG) - 14, 20, ACCENT, LEFT, "Advanced");
	row(S_DIAG, "Diagnostics", "Profiler and network errors in chromium-nx.log");
	switch_widget(1024, row_y(S_DIAG) + 21, settings.diagnostics);
	row(S_DISK, "Background disk reads", "The SD card is read without stopping other work");
	switch_widget(1024, row_y(S_DISK) + 21, settings.async_disk);
	row(S_PRIO, "Thread priorities", "Display, video and sound go before page scripts");
	switch_widget(1024, row_y(S_PRIO) + 21, settings.priorities);

	text(192, row_y(S_CLEAR) - 14, 20, ACCENT, LEFT, "Data");
	row(S_CLEAR, "Clear browsing data", "Cookies, history, cache and logins");
	row(S_EXPORT, "Export profile", "Browsing data, settings and sites, to " BACKUP_SHOWN);
	row(S_IMPORT, "Import profile", "What was exported, in place of what there is now");
	row(S_RESET, "Reset settings", "These settings and the keys as they first were");

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

/* s, cut to fit in width at size, with an ellipsis if it had to be. */
static const char *fit(const char *s, int size, int width)
{
	static char out[300];
	size_t n = strlen(s);

	snprintf(out, sizeof(out), "%s", s);
	if (text_width(out, size) <= width)
		return out;
	while (n > 1) {
		/* Not in the middle of a character of UTF-8. */
		while (n > 1 && ((unsigned char)s[n - 1] & 0xC0) == 0x80)
			n--;
		n--;
		snprintf(out, sizeof(out), "%.*s...", (int)n, s);
		if (text_width(out, size) <= width)
			break;
	}
	return out;
}

static const char *size_text(long long bytes)
{
	static char s[32];

	if (bytes < 1024)
		snprintf(s, sizeof(s), "%lld B", bytes);
	else if (bytes < (1LL << 20))
		snprintf(s, sizeof(s), "%.0f KB", bytes / 1024.0);
	else if (bytes < (1LL << 30))
		snprintf(s, sizeof(s), "%.1f MB", bytes / 1048576.0);
	else
		snprintf(s, sizeof(s), "%.2f GB", bytes / 1073741824.0);
	return s;
}

static struct spot download_row(int i)
{
	return (struct spot){ 150, DOWNLOAD_TOP + (i - download_scroll) * DOWNLOAD_ROW, 980, DOWNLOAD_ROW - 8 };
}

/* The cross that deletes a download, at the end of its row. */
static struct spot download_cross(int i)
{
	struct spot r = download_row(i);

	if (i < download_scroll || i >= download_scroll + DOWNLOAD_ROWS)
		return (struct spot){ 0, 0, 0, 0 };
	return (struct spot){ r.x + r.w - 64, r.y, 64, r.h };
}

static void draw_downloads(void)
{
	line(64, 88, 82, 70, 5, TEXT);
	line(64, 88, 82, 106, 5, TEXT);
	text(104, 100, 36, TEXT, LEFT, "Downloads");
	text(W - 40, 98, 20, TEXT_DIM, RIGHT, "/switch/chromium-nx/Downloads");
	if (!download_count) {
		text(W / 2, 360, 26, TEXT_DIM, CENTER, "Nothing downloaded yet");
		return;
	}
	if (follow_focus && focus < download_scroll)
		download_scroll = focus;
	if (follow_focus && focus >= download_scroll + DOWNLOAD_ROWS)
		download_scroll = focus - DOWNLOAD_ROWS + 1;
	for (int i = download_scroll; i < download_count && i < download_scroll + DOWNLOAD_ROWS; i++) {
		struct spot r = download_row(i), c = download_cross(i);
		int cx = c.x + c.w / 2, cy = c.y + c.h / 2;

		if (focus == i)
			rrect(r.x, r.y, r.w, r.h, 16, SURFACE_HI);
		text(r.x + 24, r.y + 37, 23, TEXT, LEFT, fit(downloads[i].name, 23, 680));
		text(c.x - 16, r.y + 37, 20, TEXT_DIM, RIGHT, size_text(downloads[i].size));
		circle(cx, cy, 18, focus == i ? RGB(0xF2, 0x8B, 0x82) : SURFACE);
		line(cx - 7, cy - 7, cx + 7, cy + 7, 3.5f, focus == i ? ACCENT_DARK : TEXT_DIM);
		line(cx - 7, cy + 7, cx + 7, cy - 7, 3.5f, focus == i ? ACCENT_DARK : TEXT_DIM);
	}
	if (download_count > DOWNLOAD_ROWS) {
		char s[48];

		snprintf(s, sizeof(s), "%d files", download_count);
		text(W / 2, 668, 18, TEXT_DIM, CENTER, s);
	}
}

/* ---------- the user's own sites ---------- */

#define MAX_SITES 32
#define SITES_FILE APP_DIR "/sites.ini"
#define SITE_ROW 180       /* a row of the grid */
#define SITE_TOP 130
#define SITE_ROWS 3        /* rows seen at once */
#define SITE_COLS 6
#define SITE_SPAN 170
#define EDIT_ROW 78
#define EDIT_TOP 130

enum { E_NAME, E_URL, E_FULL, E_MODE, E_SCREEN, E_SAVE, E_DELETE };

static const char *const mode_names[] = { "Browser", "Keyboard & mouse", "Controller" };

static struct site {
	char name[64], url[512];
	int full, mode;   /* full screen; the + menu's Controls (overlay.h) */
	int portrait;     /* the + menu's Screen */
} sites[MAX_SITES], editing;
static int site_count, site_scroll, edit_index, sites_focus;

static void load_sites(void)
{
	FILE *f = fopen(SITES_FILE, "r");
	char line[700];

	site_count = 0;
	while (f && site_count < MAX_SITES && fgets(line, sizeof(line), f)) {
		struct site *s = &sites[site_count];
		char *name = strtok(line, "\t\r\n"), *url = strtok(NULL, "\t\r\n");
		char *full = strtok(NULL, "\t\r\n"), *mode = strtok(NULL, "\t\r\n");
		char *portrait = strtok(NULL, "\t\r\n");

		if (!name || !url)
			continue;
		snprintf(s->name, sizeof(s->name), "%s", name);
		snprintf(s->url, sizeof(s->url), "%s", url);
		s->full = full && atoi(full);
		s->mode = mode ? atoi(mode) : 0;
		if (s->mode < 0 || s->mode > 2)
			s->mode = 0;
		s->portrait = portrait && atoi(portrait);
		site_count++;
	}
	if (f)
		fclose(f);
}

static void save_sites(void)
{
	FILE *f = fopen(SITES_FILE, "w");

	for (int i = 0; f && i < site_count; i++)
		fprintf(f, "%s\t%s\t%d\t%d\t%d\n", sites[i].name, sites[i].url, sites[i].full, sites[i].mode,
			sites[i].portrait);
	if (f)
		fclose(f);
}

/* What the keyboard gave, without what would break a line of sites.ini. */
static void clean_copy(char *out, size_t room, const char *in)
{
	snprintf(out, room, "%s", in);
	for (char *p = out; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r')
			*p = ' ';
}

/* The sites as tiles, as the home screen's: "Add a site" first. */
static struct spot site_tile(int i)
{
	int x0 = (W - SITE_COLS * SITE_SPAN) / 2;

	return (struct spot){ x0 + i % SITE_COLS * SITE_SPAN + 10, SITE_TOP + (i / SITE_COLS - site_scroll) * SITE_ROW,
			      SITE_SPAN - 20, 170 };
}

static int site_seen(int i)
{
	return i / SITE_COLS >= site_scroll && i / SITE_COLS < site_scroll + SITE_ROWS;
}

/* The first letter of a name, as a capital when it is one of ASCII's. */
static const char *initial(const char *name)
{
	static char out[8];
	const unsigned char *p = (const unsigned char *)name;
	int n = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : 2;

	snprintf(out, sizeof(out), "%.*s", n, name);
	if (out[0] >= 'a' && out[0] <= 'z')
		out[0] -= 32;
	return out;
}

static u32 site_color(const char *name)
{
	static const u32 colors[] = {
		RGB(0x1A, 0x73, 0xE8), RGB(0xE3, 0x74, 0x08), RGB(0x18, 0x80, 0x38), RGB(0x8E, 0x44, 0xD8),
		RGB(0xD9, 0x30, 0x25), RGB(0x00, 0x89, 0x7B), RGB(0xC2, 0x18, 0x5B), RGB(0x5F, 0x63, 0x68),
	};
	unsigned h = 0;

	for (; *name; name++)
		h = h * 31 + (unsigned char)*name;
	return colors[h % (sizeof(colors) / sizeof(colors[0]))];
}

static void draw_sites(void)
{
	line(64, 88, 82, 70, 5, TEXT);
	line(64, 88, 82, 106, 5, TEXT);
	text(104, 100, 36, TEXT, LEFT, "My sites");
	if (follow_focus && focus / SITE_COLS < site_scroll)
		site_scroll = focus / SITE_COLS;
	if (follow_focus && focus / SITE_COLS >= site_scroll + SITE_ROWS)
		site_scroll = focus / SITE_COLS - SITE_ROWS + 1;
	for (int i = 0; i <= site_count; i++) {
		struct spot t = site_tile(i);
		int cx = t.x + t.w / 2, cy = t.y + 60;

		if (!site_seen(i))
			continue;
		if (focus == i)
			rrect(t.x - 6, t.y - 6, t.w + 12, t.h + 12, 28, SURFACE_HI);
		if (!i) {
			circle(cx, cy, 50, SURFACE_HI);
			circle(cx, cy, 48, SURFACE);
			line(cx, cy - 20, cx, cy + 20, 6, ACCENT);
			line(cx - 20, cy, cx + 20, cy, 6, ACCENT);
			text(cx, t.y + 150, 22, focus == i ? TEXT : TEXT_DIM, CENTER, "Add a site");
			continue;
		}
		const struct site *s = &sites[i - 1];

		circle(cx, cy, 50, site_color(s->name));
		text(cx, cy + 19, 52, TEXT, CENTER, initial(s->name));
		text(cx, t.y + 150, 22, focus == i ? TEXT : TEXT_DIM, CENTER, fit(s->name, 22, t.w));
	}
	/* What the site with the focus is. */
	background_band(SETTINGS_BOTTOM - 8, H);
	if (focus >= 1 && focus <= site_count) {
		const struct site *s = &sites[focus - 1];
		char about[700];

		snprintf(about, sizeof(about), "%s%s%s  \xC2\xB7  %s", s->url, s->full ? "  \xC2\xB7  Full screen" : "",
			 s->portrait ? "  \xC2\xB7  Portrait" : "", mode_names[s->mode]);
		text(W / 2, 664, 18, TEXT_DIM, CENTER, fit(about, 18, 1100));
	} else if (!site_count) {
		text(W / 2, 664, 18, TEXT_DIM, CENTER,
		     "Your own pages, each opened full screen or not, with the controls you choose");
	} else if (!focus) {
		text(W / 2, 664, 18, TEXT_DIM, CENTER, "X, or a finger held on a site, to edit it");
	}
}

static int edit_rows(void)
{
	return edit_index >= 0 ? E_DELETE + 1 : E_SAVE + 1;
}

static struct spot edit_row(int i)
{
	return (struct spot){ 160, EDIT_TOP + i * EDIT_ROW, 960, EDIT_ROW - 8 };
}

static void draw_site_edit(void)
{
	line(64, 88, 82, 70, 5, TEXT);
	line(64, 88, 82, 106, 5, TEXT);
	text(104, 100, 36, TEXT, LEFT, edit_index >= 0 ? "Edit a site" : "Add a site");
	for (int i = 0; i < edit_rows(); i++) {
		struct spot r = edit_row(i);
		static const char *const titles[] = { "Name", "Address", "Full screen", "Controls", "Screen", "Save",
						      "Delete" };

		if (focus == i)
			rrect(r.x, r.y, r.w, r.h, 18, i == E_SAVE ? ACCENT : SURFACE_HI);
		else if (i == E_SAVE)
			rrect(r.x, r.y, r.w, r.h, 18, SURFACE);
		if (i >= E_SAVE) {
			text(r.x + r.w / 2, r.y + 45, 26, i == E_DELETE ? RGB(0xF2, 0x8B, 0x82) :
							  focus == i ? ACCENT_DARK : TEXT, CENTER, titles[i]);
			continue;
		}
		text(r.x + 32, r.y + 45, 26, TEXT, LEFT, titles[i]);
		switch (i) {
		case E_NAME:
			text(r.x + r.w - 32, r.y + 45, 22, editing.name[0] ? TEXT : TEXT_DIM, RIGHT,
			     editing.name[0] ? fit(editing.name, 22, 600) : "A to type it");
			break;
		case E_URL:
			text(r.x + r.w - 32, r.y + 45, 22, editing.url[0] ? TEXT : TEXT_DIM, RIGHT,
			     editing.url[0] ? fit(editing.url, 22, 640) : "A to type it");
			break;
		case E_FULL:
			switch_widget(r.x + r.w - 96, r.y + 17, editing.full);
			break;
		case E_MODE: {
			char s[48];

			snprintf(s, sizeof(s), "< %s >", mode_names[editing.mode]);
			text(r.x + r.w - 32, r.y + 45, 22, ACCENT, RIGHT, s);
			break;
		}
		case E_SCREEN:
			text(r.x + r.w - 32, r.y + 45, 22, ACCENT, RIGHT,
			     editing.portrait ? "< Portrait >" : "< Landscape >");
			break;
		}
	}
}

/* ---------- help ---------- */

static const struct {
	int heading;
	const char *a, *b;
} help_lines[] = {
	{ 1, "Faster when overclocked", NULL },
	{ 0, "", "Pages load and scroll better with the CPU overclocked (sys-clk)" },
	{ 1, "In the browser", NULL },
	{ 0, "Touch", "Tap, drag to scroll, two fingers to zoom" },
	{ 0, "Right edge", "A finger there drags the scroll bar" },
	{ 0, "Left stick", "A mouse pointer: A clicks where it is" },
	{ 0, "Right stick", "Scrolls the page" },
	{ 0, "D-pad", "Moves from link to link" },
	{ 0, "A", "Opens the link, or the keyboard on a text field" },
	{ 0, "B / X", "Back / reload" },
	{ 0, "Y", "The keyboard, to type an address" },
	{ 0, "L / R", "Backspace / Enter" },
	{ 0, "+", "The menu" },
	{ 1, "The + menu", NULL },
	{ 0, "Full screen", "Hides the tabs and the address bar" },
	{ 0, "Screen", "Portrait: hold the console turned, its left side up" },
	{ 0, "Controls", "Browser, Keyboard & mouse or Controller (below)" },
	{ 0, "Keyboard", "Types text into the page" },
	{ 0, "Clipboard", "Copies what is selected, pastes where the text cursor is" },
	{ 0, "Leave", "Launcher comes back here, Quit closes Chromium NX" },
	{ 1, "Games", NULL },
	{ 0, "Keyboard & mouse", "The pad is a keyboard and a mouse; its keys are in Settings" },
	{ 0, "Controller", "Pages see an Xbox controller: the Switch's B is its A" },
	{ 0, "", "In that mode, hold + for the menu (a short press is Start)" },
	{ 1, "Files", NULL },
	{ 0, "Downloads", "Saved in /switch/chromium-nx/Downloads on the SD card" },
	{ 0, "Sending a file", "A page's file button shows the SD card's folders" },
};
#define HELP_COUNT ((int)(sizeof(help_lines) / sizeof(help_lines[0])))
static int help_scroll;

static int help_height(void)
{
	int h = 0;

	for (int i = 0; i < HELP_COUNT; i++)
		h += help_lines[i].heading ? (i ? 58 : 40) : 36;
	return h + 20;
}

static void draw_help(void)
{
	int y = SETTINGS_TOP + 20 - help_scroll;

	for (int i = 0; i < HELP_COUNT; i++) {
		if (help_lines[i].heading) {
			y += i ? 58 : 40;
			text(160, y, 22, ACCENT, LEFT, help_lines[i].a);
		} else {
			y += 36;
			text(180, y, 21, TEXT, LEFT, help_lines[i].a);
			text(440, y, 20, TEXT_DIM, LEFT, help_lines[i].b);
		}
	}
	background_band(0, SETTINGS_TOP);
	background_band(SETTINGS_BOTTOM, H);
	status_bar();
	line(64, 88, 82, 70, 5, TEXT);
	line(64, 88, 82, 106, 5, TEXT);
	text(104, 100, 36, TEXT, LEFT, "Help");
}

/* ---------- scrolling by touch ---------- */

static int scrolls(void)
{
	return !dialog && !picking &&
	       (screen == SETTINGS || screen == DOWNLOADS || screen == SITES || screen == HELP);
}

/* How far the screen's list is scrolled, in pixels. */
static int get_scroll(void)
{
	switch (screen) {
	case SETTINGS:
		return settings_scroll;
	case DOWNLOADS:
		return download_scroll * DOWNLOAD_ROW;
	case SITES:
		return site_scroll * SITE_ROW;
	case HELP:
		return help_scroll;
	}
	return 0;
}

static int clamp(int v, int max)
{
	return v < 0 || max <= 0 ? 0 : v > max ? max : v;
}

static void set_scroll(int px)
{
	follow_focus = 0;
	switch (screen) {
	case SETTINGS:
		settings_scroll = clamp(px, settings_top[ROWS - 1] + 98 - SETTINGS_BOTTOM);
		break;
	case DOWNLOADS:
		download_scroll = clamp((px + DOWNLOAD_ROW / 2) / DOWNLOAD_ROW, download_count - DOWNLOAD_ROWS);
		break;
	case SITES:
		site_scroll = clamp((px + SITE_ROW / 2) / SITE_ROW, (site_count + SITE_COLS) / SITE_COLS - SITE_ROWS);
		break;
	case HELP:
		help_scroll = clamp(px, help_height() - (SETTINGS_BOTTOM - SETTINGS_TOP));
		break;
	}
}

static void draw_dialog(void)
{
	char s[300];

	/* Darken what is under it. */
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			blend(x, y, RGB(0, 0, 0), 140);
	rrect(340, 230, 600, 290, 28, SURFACE);
	if (dialog == DIALOG_DELETE_SITE) {
		text(W / 2, 300, 32, TEXT, CENTER, "Delete this site?");
		text(W / 2, 350, 22, TEXT_DIM, CENTER, fit(editing.name, 22, 520));
		text(W / 2, 382, 22, TEXT_DIM, CENTER, "It goes from My sites.");
	} else if (dialog == DIALOG_DELETE) {
		text(W / 2, 300, 32, TEXT, CENTER, "Delete this file?");
		snprintf(s, sizeof(s), "%s", focus < download_count ? fit(downloads[focus].name, 22, 520) : "");
		text(W / 2, 350, 22, TEXT_DIM, CENTER, s);
		text(W / 2, 382, 22, TEXT_DIM, CENTER, "It is deleted from the SD card.");
	} else if (dialog == DIALOG_IMPORT) {
		text(W / 2, 300, 32, TEXT, CENTER, "Import the profile?");
		text(W / 2, 350, 22, TEXT_DIM, CENTER, "Browsing data, settings and sites now here");
		text(W / 2, 382, 22, TEXT_DIM, CENTER, "are replaced by those of the backup.");
	} else if (dialog == DIALOG_EXPORT) {
		text(W / 2, 300, 32, TEXT, CENTER, "Replace the backup?");
		text(W / 2, 350, 22, TEXT_DIM, CENTER, "There is one already in " BACKUP_SHOWN ".");
		text(W / 2, 382, 22, TEXT_DIM, CENTER, "It is replaced by what there is now.");
	} else if (dialog == DIALOG_RESET) {
		text(W / 2, 300, 32, TEXT, CENTER, "Reset the settings?");
		text(W / 2, 350, 22, TEXT_DIM, CENTER, "Settings and keys go back to their defaults.");
		text(W / 2, 382, 22, TEXT_DIM, CENTER, "Browsing data and sites are kept.");
	} else {
		text(W / 2, 300, 32, TEXT, CENTER, "Clear browsing data?");
		text(W / 2, 350, 22, TEXT_DIM, CENTER, "Cookies, history, cache and logins are deleted.");
		text(W / 2, 382, 22, TEXT_DIM, CENTER, "This cannot be undone.");
	}
	for (int i = 0; i < 2; i++) {
		struct spot b = dialog_spot(i);

		rrect(b.x, b.y, b.w, b.h, 28, dialog_focus == i ? (i ? RGB(0xF2, 0x8B, 0x82) : ACCENT) : SURFACE_HI);
		text(b.x + b.w / 2, b.y + 37, 24, dialog_focus == i ? ACCENT_DARK : TEXT, CENTER,
		     !i ? "Cancel" : dialog == DIALOG_CLEAR ? "Clear" : dialog == DIALOG_IMPORT ? "Import" :
		     dialog == DIALOG_EXPORT ? "Replace" : dialog == DIALOG_RESET ? "Reset" : "Delete");
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
	upd_missing = access(APP_DIR "/" UPDATE_IMAGE, F_OK) != 0;
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
			screen == KEYS ? "A  Change     Y  Reset all     B  Back" :
			screen == DOWNLOADS ? (download_count ? "A  Delete     B  Back" : "B  Back") :
			screen == SITES ? (focus ? "A  Open     X  Edit     B  Back" : "A  Add     B  Back") :
			screen == SITE_EDIT ? "A  Change     B  Cancel" :
			screen == HELP ? "B  Back" :
			"A  Change     B  Back     +  Exit";

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
	if (!remove(APP_DIR "/profile.ext4"))
		cleared = 1;
	say(cleared ? "Browsing data cleared" : "Nothing to clear");
}

static void reset_settings(void)
{
	memset(&settings, 0, sizeof(settings));
	settings.async_disk = settings.priorities = settings.hires = settings.check_updates = 1;
	for (int i = 0; i < GX_INPUTS; i++)
		settings.keys[i] = gx_inputs[i].default_key;
	save_settings();
	say("Settings reset");
}

/* Export and import: the files copied a piece at a time, a frame's worth
 * each frame, so that the screen says how far it has come. Each goes to a
 * file of its own first, which takes its name once it is whole. */
static const char *const backup_files[] = { "profile.ext4", "settings.ini", "sites.ini" };
#define BACKUP_FILES 3
#define COPY_CHUNK (1 << 20)

static struct {
	int active, importing, file;
	FILE *in, *out;
	uint64_t done, total;
	char *buffer;
} job;

static void job_path(char *out, size_t room, int to, int file)
{
	/* to: where the file goes (1), or where it comes from (0). */
	int backup = to != job.importing;

	snprintf(out, room, "%s/%s", backup ? BACKUP_DIR : APP_DIR, backup_files[file]);
}

static long long file_size(const char *path)
{
	struct stat st;

	return stat(path, &st) ? -1 : (long long)st.st_size;
}

static int backup_exists(void)
{
	char path[256];

	for (int i = 0; i < BACKUP_FILES; i++) {
		snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, backup_files[i]);
		if (file_size(path) >= 0)
			return 1;
	}
	return 0;
}

static void job_end(const char *said)
{
	char part[300];

	if (job.in)
		fclose(job.in);
	if (job.out) {
		fclose(job.out);
		job_path(part, sizeof(part) - 8, 1, job.file);
		strcat(part, ".part");
		remove(part);
	}
	job.in = job.out = NULL;
	free(job.buffer);
	job.buffer = NULL;
	job.active = 0;
	if (job.importing) {
		load_settings();
		load_sites();
	}
	say(said);
}

static void start_job(int importing)
{
	char path[300];

	memset(&job, 0, sizeof(job));
	job.importing = importing;
	for (int i = 0; i < BACKUP_FILES; i++) {
		long long n;

		job_path(path, sizeof(path), 0, i);
		n = file_size(path);
		if (n > 0)
			job.total += n;
	}
	if (!importing)
		mkdir(BACKUP_DIR, 0777);
	job.buffer = malloc(COPY_CHUNK);
	if (!job.buffer) {
		say("Not enough memory");
		return;
	}
	job.active = 1;
}

/* Each frame: a little more of the copy. */
static void job_step(void)
{
	char from[300], to[300], part[310];

	for (int pieces = 0; job.active && pieces < 16; pieces++) {
		if (job.file >= BACKUP_FILES) {
			job_end(job.importing ? "Profile imported" : "Profile exported to " BACKUP_SHOWN);
			return;
		}
		job_path(from, sizeof(from), 0, job.file);
		job_path(to, sizeof(to), 1, job.file);
		snprintf(part, sizeof(part), "%s.part", to);
		if (!job.in) {
			/* What the source has not, the copy has not either: it is
			 * what was there, in place of what there is. */
			if (!(job.in = fopen(from, "rb"))) {
				remove(to);
				job.file++;
				continue;
			}
			if (!(job.out = fopen(part, "wb"))) {
				job_end("Cannot write to the SD card");
				return;
			}
		}
		size_t n = fread(job.buffer, 1, COPY_CHUNK, job.in);

		if (n && fwrite(job.buffer, 1, n, job.out) != n) {
			job_end("Cannot write to the SD card (is it full?)");
			return;
		}
		job.done += n;
		if (n < COPY_CHUNK) {
			int failed = ferror(job.in);

			fclose(job.in);
			job.in = NULL;
			if (fclose(job.out) || failed) {
				job.out = NULL;
				remove(part);
				job_end(failed ? "Cannot read a file" : "Cannot write to the SD card");
				return;
			}
			job.out = NULL;
			remove(to);
			if (rename(part, to)) {
				job_end("Cannot write to the SD card");
				return;
			}
			job.file++;
		}
	}
}

static void draw_job(void)
{
	char s[120];
	uint64_t total = job.total ? job.total : 1;
	int w = (int)(640 * (job.done > total ? total : job.done) / total);

	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			blend(x, y, RGB(0, 0, 0), 140);
	rrect(220, 210, 840, 290, 28, SURFACE);
	text(W / 2, 282, 32, TEXT, CENTER, job.importing ? "Importing the profile" : "Exporting the profile");
	text(W / 2, 326, 22, TEXT_DIM, CENTER, job.file < BACKUP_FILES ? backup_files[job.file] : "");
	rrect(320, 360, 640, 16, 8, SURFACE_HI);
	if (job.done)
		rrect(320, 360, w < 16 ? 16 : w, 16, 8, ACCENT);
	snprintf(s, sizeof(s), "%s of %s", megabytes(job.done), megabytes(job.total));
	text(W / 2, 430, 22, TEXT_DIM, CENTER, s);
}

static int activate(void)
{
	char in[512] = "";

	if (dialog) {
		if (dialog_focus && dialog == DIALOG_DELETE) {
			delete_download(focus);
		} else if (dialog_focus && dialog == DIALOG_DELETE_SITE) {
			memmove(&sites[edit_index], &sites[edit_index + 1], (site_count - edit_index - 1) * sizeof(sites[0]));
			site_count--;
			save_sites();
			screen = SITES;
			focus = 0;
			follow_focus = 1;
			say("Site deleted");
		} else if (dialog_focus && dialog == DIALOG_IMPORT) {
			start_job(1);
		} else if (dialog_focus && dialog == DIALOG_EXPORT) {
			start_job(0);
		} else if (dialog_focus && dialog == DIALOG_RESET) {
			reset_settings();
		} else if (dialog_focus) {
			clear_data();
		}
		dialog = 0;
		return NONE;
	}
	if (screen == SITES) {
		if (!focus) {
			memset(&editing, 0, sizeof(editing));
			edit_index = -1;
			sites_focus = 0;
			screen = SITE_EDIT;
			focus = E_NAME;
			return NONE;
		}
		snprintf(go_url, sizeof(go_url), "%s", sites[focus - 1].url);
		go_full = sites[focus - 1].full;
		go_mode = sites[focus - 1].mode;
		go_portrait = sites[focus - 1].portrait;
		go_tv = 0;
		return GO;
	}
	if (screen == SITE_EDIT) {
		switch (focus) {
		case E_NAME:
			if (ask("Name", "The site's name", editing.name, in, sizeof(in)))
				clean_copy(editing.name, sizeof(editing.name), in);
			break;
		case E_URL:
			if (ask("Address", "https://...", editing.url[0] ? editing.url : NULL, in, sizeof(in))) {
				char url[600];

				snprintf(url, sizeof(url), "%s%s", strstr(in, "://") ? "" : "https://", in);
				clean_copy(editing.url, sizeof(editing.url), url);
			}
			break;
		case E_FULL:
			editing.full = !editing.full;
			break;
		case E_MODE:
			editing.mode = (editing.mode + 1) % 3;
			break;
		case E_SCREEN:
			editing.portrait = !editing.portrait;
			break;
		case E_SAVE:
			if (!editing.name[0] || !editing.url[0]) {
				say("A site needs a name and an address");
				break;
			}
			if (edit_index < 0 && site_count >= MAX_SITES) {
				say("There is no room for more sites");
				break;
			}
			if (edit_index < 0)
				edit_index = site_count++;
			sites[edit_index] = editing;
			save_sites();
			screen = SITES;
			focus = edit_index + 1;
			follow_focus = 1;
			break;
		case E_DELETE:
			dialog = DIALOG_DELETE_SITE;
			dialog_focus = 0;
			break;
		}
		return NONE;
	}
	if (screen == DOWNLOADS) {
		if (focus < download_count) {
			dialog = DIALOG_DELETE;
			dialog_focus = 0;
		}
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
		go_tv = focus == 1 + APP_YOUTUBE;
		go_full = go_mode = go_portrait = 0;
		follow_focus = 1;
		switch (focus - 1) {
		case -1:
			if (ask("Search", "Search or type a web address", NULL, in, sizeof(in))) {
				to_url(in, go_url, sizeof(go_url));
				return GO;
			}
			return NONE;
		case APP_CHROMIUM:
			go_url[0] = 0;
			return GO;
		case APP_YOUTUBE:
			snprintf(go_url, sizeof(go_url), "https://www.youtube.com/tv");
			return GO;
		case APP_SITES:
			load_sites();
			screen = SITES;
			focus = 0;
			site_scroll = 0;
			return NONE;
		case APP_HELP:
			screen = HELP;
			help_scroll = 0;
			return NONE;
		case APP_GITHUB:
			snprintf(go_url, sizeof(go_url), "https://github.com/" UPDATE_REPO);
			return GO;
		case APP_DOWNLOADS:
			load_downloads();
			screen = DOWNLOADS;
			focus = 0;
			return NONE;
		case APP_CHROMIUM_SETTINGS:
			snprintf(go_url, sizeof(go_url), "chrome://settings/");
			return GO;
		case APP_SETTINGS:
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
	case S_HIRES:
		settings.hires = !settings.hires;
		break;
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
		dialog = DIALOG_CLEAR;
		dialog_focus = 0;
		return NONE;
	case S_EXPORT:
		if (backup_exists()) {
			dialog = DIALOG_EXPORT;
			dialog_focus = 0;
		} else {
			start_job(0);
		}
		return NONE;
	case S_IMPORT:
		if (backup_exists()) {
			dialog = DIALOG_IMPORT;
			dialog_focus = 0;
		} else {
			say("No backup in " BACKUP_SHOWN);
		}
		return NONE;
	case S_RESET:
		dialog = DIALOG_RESET;
		dialog_focus = 0;
		return NONE;
	}
	save_settings();
	return NONE;
}

/* The site at row i of the list, to the form. */
static void edit_site(int i)
{
	if (i < 1 || i > site_count)
		return;
	editing = sites[i - 1];
	edit_index = i - 1;
	screen = SITE_EDIT;
	focus = E_NAME;
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
	} else if (screen == DOWNLOADS) {
		screen = HOME;
		focus = 1 + APP_DOWNLOADS;
	} else if (screen == SITE_EDIT) {
		screen = SITES;
		focus = edit_index >= 0 ? edit_index + 1 : 0;
		follow_focus = 1;
	} else if (screen == SITES) {
		screen = HOME;
		focus = 1 + APP_SITES;
	} else if (screen == HELP) {
		screen = HOME;
		focus = 1 + APP_HELP;
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
	follow_focus = 1;
	if (screen == HELP) {
		set_scroll(help_scroll + dy * 72);
		return;
	}
	if (screen == SITES) {
		int to = focus + dx + dy * SITE_COLS;

		if (to >= 0 && to <= site_count && (dy || to / SITE_COLS == focus / SITE_COLS))
			focus = to;
		else if (dy > 0 && focus / SITE_COLS < site_count / SITE_COLS)
			focus = site_count;   /* the last row, shorter */
		return;
	}
	if (screen == SITE_EDIT) {
		if (dy)
			focus = focus + dy < 0 ? 0 : focus + dy >= edit_rows() ? edit_rows() - 1 : focus + dy;
		else if (dx && focus == E_MODE)
			editing.mode = (editing.mode + 3 + dx) % 3;
		else if (dx && focus == E_FULL)
			editing.full = dx > 0;
		else if (dx && focus == E_SCREEN)
			editing.portrait = !editing.portrait;
		return;
	}
	if (screen == DOWNLOADS) {
		if (dy)
			focus = focus + dy < 0 ? 0 : focus + dy >= download_count ? download_count - 1 : focus + dy;
		if (focus < 0)
			focus = 0;
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
	if (screen == HELP) {
		if (x < 140 && y < 150)
			back();
		return NONE;
	}
	if (screen == SITES) {
		if (x < 140 && y < 150) {
			back();
			return NONE;
		}
		for (int i = 0; i <= site_count && y < SETTINGS_BOTTOM - 8; i++) {
			if (!site_seen(i))
				continue;
			if (inside(site_tile(i), x, y)) {
				focus = i;
				return activate();
			}
		}
		return NONE;
	}
	if (screen == SITE_EDIT) {
		if (x < 140 && y < 150) {
			back();
			return NONE;
		}
		for (int i = 0; i < edit_rows(); i++)
			if (inside(edit_row(i), x, y)) {
				focus = i;
				return activate();
			}
		return NONE;
	}
	if (screen == DOWNLOADS) {
		if (x < 140 && y < 150) {
			back();
			return NONE;
		}
		for (int i = 0; i < download_count; i++)
			if (inside(download_cross(i), x, y)) {
				focus = i;
				return activate();
			}
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
	int touching = 0, result = NONE, touch_x0 = 0, touch_y0 = 0, dragged = 0, scroll0 = 0, deferred = 0;
	int held = 0;   /* the finger was held long enough to do what that does */
	u64 touch_t0 = 0;
	FILE *old;

	/* Left by an older version, when the browser was a program of its own
	 * and its command line a file the build wrote (not DEV=1's menu). */
	remove(APP_DIR "/gx.nro");
	if ((old = fopen(APP_DIR "/programs.txt", "r"))) {
		char first[32] = "";
		int built = fgets(first, sizeof(first), old) && !strncmp(first, "[Chromium NX]", 13);

		fclose(old);
		if (built)
			remove(APP_DIR "/programs.txt");
	}
	/* A launch asked for and not carried out (a test writes one): it is. */
	if (!access(APP_DIR "/launch.txt", F_OK) && !start_browser())
		return 0;

	plInitialize(PlServiceType_User);
	psmInitialize();
	/* Any number of players, so that Joy-Con taken off, put back or paired
	 * while we run are let in (with one, only the HOME menu takes them);
	 * all of them read as one pad. */
	padConfigureInput(8, HidNpadStyleSet_NpadStandard);
	padInitializeAny(&pad);
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
	else if (access(APP_DIR "/" UPDATE_IMAGE, F_OK))
		show_image_dialog();

	while (appletMainLoop()) {
		HidTouchScreenState touch = { 0 };
		u64 down;

		padUpdate(&pad);
		down = padGetButtonsDown(&pad);
		if (job.active) {
			/* An export or import has the pad and the screen. */
			job_step();
			down = 0;
		}
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
		if ((down & HidNpadButton_X) && screen == SITES && !dialog)
			edit_site(focus);
		if ((down & HidNpadButton_X) && screen == HOME && !dialog) {
			screen = SETTINGS;
			focus = 0;
		}
		if (down & HidNpadButton_A)
			result = activate();
		/* A finger: on a list that scrolls, a tap is a finger lifted where
		 * it went down, and one that moved scrolls the list; elsewhere, a
		 * tap is a finger put down. */
		if (job.active) {
			touching = 0;
		} else if (!upd && hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
			int x = touch.touches[0].x, y = touch.touches[0].y;

			if (!touching) {
				touching = 1;
				touch_x0 = x;
				touch_y0 = y;
				dragged = 0;
				held = 0;
				touch_t0 = armGetSystemTick();
				scroll0 = get_scroll();
				deferred = scrolls();
				if (!deferred)
					result = tap(x, y);
			} else if (deferred) {
				if (abs(y - touch_y0) > 12 || abs(x - touch_x0) > 12)
					dragged = 1;
				if (dragged)
					set_scroll(scroll0 - (y - touch_y0));
				/* Held on a site without moving: its form. */
				if (!dragged && !held && screen == SITES &&
				    armTicksToNs(armGetSystemTick() - touch_t0) > 500000000ULL) {
					held = 1;
					for (int i = 1; i <= site_count && touch_y0 < SETTINGS_BOTTOM - 8; i++)
						if (site_seen(i) && inside(site_tile(i), touch_x0, touch_y0)) {
							focus = i;
							edit_site(i);
						}
				}
			}
		} else if (!upd) {
			if (touching && deferred && !dragged && !held)
				result = tap(touch_x0, touch_y0);
			touching = 0;
		}
		/* No browser without its image: what to do about it instead. */
		if (result == GO && access(APP_DIR "/" UPDATE_IMAGE, F_OK)) {
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
		else if (screen == DOWNLOADS)
			draw_downloads();
		else if (screen == SITES)
			draw_sites();
		else if (screen == SITE_EDIT)
			draw_site_edit();
		else if (screen == HELP)
			draw_help();
		else
			draw_settings();
		if (picking)
			draw_picker();
		draw_hints();
		if (dialog)
			draw_dialog();
		if (upd)
			draw_update();
		if (job.active)
			draw_job();
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
