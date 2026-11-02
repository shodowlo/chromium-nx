/*
 * cdp.c - a screen and inputs for a headless Chromium.
 *
 * chromium-headless-shell has no window, but it can be driven through the
 * Chrome DevTools Protocol: with --remote-debugging-pipe it reads commands on
 * its descriptor 3 and writes answers and events on 4, as JSON texts ended by
 * a zero byte. One host thread holds the other ends of those pipes (they are
 * the kernel's, so it talks through LKL like the guest), asks for a
 * "screencast" of the page, hands each picture to the host to show, and
 * turns what the user does into input events of the protocol.
 *
 * Only what is needed is read out of the JSON, by looking for the keys: the
 * messages are Chromium's own and their shape is known.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lkl.h>
#include "gx.h"
#include "cdp.h"

#define MESSAGE_MAX (8UL << 20)
#define HOST_READ_FD 250    /* our ends, out of the way of the guest's */
#define HOST_WRITE_FD 251

static int next_id = 1;
static int window;          /* Chromium shows itself: see cdp_start() */
static int ask, wanted, page_action;   /* between wl.c's thread and ours */
static int repaint = -1;               /* cdp_repaint(): 1 on, 0 off, -1 nothing to do */
/* A page's file chooser (<input type=file>): Chromium has no dialog of its
 * own to show here, so it says when one is wanted (Page.fileChooserOpened)
 * and the file is given back to it (cdp_file_chosen()). */
static char chooser_session[64];
static int chooser_node, chooser_wanted, chosen_ready;
static char chosen[1024];
static char start_url[1024];   /* cdp_start_url() */
/* With a window there may be several pages, one per tab, each with a session
 * of its own; they come and go as tabs do. */
#define MAX_PAGES 16
static char pages[MAX_PAGES][128];
static int page_count;
static uint64_t ask_deadline, load_started;

/*
 * What every page is given when Chromium has a window: a way to tell whether
 * what has the focus is text to edit, and a word to us (gxEdit, a binding)
 * when the user touches such text, which is when the keyboard should come up
 * without being asked for.
 */
static const char page_script[] =
	/* What is added to the page does not show: names that are not
	 * enumerated, and functions put in place of the browser's own that say
	 * they are its own when asked (Function.prototype.toString). Sites that
	 * look for a robot (YouTube) look for such things. */
	"(function(){if(window.__gx)return;"
	"Object.defineProperty(window,'__gx',{value:1});"
	"var nat=new WeakMap(),fts=Function.prototype.toString;"
	"function hide(w,g){nat.set(w,g);try{Object.defineProperty(w,'name',{value:g.name});"
	"Object.defineProperty(w,'length',{value:g.length})}catch(e){}return w}"
	"Function.prototype.toString=hide(function toString(){return fts.call(nat.get(this)||this)},fts);"
	"function ed(e){if(!e)return false;if(e.isContentEditable)return true;"
	"if(e.readOnly||e.disabled)return false;if(e.tagName=='TEXTAREA')return true;"
	"if(e.tagName!='INPUT')return false;"
	"return['text','search','url','email','password','tel','number'].indexOf(e.type)>=0}"
	"Object.defineProperty(window,'__gxText',{value:function(){var a=document.activeElement;"
	"while(a&&a.shadowRoot&&a.shadowRoot.activeElement)a=a.shadowRoot.activeElement;return ed(a)}});"
	"document.addEventListener('click',function(v){"
	"if(v.isTrusted&&v.pointerType=='touch'&&window.__gxText()&&window.gxEdit)gxEdit('touch')},true);"
	/* Video is decoded by the processor here, and VP9 and AV1 cost it far
	 * more than H.264: a page that asks is told they cannot be played, and
	 * serves H.264 instead (what the h264ify extension does). */
	"var no=/vp8|vp9|vp09|av01/i;"
	/* Nor anything taller than the screen: every picture is copied out of
	 * the decoder and into the GPU, for nothing if it is then shrunk. */
	"function tall(t){var m=/height=(\\d+)/.exec(t);return m&&+m[1]>720}"
	"function not(o,f){var g=o&&o[f];if(g)o[f]=hide(function(t){return no.test(t)||tall(t)?"
	"(f=='canPlayType'?'':false):g.apply(this,arguments)},g)}"
	"not(window.MediaSource,'isTypeSupported');not(window.ManagedMediaSource,'isTypeSupported');"
	"not(HTMLMediaElement.prototype,'canPlayType');"
	"var mc=navigator.mediaCapabilities,di=mc&&mc.decodingInfo;"
	"if(di)mc.decodingInfo=hide(function(c){var v=c&&c.video;"
	"if(v&&(no.test(v.contentType)||v.height>720))"
	"return Promise.resolve({supported:false,smooth:false,powerEfficient:false});"
	"return di.apply(this,arguments)},di)"
	"})()";
static char session[128];   /* the page we are attached to, once we are */

/*
 * How smooth it is, written to the log every few seconds: how many pictures
 * came, how large, and where the time of each went. "pipe" is from the first
 * byte of a picture to its last, "show" is the host decoding and displaying
 * it, and "wait" is from our acknowledgement (sent as soon as a picture is
 * complete) to the first byte of the next one, which is Chromium drawing and
 * compressing when the page is changing (and nothing at all when it is not:
 * the longest waits are the user reading).
 */
#define STATS_PERIOD_NS 5000000000ULL

int cdp_frames_per_second;
int cdp_network_log;

static struct {
	uint64_t since, second_since;
	uint64_t first_byte, acked;
	int frames, second_frames, inputs;
	uint64_t bytes, pipe_ns, show_ns, show_max, wait_ns, wait_max;
} stats;

static void stats_frame(size_t bytes, uint64_t complete, uint64_t shown)
{
	uint64_t wait = stats.acked && stats.first_byte > stats.acked ? stats.first_byte - stats.acked : 0;

	stats.frames++;
	stats.second_frames++;
	stats.bytes += bytes;
	stats.pipe_ns += complete - stats.first_byte;
	stats.show_ns += shown - complete;
	if (shown - complete > stats.show_max)
		stats.show_max = shown - complete;
	stats.wait_ns += wait;
	if (wait > stats.wait_max)
		stats.wait_max = wait;
	stats.acked = complete;
}

static void stats_tick(uint64_t now)
{
	if (!stats.since)
		stats.since = stats.second_since = now;
	if (now - stats.second_since >= 1000000000ULL) {
		cdp_frames_per_second = stats.second_frames;
		stats.second_frames = 0;
		stats.second_since = now;
	}
	if (now - stats.since < STATS_PERIOD_NS)
		return;
	if (stats.frames) {
		int n = stats.frames;

		host_log("gx: %d pictures in 5 s (%d.%d per second), %lu KB each; per picture: "
			 "pipe %lu ms, show %lu ms (%lu at most), wait %lu ms (%lu at most); %d inputs\n",
			 n, n / 5, n * 2 % 10, (unsigned long)(stats.bytes / n >> 10),
			 (unsigned long)(stats.pipe_ns / n / 1000000),
			 (unsigned long)(stats.show_ns / n / 1000000),
			 (unsigned long)(stats.show_max / 1000000),
			 (unsigned long)(stats.wait_ns / n / 1000000),
			 (unsigned long)(stats.wait_max / 1000000), stats.inputs);
	}
	stats.since = now;
	stats.frames = stats.inputs = 0;
	stats.bytes = stats.pipe_ns = stats.show_ns = stats.show_max = 0;
	stats.wait_ns = stats.wait_max = 0;
}

static void send_text(const char *text, size_t len)
{
	for (size_t done = 0; done < len + 1;) {   /* with its final zero */
		long n = lkl_sys_write(HOST_WRITE_FD, text + done, len + 1 - done);

		if (n <= 0)
			return;
		done += n;
	}
}

/* Sends a command to a session, or to the browser itself if to is "". */
static void send_command(const char *to, const char *method, const char *params)
{
	char text[5000];
	int n;

	if (to[0])
		n = snprintf(text, sizeof(text), "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"%s\",\"params\":%s}",
			     next_id++, to, method, params);
	else
		n = snprintf(text, sizeof(text), "{\"id\":%d,\"method\":\"%s\",\"params\":%s}",
			     next_id++, method, params);
	if (n > 0 && n < (int)sizeof(text))
		send_text(text, n);
}

/* Sends a command to the page (or to the browser before there is a page). */
static void command(const char *method, const char *params_fmt, ...)
{
	char params[3072] = "{}";
	va_list ap;

	if (params_fmt) {
		va_start(ap, params_fmt);
		vsnprintf(params, sizeof(params), params_fmt, ap);
		va_end(ap);
	}
	send_command(session, method, params);
}

/* Sends a command to every page there is. */
static void page_command(const char *method, const char *params)
{
	for (int i = 0; i < MAX_PAGES; i++)
		if (pages[i][0])
			send_command(pages[i], method, params);
}

/* Copies the string value of "key" out of a message; returns its length, or
 * -1. Enough for values without escapes: identifiers, base64. */
static long string_value(const char *message, const char *key, char *out, size_t room)
{
	char pattern[64];
	const char *start, *end;

	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	start = strstr(message, pattern);
	if (!start)
		return -1;
	start += strlen(pattern);
	end = strchr(start, '"');
	if (!end || (size_t)(end - start) >= room)
		return -1;
	memcpy(out, start, end - start);
	out[end - start] = 0;
	return end - start;
}

/* Decodes base64 in place; returns the number of bytes. */
static size_t base64_decode(char *text, size_t len)
{
	static signed char value[256];
	uint8_t *out = (uint8_t *)text;
	uint32_t bits = 0;
	int count = 0;

	if (!value['B']) {
		static const char alphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		memset(value, -1, sizeof(value));
		for (int i = 0; i < 64; i++)
			value[(unsigned char)alphabet[i]] = i;
	}
	for (size_t i = 0; i < len; i++) {
		int v = value[(unsigned char)text[i]];

		if (v < 0)
			continue;
		bits = bits << 6 | v;
		if (++count == 4) {
			*out++ = bits >> 16;
			*out++ = bits >> 8;
			*out++ = bits;
			count = 0;
		}
	}
	if (count == 3) {
		*out++ = bits >> 10;
		*out++ = bits >> 2;
	} else if (count == 2) {
		*out++ = bits >> 4;
	}
	return out - (uint8_t *)text;
}

/* Writes text as the inside of a JSON string. */
static void json_escape(char *out, size_t room, const char *text)
{
	size_t n = 0;

	for (; *text && n + 7 < room; text++) {
		unsigned char c = *text;

		if (c == '"' || c == '\\') {
			out[n++] = '\\';
			out[n++] = c;
		} else if (c < 0x20) {
			n += snprintf(out + n, room - n, "\\u%04x", c);
		} else {
			out[n++] = c;
		}
	}
	out[n] = 0;
}

/* With a window: pages that appear are given the script, and what they say
 * is passed on to wl.c. */
static void handle_window_message(const char *message)
{
	char id[128];

	if (strstr(message, "\"method\":\"Target.attachedToTarget\"")) {
		char script[4096], params[4300];
		int slot = -1;

		if (!strstr(message, "\"type\":\"page\"") ||
		    string_value(message, "sessionId", id, sizeof(id)) <= 0)
			return;
		for (int i = 0; i < MAX_PAGES; i++) {
			if (!strcmp(pages[i], id))
				return;
			if (slot < 0 && !pages[i][0])
				slot = i;
		}
		if (slot < 0)
			return;
		strcpy(pages[slot], id);
		json_escape(script, sizeof(script), page_script);
		/* No Runtime.enable: bindings and evaluations work without it, and
		 * sites that look for a robot (YouTube) can tell it is on. */
		send_command(id, "Runtime.addBinding", "{\"name\":\"gxEdit\"}");
		send_command(id, "Page.enable", "{}");
		send_command(id, "Page.setInterceptFileChooserDialog", "{\"enabled\":true}");
		/* For the log: requests that fail or are refused (a video that
		 * stops is a server saying no, or a connection that broke). */
		if (cdp_network_log) {
			send_command(id, "Network.enable",
				     "{\"maxTotalBufferSize\":0,\"maxResourceBufferSize\":0}");
			/* And what media players say (chrome://media-internals). */
			send_command(id, "Media.enable", "{}");
		}
		snprintf(params, sizeof(params), "{\"source\":\"%s\"}", script);
		send_command(id, "Page.addScriptToEvaluateOnNewDocument", params);
		snprintf(params, sizeof(params), "{\"expression\":\"%s\"}", script);
		send_command(id, "Runtime.evaluate", params);
		/* The first page has the keyboard from the start, not the
		 * address above it. */
		if (!__atomic_fetch_add(&page_count, 1, __ATOMIC_SEQ_CST)) {
			send_command(id, "Page.bringToFront", "{}");
			/* The page to start on that the command line could not
			 * give (chrome:// pages are refused there on Linux). */
			if (start_url[0]) {
				char url[1100];

				json_escape(url, sizeof(url), start_url);
				snprintf(params, sizeof(params), "{\"url\":\"%s\"}", url);
				send_command(id, "Page.navigate", params);
				start_url[0] = 0;
			}
		}
	} else if (strstr(message, "\"method\":\"Target.detachedFromTarget\"")) {
		if (string_value(message, "sessionId", id, sizeof(id)) <= 0)
			return;
		for (int i = 0; i < MAX_PAGES; i++)
			if (!strcmp(pages[i], id))
				pages[i][0] = 0;
	} else if (strstr(message, "\"method\":\"Page.fileChooserOpened\"")) {
		const char *node = strstr(message, "\"backendNodeId\":");

		if (node && string_value(message, "sessionId", chooser_session, sizeof(chooser_session)) > 0) {
			chooser_node = atoi(node + 16);
			__atomic_store_n(&chooser_wanted, 1, __ATOMIC_SEQ_CST);
		}
	} else if (strstr(message, "\"method\":\"Network.responseReceived\"")) {
		const char *status = strstr(message, "\"status\":");
		char url[160];

		if (status && atoi(status + 9) >= 400 &&
		    string_value(message, "url", url, sizeof(url)) > 0)
			host_log("gx: http %d: %.120s\n", atoi(status + 9), url);
		else if (status && atoi(status + 9) >= 400)
			host_log("gx: http %d\n", atoi(status + 9));
	} else if (strstr(message, "\"method\":\"Media.playerErrorsRaised\"") ||
		   (strstr(message, "\"method\":\"Media.playerMessages\"") &&
		    strstr(message, "\"level\":\"error\""))) {
		const char *params = strstr(message, "\"params\":");

		host_log("gx: media: %.400s\n", params ? params + 9 : message);
	} else if (strstr(message, "\"method\":\"Media.playerPropertiesChanged\"") &&
		   (strstr(message, "kVideoDecoderName") || strstr(message, "kResolution") ||
		    strstr(message, "kIsVideoDecryptingDemuxerStream") || strstr(message, "kVideoTracks"))) {
		const char *params = strstr(message, "\"properties\":");

		host_log("gx: media: %.300s\n", params ? params + 13 : message);
	} else if (strstr(message, "\"method\":\"Network.loadingFailed\"")) {
		char error[96];

		if (string_value(message, "errorText", error, sizeof(error)) > 0 &&
		    !strstr(message, "\"canceled\":true"))
			host_log("gx: a request failed: %s (%s)\n", error,
				 strstr(message, "\"type\":\"Media\"") ? "media" :
				 strstr(message, "\"type\":\"XHR\"") ? "xhr" :
				 strstr(message, "\"type\":\"Fetch\"") ? "fetch" : "other");
	} else if (strstr(message, "\"method\":\"Page.frameStartedLoading\"")) {
		/* How long pages take to load, for the log: from the first frame
		 * that starts loading to the page's load event. */
		if (!load_started)
			load_started = host_now_ns();
	} else if (strstr(message, "\"method\":\"Page.domContentEventFired\"")) {
		if (load_started)
			host_log("gx: page: DOM ready after %lu ms\n",
				 (unsigned long)((host_now_ns() - load_started) / 1000000));
	} else if (strstr(message, "\"method\":\"Page.loadEventFired\"")) {
		if (load_started)
			host_log("gx: page: loaded in %lu ms\n",
				 (unsigned long)((host_now_ns() - load_started) / 1000000));
		load_started = 0;
	} else if (strstr(message, "\"method\":\"Runtime.bindingCalled\"") &&
		   strstr(message, "\"name\":\"gxEdit\"")) {
		__atomic_store_n(&wanted, CDP_KEYBOARD, __ATOMIC_SEQ_CST);
	} else if (ask_deadline && strstr(message, "\"value\":\"gx-text\"")) {
		/* Only while the question is open: an answer that comes after
		 * we stopped waiting has been acted on already. */
		ask_deadline = 0;
		__atomic_store_n(&wanted, CDP_KEYBOARD, __ATOMIC_SEQ_CST);
	} else if (ask_deadline && strstr(message, "\"value\":\"gx-other\"")) {
		ask_deadline = 0;
		__atomic_store_n(&wanted, CDP_ENTER, __ATOMIC_SEQ_CST);
	}
}

static void handle_message(char *message, size_t len)
{
	static char frame_session[32];
	char *data;

	if (window) {
		handle_window_message(message);
		return;
	}
	if (!session[0]) {
		/* The answer to Target.getTargets, or to Target.attachToTarget. */
		char target[128];
		const char *page = strstr(message, "\"type\":\"page\"");

		if (string_value(message, "sessionId", session, sizeof(session)) > 0) {
			command("Page.enable", NULL);
			command("Page.startScreencast",
				"{\"format\":\"jpeg\",\"quality\":70,\"maxWidth\":1280,\"maxHeight\":720}");
		} else if (page) {
			/* The identifier comes first in each description. */
			const char *start = page;

			while (start > message && strncmp(start, "\"targetId\":\"", 12))
				start--;
			if (string_value(start, "targetId", target, sizeof(target)) > 0)
				command("Target.attachToTarget", "{\"targetId\":\"%s\",\"flatten\":true}",
					target);
		}
		return;
	}
	if (!strstr(message, "\"method\":\"Page.screencastFrame\""))
		return;
	/* The picture, and the number to acknowledge it with (the "sessionId"
	 * inside the parameters, a number, unlike the page's which is text). */
	data = strstr(message, "\"data\":\"");
	if (data) {
		const char *number = strstr(message, "\"sessionId\":");
		char *end;

		while (number && number[12] == '"')
			number = strstr(number + 12, "\"sessionId\":");
		data += 8;
		end = strchr(data, '"');
		if (number)
			snprintf(frame_session, sizeof(frame_session), "%d", atoi(number + 12));
		if (end) {
			uint64_t complete = host_now_ns();
			size_t bytes;

			/* Acknowledged before it is shown: Chromium then draws the
			 * next picture while this one is decoded and displayed,
			 * instead of after. */
			command("Page.screencastFrameAck", "{\"sessionId\":%s}", frame_session);
			bytes = base64_decode(data, end - data);
			host_frame((const uint8_t *)data, bytes);
			stats_frame(bytes, complete, host_now_ns());
		}
	}
}

static void handle_input(const struct cdp_input *in)
{
	char text[768];

	switch (in->type) {
	case CDP_TOUCH_DOWN:
	case CDP_TOUCH_MOVE:
		command("Input.dispatchTouchEvent", "{\"type\":\"%s\",\"touchPoints\":[{\"x\":%d,\"y\":%d}]}",
			in->type == CDP_TOUCH_DOWN ? "touchStart" : "touchMove", in->x, in->y);
		break;
	case CDP_TOUCH_UP:
		command("Input.dispatchTouchEvent", "{\"type\":\"touchEnd\",\"touchPoints\":[]}");
		break;
	case CDP_SCROLL:
		command("Input.dispatchMouseEvent",
			"{\"type\":\"mouseWheel\",\"x\":640,\"y\":360,\"deltaX\":%d,\"deltaY\":%d}",
			in->x, in->y);
		break;
	case CDP_BACK:
		command("Runtime.evaluate", "{\"expression\":\"history.back()\"}");
		break;
	case CDP_RELOAD:
		command("Page.reload", NULL);
		break;
	case CDP_NAVIGATE:
		json_escape(text, sizeof(text), in->text);
		command("Page.navigate", "{\"url\":\"%s\"}", text);
		break;
	case CDP_TEXT:
		json_escape(text, sizeof(text), in->text);
		command("Input.insertText", "{\"text\":\"%s\"}", text);
		break;
	case CDP_ENTER:
		command("Input.dispatchKeyEvent",
			"{\"type\":\"keyDown\",\"key\":\"Enter\",\"code\":\"Enter\","
			"\"windowsVirtualKeyCode\":13,\"text\":\"\\r\"}");
		command("Input.dispatchKeyEvent",
			"{\"type\":\"keyUp\",\"key\":\"Enter\",\"code\":\"Enter\",\"windowsVirtualKeyCode\":13}");
		break;
	case CDP_BACKSPACE:
		command("Input.dispatchKeyEvent",
			"{\"type\":\"keyDown\",\"key\":\"Backspace\",\"code\":\"Backspace\","
			"\"windowsVirtualKeyCode\":8}");
		command("Input.dispatchKeyEvent",
			"{\"type\":\"keyUp\",\"key\":\"Backspace\",\"code\":\"Backspace\","
			"\"windowsVirtualKeyCode\":8}");
		break;
	case CDP_QUIT:
		session[0] = 0;   /* a command of the browser, not of the page */
		command("Browser.close", NULL);
		break;
	}
}

static void cdp_thread(void *arg)
{
	char *message = malloc(MESSAGE_MAX);
	size_t len = 0;
	int waited = 0;

	if (!message)
		return;
	for (;;) {
		struct lkl_pollfd pfd = { .fd = HOST_READ_FD, .events = LKL_POLLIN };
		struct cdp_input in;

		/* Short waits: what the user does is looked at in between. */
		if (lkl_sys_poll(&pfd, 1, 10) > 0) {
			long n = lkl_sys_read(HOST_READ_FD, message + len, MESSAGE_MAX - 1 - len);

			if (n <= 0)
				break;   /* Chromium is gone */
			if (!len)
				stats.first_byte = host_now_ns();
			len += n;
			for (;;) {
				char *end = memchr(message, 0, len);
				size_t used;

				if (!end)
					break;
				used = end - message + 1;
				handle_message(message, used - 1);
				memmove(message, message + used, len - used);
				len -= used;
				if (len)
					stats.first_byte = host_now_ns();
			}
			if (len == MESSAGE_MAX - 1)
				len = 0;   /* not a message we can do anything with */
		}
		if (window) {
			/* The page that is on the screen is the one that acts:
			 * the others are told the same and know they are not it. */
			static const char seen[] = "document.visibilityState=='visible'";
			char params[1024];

			/* Pages are handed to us as they appear, once the browser
			 * has heard that we want them. */
			if (!page_count && waited++ % 50 == 0)
				send_command("", "Target.setAutoAttach", "{\"autoAttach\":true,"
					     "\"waitForDebuggerOnStart\":false,\"flatten\":true}");
			switch (__atomic_exchange_n(&page_action, CDP_NONE, __ATOMIC_SEQ_CST)) {
			case CDP_BACK:
				snprintf(params, sizeof(params), "{\"expression\":\"if(%s)history.back()\"}", seen);
				page_command("Runtime.evaluate", params);
				break;
			case CDP_RELOAD:
				snprintf(params, sizeof(params), "{\"expression\":\"if(%s)location.reload()\"}", seen);
				page_command("Runtime.evaluate", params);
				break;
			case CDP_NAVIGATE:
				snprintf(params, sizeof(params), "{\"expression\":\"if(%s)window.focus()\"}", seen);
				page_command("Runtime.evaluate", params);
				page_command("Page.bringToFront", "{}");
				break;
			}
			if (__atomic_exchange_n(&chosen_ready, 0, __ATOMIC_SEQ_CST) && chooser_session[0]) {
				char file[1100], text[1300];

				json_escape(file, sizeof(file), chosen);
				snprintf(text, sizeof(text), "{\"files\":[\"%s\"],\"backendNodeId\":%d}", file,
					 chooser_node);
				send_command(chooser_session, "DOM.setFileInputFiles", text);
			}
			switch (__atomic_exchange_n(&repaint, -1, __ATOMIC_SEQ_CST)) {
			case 1:
				/* A speck that never stops changing: the page is
				 * drawn and shown at every frame, and the overlay
				 * with it (glwin.c). Black at 1%, two pixels. */
				snprintf(params, sizeof(params), "{\"expression\":\"if(%s&&!document.getElementById('__gxr'))"
					 "{var d=document.createElement('div');d.id='__gxr';d.style.cssText="
					 "'position:fixed;left:0;top:0;width:2px;height:2px;background:#000;"
					 "pointer-events:none;z-index:2147483647;opacity:.01';"
					 "document.documentElement.appendChild(d);d.animate([{opacity:.01},"
					 "{opacity:.02}],{duration:300,iterations:Infinity,direction:'alternate'})}\"}",
					 seen);
				page_command("Runtime.evaluate", params);
				break;
			case 0:
				page_command("Runtime.evaluate", "{\"expression\":\"var d=document."
					     "getElementById('__gxr');if(d)d.remove()\"}");
				break;
			}
			if (__atomic_exchange_n(&ask, 0, __ATOMIC_SEQ_CST)) {
				/* A page that does not have the keys says nothing we
				 * act on: they are the window's then (its address),
				 * and no answer in half a second is Enter. */
				snprintf(params, sizeof(params), "{\"expression\":\"(%s&&document.hasFocus())?"
					 "(window.__gxText&&__gxText()?'gx-text':'gx-other'):'gx-away'\","
					 "\"returnByValue\":true}", seen);
				page_command("Runtime.evaluate", params);
				ask_deadline = host_now_ns() + 500000000ULL;
			}
			if (ask_deadline && host_now_ns() > ask_deadline) {
				ask_deadline = 0;
				__atomic_store_n(&wanted, CDP_ENTER, __ATOMIC_SEQ_CST);
			}
			continue;
		}
		if (!session[0]) {
			/* Until the page exists, keep asking where it is. */
			if (waited++ % 50 == 0)
				command("Target.getTargets", NULL);
			continue;
		}
		while (host_input(&in)) {
			handle_input(&in);
			stats.inputs++;
		}
		stats_tick(host_now_ns());
	}
	free(message);
}

int cdp_activate(void)
{
	if (!window || !page_count)
		return 0;
	__atomic_store_n(&ask, 1, __ATOMIC_SEQ_CST);
	return 1;
}

int cdp_page(int what)
{
	if (!window || !page_count)
		return 0;
	__atomic_store_n(&page_action, what, __ATOMIC_SEQ_CST);
	return 1;
}

void cdp_start_url(const char *url)
{
	snprintf(start_url, sizeof(start_url), "%s", url);
}

int cdp_file_wanted(void)
{
	return __atomic_exchange_n(&chooser_wanted, 0, __ATOMIC_SEQ_CST);
}

void cdp_file_chosen(const char *guest_path)
{
	snprintf(chosen, sizeof(chosen), "%s", guest_path);
	__atomic_store_n(&chosen_ready, 1, __ATOMIC_SEQ_CST);
}

void cdp_repaint(int on)
{
	__atomic_store_n(&repaint, !!on, __ATOMIC_SEQ_CST);
}

int cdp_wanted(void)
{
	return __atomic_exchange_n(&wanted, CDP_NONE, __ATOMIC_SEQ_CST);
}

int cdp_start(int with_window)
{
	int to_guest[2], from_guest[2];
	long args[6] = { 0 };

	window = with_window;
	if (lkl_sys_pipe(to_guest) || lkl_sys_pipe(from_guest))
		return -1;
	/* Chromium reads commands on 3 and writes on 4; our ends go far away
	 * and the numbers the pipes were born with are given back. */
	static const int moves[4][2] = { { 0, 3 }, { 3, 4 }, { 1, HOST_WRITE_FD }, { 2, HOST_READ_FD } };
	int fds[4] = { to_guest[0], to_guest[1], from_guest[0], from_guest[1] };
	int moved[4];

	/* First out of the way, so that no move lands on a descriptor that is
	 * still to be moved. */
	for (int i = 0; i < 4; i++) {
		args[0] = fds[i];
		args[1] = 260 + i;
		args[2] = 0;
		if (lkl_syscall(__lkl__NR_dup3, args) < 0)
			return -1;
		lkl_sys_close(fds[i]);
		moved[i] = 260 + i;
	}
	for (int i = 0; i < 4; i++) {
		args[0] = moved[moves[i][0]];
		args[1] = moves[i][1];
		args[2] = 0;
		if (lkl_syscall(__lkl__NR_dup3, args) < 0)
			return -1;
	}
	for (int i = 0; i < 4; i++)
		lkl_sys_close(moved[i]);
	return host_thread_start(cdp_thread, NULL);
}
