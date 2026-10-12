/*
 * strain - memory under the load a large program puts on it, checked.
 *
 * Several threads take memory that is only there when first used, fill it
 * (by hand, by memcpy(), from a file, through a pipe), give parts of it back
 * and take it again, while one of them runs other programs: what a Steam
 * client does when its pages come out wrong. Every stage is on its own first
 * and then with the others, and each says what it read that it had not
 * written: where, and whether a whole page of it was zero.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096UL
#define FILE_SIZE (24UL << 20)

static const char *file_path;   /* NULL: nowhere to keep a file, the stage is left out */
static volatile int stop;
static int errors[12];
enum { STAGES = 9, CHILDREN = 9 };
static const char *stage_names[] = { "by hand", "memcpy", "file", "pipe", "give back", "malloc", "shared", "reserve",
				     "relay", "children" };

static uint64_t word_at(uint64_t seed, uint64_t i)
{
	uint64_t x = seed * 0x9E3779B97F4A7C15UL + i * 0xBF58476D1CE4E5B9UL;

	return x ^ (x >> 29) ^ 0x0101010101010101UL;   /* never zero for long */
}

static void *take(size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return p == MAP_FAILED ? NULL : p;
}

static void wrong(int stage, const uint64_t *base, size_t i, uint64_t want, int thread)
{
	const uint64_t *page = (const uint64_t *)((uintptr_t)&base[i] & ~(PAGE - 1));
	int zero = 1;

	for (size_t k = 0; k < PAGE / 8; k++)
		zero &= page[k] == 0;
	if (__atomic_fetch_add(&errors[stage], 1, __ATOMIC_SEQ_CST) < 6)
		printf("FAIL: %s, thread %d: at %p (+%#zx) %#lx for %#lx%s\n", stage_names[stage], thread,
		       (void *)&base[i], i * 8, (unsigned long)base[i], (unsigned long)want,
		       zero ? ", the whole page zero" : "");
}

static void check(int stage, const uint64_t *p, size_t words, uint64_t seed, uint64_t from, int thread)
{
	for (size_t i = 0; i < words; i++)
		if (p[i] != word_at(seed, from + i)) {
			wrong(stage, p, i, word_at(seed, from + i), thread);
			i |= PAGE / 8 - 1;   /* one line for a page */
		}
}

static void fill(uint64_t *p, size_t words, uint64_t seed, uint64_t from)
{
	for (size_t i = 0; i < words; i++)
		p[i] = word_at(seed, from + i);
}

/* One turn of each stage; `only` is a stage, or -1 for them all. */
static void turn(int thread, unsigned round, int only)
{
	uint64_t seed = thread * 1000003UL + round;
	size_t len = ((round * 7 + thread * 13) % 61 + 3) * 16 * PAGE + (round % 3) * PAGE;
	size_t words = len / 8;

	if (only < 0 || only == 0) {
		uint64_t *a = take(len);

		if (a) {
			fill(a, words, seed, 0);
			check(0, a, words, seed, 0, thread);
			munmap(a, len);
		}
	}
	if (only < 0 || only == 1) {
		uint64_t *a = take(len), *b = take(len);

		if (a && b) {
			fill(a, words, seed, 0);
			memcpy(b, a, len);   /* to memory that is not there yet */
			check(1, b, words, seed, 0, thread);
			memmove((uint8_t *)b + PAGE, b, len - PAGE);
			check(1, b + PAGE / 8, words - PAGE / 8, seed, 0, thread);
		}
		if (a)
			munmap(a, len);
		if (b)
			munmap(b, len);
	}
	if (file_path && (only < 0 || only == 2)) {
		int fd = open(file_path, O_RDONLY);
		uint64_t *a = take(len);
		off_t off = ((seed * 4099) % ((FILE_SIZE - len) / PAGE)) * PAGE + (round % 2) * 8 * (seed % 300);
		size_t got = 0;

		if (fd >= 0 && a) {
			/* In pieces of odd sizes, like a program that parses. */
			while (got < len) {
				size_t want = (seed + got) % 3 ? 65536 : 1000003;
				ssize_t n = pread(fd, (uint8_t *)a + got, want < len - got ? want : len - got, off + got);

				if (n <= 0)
					break;
				got += n;
			}
			if (got == len)
				check(2, a, words, 7, off / 8, thread);
			else
				wrong(2, a, 0, got, thread);
		}
		if (fd >= 0)
			close(fd);
		if (a)
			munmap(a, len);
	}
	if (only < 0 || only == 3) {
		int ends[2];
		uint64_t *a = take(len), *b = take(len);

		if (a && b && !pipe(ends)) {
			size_t sent = 0, got = 0;

			fill(a, words, seed, 0);
			while (got < len) {
				if (sent < len) {
					size_t n = len - sent < 32768 ? len - sent : 32768;

					sent += write(ends[1], (uint8_t *)a + sent, n);
				}
				got += read(ends[0], (uint8_t *)b + got, len - got);
			}
			check(3, b, words, seed, 0, thread);
			close(ends[0]);
			close(ends[1]);
		}
		if (a)
			munmap(a, len);
		if (b)
			munmap(b, len);
	}
	if (only < 0 || only == 4) {
		/* Parts given back and taken again: what was given back reads
		 * as zero, what was not is as it was. */
		uint64_t *a = take(len);
		size_t pages = len / PAGE, cut = pages / 3, n = pages / 4 ? pages / 4 : 1;

		if (a) {
			fill(a, words, seed, 0);
			madvise((uint8_t *)a + cut * PAGE, n * PAGE, MADV_DONTNEED);
			check(4, a, cut * PAGE / 8, seed, 0, thread);
			for (size_t i = cut * PAGE / 8; i < (cut + n) * PAGE / 8; i++)
				if (a[i]) {
					wrong(4, a, i, 0, thread);
					break;
				}
			check(4, a + (cut + n) * PAGE / 8, words - (cut + n) * PAGE / 8, seed, (cut + n) * PAGE / 8,
			      thread);
			mprotect((uint8_t *)a + cut * PAGE, n * PAGE, PROT_NONE);
			mprotect((uint8_t *)a + cut * PAGE, n * PAGE, PROT_READ | PROT_WRITE);
			fill(a + cut * PAGE / 8, n * PAGE / 8, seed, cut * PAGE / 8);
			munmap(a, cut * PAGE);   /* the bottom goes, the rest stays */
			check(4, a + cut * PAGE / 8, words - cut * PAGE / 8, seed, cut * PAGE / 8, thread);
			munmap((uint8_t *)a + cut * PAGE, len - cut * PAGE);
		}
	}
	if (only < 0 || only == 5) {
		/* The C library's own: small pieces, grown. */
		size_t n = 1000 + seed % 5000;
		uint64_t *a = malloc(n * 8);

		if (a) {
			fill(a, n, seed, 0);
			for (int grow = 0; grow < 6 && a; grow++) {
				uint64_t *b = realloc(a, n * 2 * 8);

				if (!b)
					break;
				a = b;
				fill(a + n, n, seed, n);
				n *= 2;
			}
			check(5, a, n, seed, 0, thread);
			free(a);
		}
	}
	if (only < 0 || only == 6) {
		/* Memory with no file's name, mapped twice: what goes in at
		 * one address is read at the other, a piece at a time and
		 * round and round, the way a browser hands a script from the
		 * thread that read it to the one that runs it. */
		enum { RING = 16 * PAGE };
		int fd = syscall(SYS_memfd_create, "strain", 0);
		uint8_t *in = NULL, *out = NULL;
		uint64_t *a = take(len), *b = take(len);

		if (fd >= 0 && !ftruncate(fd, RING)) {
			in = mmap(NULL, RING, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			out = mmap(NULL, RING, PROT_READ, MAP_SHARED, fd, 0);
		}
		if (in == MAP_FAILED || out == MAP_FAILED || in == out) {
			if (__atomic_fetch_add(&errors[6], 1, __ATOMIC_SEQ_CST) < 6)
				printf("FAIL: shared, thread %d: the two mappings are %p and %p\n", thread, (void *)in,
				       (void *)out);
		} else if (in && out && a && b) {
			size_t done = 0, at = 0;

			fill(a, words, seed, 0);
			while (done < len) {
				size_t n = (seed + done) % 5 ? 65536 - 8 * (seed % 97) : 4096 * 3 + 8, first;

				if (n > len - done)
					n = len - done;
				if (n > RING)
					n = RING;
				first = n < RING - at ? n : RING - at;
				memcpy(in + at, (uint8_t *)a + done, first);
				memcpy(in, (uint8_t *)a + done + first, n - first);
				memcpy((uint8_t *)b + done, out + at, first);
				memcpy((uint8_t *)b + done + first, out, n - first);
				at = (at + n) % RING;
				done += n;
			}
			check(6, b, words, seed, 0, thread);
		}
		if (in && in != MAP_FAILED)
			munmap(in, RING);
		if (out && out != MAP_FAILED)
			munmap(out, RING);
		if (fd >= 0)
			close(fd);
		if (a)
			munmap(a, len);
		if (b)
			munmap(b, len);
	}
	if (only < 0 || only == 7) {
		/* Addresses kept, then used a part at a time, the way a
		 * browser's allocator has them: what is made unusable and
		 * usable again is as it was, what is given up is zero when it
		 * is had again. */
		size_t room = 4UL << 20, pages = room / PAGE, part = 16 + seed % 48;
		uint8_t *r = mmap(NULL, room, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

		if (r != MAP_FAILED) {
			for (size_t p = seed % 7; p + part <= pages; p += part + seed % 5) {
				uint64_t *a = (uint64_t *)(r + p * PAGE);
				size_t n = part * PAGE / 8;

				mprotect(a, part * PAGE, PROT_READ | PROT_WRITE);
				fill(a, n, seed, p);
				mprotect(a, part * PAGE, PROT_NONE);
				mprotect(a, part * PAGE, PROT_READ | PROT_WRITE);
				check(7, a, n, seed, p, thread);
				if (p % 3 == 0) {
					madvise(a, part * PAGE, MADV_DONTNEED);
				} else if (p % 3 == 1) {
					mmap(a, part * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
					mprotect(a, part * PAGE, PROT_READ | PROT_WRITE);
				} else {
					mprotect(a, part * PAGE, PROT_NONE);
					madvise(a, part * PAGE, MADV_DONTNEED);
					mprotect(a, part * PAGE, PROT_READ | PROT_WRITE);
				}
				for (size_t i = 0; i < n; i++)
					if (a[i]) {
						wrong(7, a, i, 0, thread);
						break;
					}
				fill(a, n, seed + 1, p);
				check(7, a, n, seed + 1, p, thread);
			}
			munmap(r, room);
		}
	}
}

/* A piece of memory passed from thread to thread, each waiting its turn: a
 * thread that waits gives its host thread to another and comes back on any,
 * and what it had in its registers (numbers that are not whole among them)
 * is to be there still. */
static struct relay {
	pthread_mutex_t lock;
	pthread_cond_t turn;
	int whose, players;
	uint64_t *piece;
	size_t words;
	uint64_t seed;
} relay = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, NULL, 0, 0 };

static void *relay_player(void *arg)
{
	int me = (int)(intptr_t)arg;
	double mine = me * 1.25 + 0.5, sum = 0;
	unsigned rounds = 0;

	pthread_mutex_lock(&relay.lock);
	while (!stop) {
		double before = mine * 3.0 + rounds;

		while (relay.whose != me && !stop)
			pthread_cond_wait(&relay.turn, &relay.lock);
		if (stop)
			break;
		if (before != mine * 3.0 + rounds || mine != me * 1.25 + 0.5)
			if (__atomic_fetch_add(&errors[8], 1, __ATOMIC_SEQ_CST) < 6)
				printf("FAIL: relay, thread %d: a number came back as %g\n", me, before);
		if (relay.piece) {
			check(8, relay.piece, relay.words, relay.seed, 0, me);
			munmap(relay.piece, relay.words * 8);
		}
		relay.seed = me * 7919UL + rounds;
		relay.words = (64 + (relay.seed % 64) * 16) * PAGE / 8;
		relay.piece = take(relay.words * 8);
		if (relay.piece)
			fill(relay.piece, relay.words, relay.seed, 0);
		sum += before;
		rounds++;
		relay.whose = (me + 1) % relay.players;
		pthread_cond_broadcast(&relay.turn);
	}
	pthread_mutex_unlock(&relay.lock);
	return sum < 0 ? arg : NULL;
}

static void *worker(void *arg)
{
	int thread = (int)(intptr_t)arg;

	for (unsigned round = 0; !stop; round++)
		turn(thread, round, -1);
	return NULL;
}

/* Runs other programs, the two ways a Steam client does. */
static void *parent(void *arg)
{
	while (!stop) {
		pid_t pid = fork();
		int status = 0;

		if (!pid) {
			execl("/bin/strain", "strain", "child", (char *)NULL);
			_exit(41);
		}
		if (pid > 0 && (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7))
			if (__atomic_fetch_add(&errors[CHILDREN], 1, __ATOMIC_SEQ_CST) < 6)
				printf("FAIL: a child ended with %#x\n", status);
		pid = fork();
		if (!pid)
			_exit(7);   /* one that ends without a program of its own */
		if (pid > 0)
			waitpid(pid, &status, 0);
		usleep(20000);
	}
	return NULL;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int total(void)
{
	int n = 0;

	for (int i = 0; i <= CHILDREN; i++)
		n += errors[i];
	return n;
}

int main(int argc, char **argv)
{
	enum { THREADS = 8 };
	enum { PLAYERS = 6 };
	pthread_t threads[THREADS], players[PLAYERS], runner;
	double start;
	int before;

	if (argc > 1 && !strcmp(argv[1], "child")) {
		/* A child: a little memory of its own, and out. */
		uint64_t *a = take(64 * PAGE);

		if (a) {
			fill(a, 64 * PAGE / 8, 99, 0);
			check(CHILDREN, a, 64 * PAGE / 8, 99, 0, -1);
		}
		return errors[CHILDREN] ? 8 : 7;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	{
		/* The file the threads read from, where one can be kept. */
		static const char *places[] = { "/tmp/strain.data", "/dev/shm/strain.data", "/root/strain.data" };
		uint64_t *block = take(1 << 20);

		for (int i = 0; i < 3 && !file_path && block; i++) {
			int fd = open(places[i], O_CREAT | O_TRUNC | O_WRONLY, 0600);
			uint64_t at = 0;

			if (fd < 0)
				continue;
			for (; at < FILE_SIZE; at += 1 << 20) {
				fill(block, (1 << 20) / 8, 7, at / 8);
				if (write(fd, block, 1 << 20) != 1 << 20)
					break;
			}
			close(fd);
			if (at == FILE_SIZE)
				file_path = places[i];
			else
				unlink(places[i]);
		}
		if (block)
			munmap(block, 1 << 20);
		if (!file_path)
			printf("no file could be kept: nothing is read from one\n");
	}
	/* Each stage on its own, one thread. */
	for (int stage = 0; stage < 8; stage++) {
		for (unsigned round = 0; round < 40; round++)
			turn(0, round, stage);
		printf("%s, alone: %d wrong\n", stage_names[stage], errors[stage]);
	}
	before = total();
	/* All of them, on several threads. */
	start = now();
	for (int i = 0; i < THREADS; i++)
		pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)(i + 1));
	while (now() - start < 20)
		usleep(100000);
	stop = 1;
	for (int i = 0; i < THREADS; i++)
		pthread_join(threads[i], NULL);
	printf("%d threads: %d wrong\n", THREADS, total() - before);
	for (int stage = 0; stage < 8; stage++)
		printf("  %s: %d\n", stage_names[stage], errors[stage]);
	before = total();
	/* And with programs run meanwhile. */
	stop = 0;
	start = now();
	for (int i = 0; i < THREADS; i++)
		pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)(i + 101));
	pthread_create(&runner, NULL, parent, NULL);
	relay.players = PLAYERS;
	for (int i = 0; i < PLAYERS; i++)
		pthread_create(&players[i], NULL, relay_player, (void *)(intptr_t)i);
	while (now() - start < 30)
		usleep(100000);
	stop = 1;
	pthread_mutex_lock(&relay.lock);
	pthread_cond_broadcast(&relay.turn);
	pthread_mutex_unlock(&relay.lock);
	for (int i = 0; i < THREADS; i++)
		pthread_join(threads[i], NULL);
	for (int i = 0; i < PLAYERS; i++)
		pthread_join(players[i], NULL);
	pthread_join(runner, NULL);
	printf("%d threads, a relay and children: %d wrong\n", THREADS, total() - before);
	for (int stage = 0; stage <= CHILDREN; stage++)
		printf("  %s: %d\n", stage_names[stage], errors[stage]);
	if (file_path)
		unlink(file_path);
	printf(total() ? "FAIL\n" : "PASS\n");
	return total() != 0;
}
