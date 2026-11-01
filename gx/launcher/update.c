/*
 * update - new versions of Chromium NX from its releases on GitHub.
 *
 * Each release (scripts/release.py) has, besides its files, manifest.txt:
 *
 *	version=0.3
 *	tag=v0.3
 *	file=chromium-nx.nro <size> <sha-256>
 *	file=chromium-full.ext4 <size> <sha-256>
 *
 * When the launcher starts it reads the newest release's (GitHub sends
 * releases/latest/download/<name> to it) and, if that is not this version,
 * this version's own, to know what image goes with this program. The image
 * on the card is known by its SHA-256, worked out once and kept next to it
 * (chromium-full.ext4.sha256, with its size and date), so that 411 MB is not
 * read at every start. A file is downloaded only when the release's differs
 * from the card's: an update of the program alone does not bring the image
 * again.
 *
 * A download goes into <name>.part, which a later try goes on from, and
 * takes the file's place only once all of the release's files are there and
 * each has the SHA-256 the release says: an update cut short leaves the
 * program and the image as they were. The program's previous version is kept
 * as chromium-nx.nro.old. The profile, the settings and the downloads are
 * never touched.
 *
 * curl does HTTPS with the console's own TLS (libnx), and runs on a thread
 * of its own: the launcher is never held up by the network.
 */
#include <switch.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "update.h"
#include "../version.h"

#define APP_DIR "sdmc:/switch/chromium-nx"
/* Where the releases are; a test build is given a server of its own
 * (build-nx.sh, UPDATE_RELEASES; scripts/release.py --local). */
#ifndef UPDATE_RELEASES
#define UPDATE_RELEASES "https://github.com/" UPDATE_REPO "/releases/"
#endif
#define RELEASES UPDATE_RELEASES
#define MAX_FILES 8

struct entry {
	char name[64];
	uint64_t size;
	char sha[65];
};

struct manifest {
	int ok;
	char version[16], tag[32];
	int count;
	struct entry files[MAX_FILES];
};

static Mutex lock;
static struct update_status status;
static struct manifest latest, current;
static Thread worker;
static int worker_started, cancel_wanted, net_ready;
static uint64_t done_before;   /* bytes of the files already downloaded, this time */

static void set_state(int state)
{
	mutexLock(&lock);
	status.state = state;
	mutexUnlock(&lock);
}

static void fail(const char *why)
{
	mutexLock(&lock);
	snprintf(status.error, sizeof(status.error), "%s", why);
	status.state = UPDATE_FAILED;
	mutexUnlock(&lock);
}

void update_status(struct update_status *out)
{
	mutexLock(&lock);
	*out = status;
	mutexUnlock(&lock);
}

void update_cancel(void)
{
	__atomic_store_n(&cancel_wanted, 1, __ATOMIC_SEQ_CST);
}

static int net(void)
{
	if (!net_ready) {
		if (R_FAILED(socketInitializeDefault()))
			return 0;
		curl_global_init(CURL_GLOBAL_DEFAULT);
		net_ready = 1;
	}
	return 1;
}

static CURL *easy(const char *url)
{
	CURL *c = curl_easy_init();

	if (!c)
		return NULL;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   /* GitHub sends files elsewhere */
	curl_easy_setopt(c, CURLOPT_USERAGENT, "chromium-nx/" GX_VERSION);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
	return c;
}

/* ---------- manifests ---------- */

struct text {
	char *data;
	size_t len, room;
};

static size_t to_text(void *p, size_t size, size_t n, void *arg)
{
	struct text *t = arg;

	if (t->len + size * n + 1 > t->room)
		return 0;
	memcpy(t->data + t->len, p, size * n);
	t->len += size * n;
	t->data[t->len] = 0;
	return size * n;
}

/* curl's own time limits were seen not to end a connection to a host that
 * does not answer: they are kept here, by the console's clock, in curl's
 * progress callback (which is called every second or so whatever happens). */
static u64 deadline;   /* system ticks; 0: none */

static int stop_wanted(void *arg, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	(void)arg, (void)a, (void)b, (void)c, (void)d;
	return __atomic_load_n(&cancel_wanted, __ATOMIC_SEQ_CST) || (deadline && armGetSystemTick() > deadline);
}

static int fetch_manifest(const char *url, struct manifest *m)
{
	char data[4096];
	struct text t = { data, 0, sizeof(data) };
	CURL *c = easy(url);
	CURLcode rc;

	memset(m, 0, sizeof(*m));
	data[0] = 0;
	if (!c)
		return 0;
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, stop_wanted);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_text);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
	deadline = armGetSystemTick() + armNsToTicks(10000000000ULL);
	rc = curl_easy_perform(c);
	deadline = 0;
	if (rc != CURLE_OK) {
		long code = 0;

		/* Why, for "Check now" to say. */
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		mutexLock(&lock);
		if (rc == CURLE_HTTP_RETURNED_ERROR)
			snprintf(status.error, sizeof(status.error), "HTTP %ld", code);
		else if (rc == CURLE_ABORTED_BY_CALLBACK)
			snprintf(status.error, sizeof(status.error), "no answer");
		else
			snprintf(status.error, sizeof(status.error), "%s", curl_easy_strerror(rc));
		mutexUnlock(&lock);
	}
	curl_easy_cleanup(c);
	if (rc != CURLE_OK)
		return 0;
	for (char *line = strtok(data, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		if (!strncmp(line, "version=", 8)) {
			snprintf(m->version, sizeof(m->version), "%s", line + 8);
		} else if (!strncmp(line, "tag=", 4)) {
			snprintf(m->tag, sizeof(m->tag), "%s", line + 4);
		} else if (!strncmp(line, "file=", 5) && m->count < MAX_FILES) {
			struct entry *e = &m->files[m->count];
			unsigned long long size;

			if (sscanf(line + 5, "%63s %llu %64s", e->name, &size, e->sha) == 3 &&
			    !strchr(e->name, '/') && strlen(e->sha) == 64) {
				e->size = size;
				m->count++;
			}
		}
	}
	m->ok = m->version[0] && m->tag[0];
	return m->ok;
}

static const struct entry *find(const struct manifest *m, const char *name)
{
	for (int i = 0; m->ok && i < m->count; i++)
		if (!strcmp(m->files[i].name, name))
			return &m->files[i];
	return NULL;
}

/* -1, 0 or 1, as a is older, the same or newer than b ("0.10" > "0.9"). */
static int version_compare(const char *a, const char *b)
{
	for (;;) {
		char *end_a, *end_b;
		long x = strtol(a, &end_a, 10), y = strtol(b, &end_b, 10);

		if (x != y)
			return x < y ? -1 : 1;
		if (*end_a != '.' && *end_b != '.')
			return 0;
		a = *end_a == '.' ? end_a + 1 : end_a;
		b = *end_b == '.' ? end_b + 1 : end_b;
	}
}

/* ---------- SHA-256 of files ---------- */

static int hash_file(const char *path, char out[65])
{
	static const char hex[] = "0123456789abcdef";
	FILE *f = fopen(path, "rb");
	Sha256Context ctx;
	u8 hash[SHA256_HASH_SIZE];
	unsigned char *buf = malloc(1 << 20);
	size_t n;

	if (!f || !buf) {
		if (f)
			fclose(f);
		free(buf);
		return 0;
	}
	sha256ContextCreate(&ctx);
	while ((n = fread(buf, 1, 1 << 20, f)) > 0 && !__atomic_load_n(&cancel_wanted, __ATOMIC_SEQ_CST))
		sha256ContextUpdate(&ctx, buf, n);
	fclose(f);
	free(buf);
	if (__atomic_load_n(&cancel_wanted, __ATOMIC_SEQ_CST))
		return 0;   /* the launcher is ending */
	sha256ContextGetHash(&ctx, hash);
	for (int i = 0; i < SHA256_HASH_SIZE; i++) {
		out[i * 2] = hex[hash[i] >> 4];
		out[i * 2 + 1] = hex[hash[i] & 15];
	}
	out[64] = 0;
	return 1;
}

static void remember_hash(const char *path, const char *sha)
{
	char cache[256];
	struct stat st;
	FILE *f;

	snprintf(cache, sizeof(cache), "%s.sha256", path);
	if (stat(path, &st) || !(f = fopen(cache, "w")))
		return;
	fprintf(f, "%llu %lld %s\n", (unsigned long long)st.st_size, (long long)st.st_mtime, sha);
	fclose(f);
}

/* The SHA-256 of a file of the card, from what was kept of it if the file
 * has not changed since. 0 if there is no such file. */
static int file_hash(const char *path, char out[65])
{
	char cache[256], sha[65];
	unsigned long long size;
	long long mtime;
	struct stat st;
	FILE *f;

	if (stat(path, &st))
		return 0;
	snprintf(cache, sizeof(cache), "%s.sha256", path);
	if ((f = fopen(cache, "r"))) {
		int known = fscanf(f, "%llu %lld %64s", &size, &mtime, sha) == 3 &&
			    size == (unsigned long long)st.st_size && mtime == (long long)st.st_mtime;

		fclose(f);
		if (known) {
			memcpy(out, sha, 65);
			return 1;
		}
	}
	if (!hash_file(path, out))
		return 0;
	remember_hash(path, out);
	return 1;
}

/* ---------- checking ---------- */

static void start(void (*fn)(void *))
{
	if (worker_started) {
		threadWaitForExit(&worker);
		threadClose(&worker);
		worker_started = 0;
	}
	if (R_SUCCEEDED(threadCreate(&worker, fn, NULL, NULL, 0x40000, 0x2C, -2)) &&
	    R_SUCCEEDED(threadStart(&worker)))
		worker_started = 1;
	else
		fail("Cannot start");
}

static void check(void *unused)
{
	char sha[65];
	const struct entry *image, *program;
	int have;

	(void)unused;
	if (net() && fetch_manifest(RELEASES "latest/download/manifest.txt", &latest) &&
	    !strcmp(latest.version, GX_VERSION))
		current = latest;
	else if (net_ready)
		fetch_manifest(RELEASES "download/v" GX_VERSION "/manifest.txt", &current);

	/* The image is read through only when a release has one to compare
	 * it with. */
	have = latest.ok || current.ok ? file_hash(APP_DIR "/" UPDATE_IMAGE, sha) :
					 !access(APP_DIR "/" UPDATE_IMAGE, F_OK);
	mutexLock(&lock);
	status.newer = latest.ok && version_compare(latest.version, GX_VERSION) > 0;
	snprintf(status.latest, sizeof(status.latest), "%s", latest.ok ? latest.version : "");
	image = find(&current, UPDATE_IMAGE);
	status.image = !have ? IMAGE_MISSING : !image ? IMAGE_UNKNOWN : strcmp(sha, image->sha) ? IMAGE_OTHER : IMAGE_OK;
	status.image_bytes = image ? image->size : 0;
	status.update_bytes = 0;
	if (status.newer) {
		program = find(&latest, UPDATE_PROGRAM);
		image = find(&latest, UPDATE_IMAGE);
		status.update_bytes = (program ? program->size : 0) +
				      (image && (!have || strcmp(sha, image->sha)) ? image->size : 0);
	}
	status.state = UPDATE_CHECKED;
	mutexUnlock(&lock);
}

void update_check(void)
{
	__atomic_store_n(&cancel_wanted, 0, __ATOMIC_SEQ_CST);
	mutexLock(&lock);
	memset(&status, 0, sizeof(status));
	status.state = UPDATE_CHECKING;
	mutexUnlock(&lock);
	start(check);
}

/* ---------- downloading ---------- */

static int stalled;   /* the last download stopped for want of data */
static curl_off_t last;
static u64 last_change;   /* when the download last moved; 0: not yet */

static int progress(void *arg, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up_now)
{
	uint64_t resumed = *(uint64_t *)arg;
	u64 tick = armGetSystemTick();

	(void)total;
	(void)up_total;
	(void)up_now;
	mutexLock(&lock);
	status.done = done_before + resumed + (uint64_t)now;
	mutexUnlock(&lock);
	/* Nothing received for 30 s: given up (it goes on from there next
	 * time). */
	if (now != last || !last_change) {
		last = now;
		last_change = tick;
	} else if (armTicksToNs(tick - last_change) > 30000000000ULL) {
		last_change = 0;
		stalled = 1;
		return 1;
	}
	return __atomic_load_n(&cancel_wanted, __ATOMIC_SEQ_CST);
}

/* Where a file of a release is downloaded to: <name>.<start of its SHA-256>.part,
 * so that what is left of a download is only ever continued with the same
 * file, not another version's. */
static void part_path(const struct entry *e, char *out, size_t room)
{
	snprintf(out, room, APP_DIR "/%s.%.12s.part", e->name, e->sha);
}

/* What other versions left of this file, half downloaded, goes. */
static void forget_other_parts(const struct entry *e)
{
	char mine[256], path[300];
	size_t len = strlen(e->name);
	struct dirent *d;
	DIR *dir = opendir(APP_DIR);

	part_path(e, mine, sizeof(mine));
	while (dir && (d = readdir(dir))) {
		size_t n = strlen(d->d_name);

		if (!strncmp(d->d_name, e->name, len) && d->d_name[len] == '.' && n > 5 &&
		    !strcmp(d->d_name + n - 5, ".part")) {
			snprintf(path, sizeof(path), APP_DIR "/%s", d->d_name);
			if (strcmp(path, mine))
				remove(path);
		}
	}
	if (dir)
		closedir(dir);
}

/* One file of a release into its .part, checked. */
static int download(const struct manifest *m, const struct entry *e)
{
	char url[256], part[256], sha[65];
	struct stat st;
	uint64_t resumed = 0;
	CURLcode rc = CURLE_OK;
	FILE *f;
	CURL *c;

	snprintf(url, sizeof(url), RELEASES "download/%s/%s", m->tag, e->name);
	part_path(e, part, sizeof(part));
	forget_other_parts(e);
	mutexLock(&lock);
	snprintf(status.file, sizeof(status.file), "%s", e->name);
	mutexUnlock(&lock);
	if (!stat(part, &st))
		resumed = (uint64_t)st.st_size;
	if (resumed > e->size) {
		remove(part);
		resumed = 0;
	}
	if (resumed < e->size) {
		static char buffer[1 << 20];

		if (!(f = fopen(part, "ab")) || !(c = easy(url))) {
			if (f)
				fclose(f);
			fail("Cannot write to the SD card");
			return 0;
		}
		setvbuf(f, buffer, _IOFBF, sizeof(buffer));
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fwrite);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
		curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resumed);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, &resumed);
		/* A connection that stalls for 30 s is given up. */
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
		stalled = 0;
		last = 0;
		last_change = 0;
		rc = curl_easy_perform(c);
		curl_easy_cleanup(c);
		if (fclose(f) && rc == CURLE_OK)
			rc = CURLE_WRITE_ERROR;
	}
	if (rc != CURLE_OK) {
		fail(rc == CURLE_ABORTED_BY_CALLBACK && stalled ? "The connection stopped: try again, it will go on from there" :
		     rc == CURLE_ABORTED_BY_CALLBACK ? "Download stopped: it will go on from there next time" :
		     rc == CURLE_WRITE_ERROR ? "Cannot write to the SD card (is it full?)" :
		     "Download failed: check the connection and try again");
		return 0;
	}
	if (!hash_file(part, sha) || strcmp(sha, e->sha)) {
		remove(part);
		fail("The downloaded file is damaged: try again");
		return 0;
	}
	done_before += e->size;
	return 1;
}

/* The checked <name>.part takes the file's place. */
static int install(const struct entry *e)
{
	char final[256], part[256], old[256];

	snprintf(final, sizeof(final), APP_DIR "/%s", e->name);
	part_path(e, part, sizeof(part));
	snprintf(old, sizeof(old), APP_DIR "/%s.old", e->name);
	remove(old);
	if (!strcmp(e->name, UPDATE_PROGRAM))
		rename(final, old);   /* the previous program is kept */
	else
		remove(final);
	if (rename(part, final)) {
		rename(old, final);
		return 0;
	}
	remember_hash(final, e->sha);
	return 1;
}

static void install_files(const struct manifest *m, const struct entry **files, int count)
{
	int program = 0;

	__atomic_store_n(&cancel_wanted, 0, __ATOMIC_SEQ_CST);
	done_before = 0;
	mutexLock(&lock);
	status.state = UPDATE_DOWNLOADING;
	status.done = 0;
	status.total = 0;
	for (int i = 0; i < count; i++)
		status.total += files[i]->size;
	mutexUnlock(&lock);
	/* The console does not go to sleep in the middle of it. */
	appletSetAutoSleepDisabled(true);
	for (int i = 0; i < count; i++)
		if (!download(m, files[i])) {
			appletSetAutoSleepDisabled(false);
			return;
		}
	for (int i = 0; i < count; i++) {
		if (!install(files[i])) {
			appletSetAutoSleepDisabled(false);
			fail("Cannot replace the files on the SD card");
			return;
		}
		program |= !strcmp(files[i]->name, UPDATE_PROGRAM);
	}
	appletSetAutoSleepDisabled(false);
	set_state(program ? UPDATE_INSTALLED : UPDATE_IMAGE_DONE);
}

static void install_latest(void *unused)
{
	const struct entry *files[MAX_FILES];
	char sha[65];
	int count = 0, have = file_hash(APP_DIR "/" UPDATE_IMAGE, sha);

	(void)unused;
	for (int i = 0; i < latest.count; i++) {
		const struct entry *e = &latest.files[i];

		/* What the card has already is not downloaded again. */
		if (!strcmp(e->name, UPDATE_IMAGE) && have && !strcmp(sha, e->sha))
			continue;
		if (strcmp(e->name, UPDATE_PROGRAM) && strcmp(e->name, UPDATE_IMAGE))
			continue;   /* not a file this version knows what to do with */
		files[count++] = e;
	}
	install_files(&latest, files, count);
}

static void install_image(void *unused)
{
	const struct entry *image = find(&current, UPDATE_IMAGE);

	(void)unused;
	if (!image) {
		fail("No release of this version has an image");
		return;
	}
	install_files(&current, &image, 1);
}

void update_install_latest(void)
{
	if (!latest.ok) {
		fail("No release found");
		return;
	}
	start(install_latest);
}

void update_install_image(void)
{
	start(install_image);
}

void update_exit(void)
{
	/* What is under way stops (a download goes on from there next time),
	 * and what the network borrowed of the heap is given back: the
	 * Homebrew loader needs the heap whole to load the next program. */
	update_cancel();
	if (worker_started) {
		threadWaitForExit(&worker);
		threadClose(&worker);
		worker_started = 0;
	}
	if (net_ready) {
		curl_global_cleanup();
		socketExit();
		net_ready = 0;
	}
}
