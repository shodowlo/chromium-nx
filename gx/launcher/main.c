/*
 * The launcher: what the Chromium NX tile opens. A home screen in the manner
 * of Android's (a clock, a search bar, a row of apps) and the settings; what
 * is chosen becomes the browser's command line, written to launch.txt, and
 * the Homebrew loader is asked to load gx.nro (the browser) once this program
 * has ended. Nothing of the launcher is left when the browser starts: it is
 * another program loaded into the process afresh, with all of its memory.
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

#define W 1280
#define H 720
#define DIR "sdmc:/switch/chromium-nx"
#define VERSION "0.2"

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

static void background(void)
{
	for (int y = 0; y < H; y++) {
		int t = y * 255 / (H - 1);
		u32 c = RGB((C_R(BG_TOP) * (255 - t) + C_R(BG_BOTTOM) * t) / 255,
			    (C_G(BG_TOP) * (255 - t) + C_G(BG_BOTTOM) * t) / 255,
			    (C_B(BG_TOP) * (255 - t) + C_B(BG_BOTTOM) * t) / 255);
		u32 *row = &fbuf[y * fstride];

		for (int x = 0; x < W; x++)
			row[x] = c;
	}
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
} settings = { 0, 1, 1 };

static void load_settings(void)
{
	FILE *f = fopen(DIR "/settings.ini", "r");
	char line[600];

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
	}
	if (f)
		fclose(f);
}

static void save_settings(void)
{
	FILE *f = fopen(DIR "/settings.ini", "w");

	if (!f)
		return;
	fprintf(f, "diagnostics=%d\nasync_disk=%d\npriorities=%d\n",
		settings.diagnostics, settings.async_disk, settings.priorities);
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

/* The browser's command line is the first line of programs.txt, made by the
 * build; the settings change what goes in front of it and its last word,
 * the page to open (none: Chromium's own start page). */
static int launch(const char *url)
{
	static char cmd[4096], out[4600];
	FILE *f = fopen(DIR "/programs.txt", "r");
	char *p, *last, opts[32] = "";
	int found = 0;

	while (f && fgets(cmd, sizeof(cmd), f)) {
		cmd[strcspn(cmd, "\r\n")] = 0;
		/* The browser's line: with DEV=1 test programs come first. */
		if (cmd[0] && cmd[0] != '#' && strstr(cmd, "/chromium ") && !strstr(cmd, "--disable-gpu ")) {
			found = 1;
			break;
		}
	}
	if (f)
		fclose(f);
	if (!found) {
		snprintf(launch_error, sizeof(launch_error), "programs.txt is missing");
		return -1;
	}
	p = cmd;
	if (*p == '[' && strchr(p, ']'))
		p = strchr(p, ']') + 1;
	while (*p == ' ')
		p++;
	last = strrchr(p, ' ');
	if (last && (!strncmp(last + 1, "http://", 7) || !strncmp(last + 1, "https://", 8)))
		*last = 0;
	if (settings.diagnostics)
		strcat(opts, "-p ");
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
	if (R_FAILED(envSetNextLoad(DIR "/gx.nro", DIR "/gx.nro"))) {
		remove(DIR "/launch.txt");
		snprintf(launch_error, sizeof(launch_error), "the loader cannot start gx.nro");
		return -1;
	}
	return 0;
}

/* ---------- screens ---------- */

enum { HOME, SETTINGS };
static int screen = HOME, focus = 1, dialog, dialog_focus;
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

#define APPS 4
static const char *app_names[APPS] = { "Chromium", "YouTube", "Google", "Settings" };

static struct spot home_spot(int i)
{
	if (i == 0)
		return (struct spot){ 240, 300, 800, 72 };
	int span = 200, x0 = W / 2 - APPS * span / 2;

	return (struct spot){ x0 + (i - 1) * span + 30, 440, 140, 170 };
}

static void draw_app_icon(int i, int cx, int cy)
{
	switch (i) {
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
	case 3: { /* Settings: a gear */
		circle(cx, cy, 50, RGB(0x5F, 0x63, 0x68));
		for (int k = 0; k < 8; k++) {
			float a = k * (float)M_PI / 4;

			line(cx + cosf(a) * 18, cy + sinf(a) * 18, cx + cosf(a) * 30, cy + sinf(a) * 30, 11, TEXT);
		}
		circle(cx, cy, 22, TEXT);
		circle(cx, cy, 9, RGB(0x5F, 0x63, 0x68));
		break;
	}
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
		text(cx, a.y + 150, 24, focus == i + 1 ? TEXT : TEXT_DIM, CENTER, app_names[i]);
	}
}

/* Settings: rows, some under a heading. */
enum { S_DIAG, S_DISK, S_PRIO, S_CLEAR, ROWS };

static int row_y(int i)
{
	static const int y[ROWS] = { 156, 236, 316, 446 };

	return y[i];
}

static struct spot settings_spot(int i)
{
	return (struct spot){ 160, row_y(i), 960, 78 };
}

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

	text(192, row_y(S_DIAG) - 14, 20, ACCENT, LEFT, "Advanced");
	row(S_DIAG, "Diagnostics", "Profiler and network errors in chromium-nx.log");
	switch_widget(1024, row_y(S_DIAG) + 21, settings.diagnostics);
	row(S_DISK, "Background disk reads", "The SD card is read without stopping other work");
	switch_widget(1024, row_y(S_DISK) + 21, settings.async_disk);
	row(S_PRIO, "Thread priorities", "Display, video and sound go before page scripts");
	switch_widget(1024, row_y(S_PRIO) + 21, settings.priorities);

	text(192, row_y(S_CLEAR) - 14, 20, ACCENT, LEFT, "Data");
	row(S_CLEAR, "Clear browsing data", "Cookies, history, cache and logins");

	text(W - 40, 98, 20, TEXT_DIM, RIGHT,
	     "Chromium NX " VERSION "  \xC2\xB7  Chromium 156 on Linux (LKL)");
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

static void draw_hints(void)
{
	const char *h = dialog ? "A  Select     B  Cancel" :
			screen == HOME ? "A  Open     X  Settings     +  Exit" : "A  Change     B  Back     +  Exit";

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
	if (screen == HOME) {
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
			snprintf(go_url, sizeof(go_url), "https://www.youtube.com/");
			return GO;
		case 3:
			snprintf(go_url, sizeof(go_url), "https://www.google.com/");
			return GO;
		case 4:
			screen = SETTINGS;
			focus = 0;
			return NONE;
		}
		return NONE;
	}
	switch (focus) {
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
	else if (screen == SETTINGS) {
		screen = HOME;
		focus = 4;
	}
}

static void move(int dx, int dy)
{
	if (dialog) {
		if (dx)
			dialog_focus = dx > 0;
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
	for (int i = 0; i < ROWS; i++)
		if (inside(settings_spot(i), x, y)) {
			focus = i;
			return activate();
		}
	return NONE;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
	Framebuffer fb;
	PadState pad;
	PlFontData font;
	int touching = 0, result = NONE;

	plInitialize(PlServiceType_User);
	psmInitialize();
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	hidInitializeTouchScreen();
	load_settings();

	if (FT_Init_FreeType(&ft) || R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) ||
	    FT_New_Memory_Face(ft, font.address, font.size, 0, &face)) {
		/* No font: straight to the browser, as before the launcher. */
		result = launch("") ? QUIT : GO;
		goto out;
	}
	load_icon();
	framebufferCreate(&fb, nwindowGetDefault(), W, H, PIXEL_FORMAT_RGBA_8888, 2);
	framebufferMakeLinear(&fb);

	while (appletMainLoop()) {
		HidTouchScreenState touch = { 0 };
		u64 down;

		padUpdate(&pad);
		down = padGetButtonsDown(&pad);
		if (down & HidNpadButton_Plus) {
			result = QUIT;
			break;
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
		if ((down & HidNpadButton_X) && screen == HOME && !dialog) {
			screen = SETTINGS;
			focus = 0;
		}
		if (down & HidNpadButton_A)
			result = activate();
		if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
			if (!touching)
				result = tap(touch.touches[0].x, touch.touches[0].y);
			touching = 1;
		} else {
			touching = 0;
		}

		fbuf = framebufferBegin(&fb, &fstride);
		fstride /= 4;
		if (result == GO) {
			draw_starting();
			framebufferEnd(&fb);
			if (!launch(go_url))
				break;
			say(launch_error);
			result = NONE;
			continue;
		}
		background();
		status_bar();
		if (screen == HOME)
			draw_home();
		else
			draw_settings();
		draw_hints();
		if (dialog)
			draw_dialog();
		draw_toast();
		framebufferEnd(&fb);
	}
	framebufferClose(&fb);
out:
	/* Everything given back: the browser is loaded into this process next. */
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
