/*
 * shm - two programs and one piece of memory: what a client and its helper
 * talk through. The second program is this one, run again by the first.
 *
 * What gx owes them: each sees at once what the other writes there, whichever
 * mapped it first and wherever each has it; a lock and a condition kept in it
 * work across the two; the file may grow, and be written to and read as a
 * file, and it is all the same bytes. Prints PASS or what failed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME "/dev/shm/gx-test-shm"
#define SIZE (1 << 20)

extern char **environ;

struct shared {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int turn;           /* 0: nobody yet; then the number of the last to write */
	int rounds;
	char text[64];
	uint32_t sums[16];
};

static int failed;

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	failed = 1;
}

static uint32_t sum(const uint8_t *p, size_t n)
{
	uint32_t s = 0;

	while (n--)
		s = s * 31 + *p++;
	return s;
}

/* The helper: answers the client a hundred times through the memory, then
 * makes the file larger and leaves something in the new part. */
static int helper(void)
{
	int fd = open(NAME, O_RDWR);
	struct shared *m = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	uint8_t *more;
	char word[16];

	if (fd < 0 || m == MAP_FAILED)
		return 2;
	if (strcmp(m->text, "from the client"))
		return 3;
	pthread_mutex_lock(&m->lock);
	for (int i = 0; i < 100; i++) {
		while (m->turn != 1)
			pthread_cond_wait(&m->cond, &m->lock);
		m->rounds++;
		m->turn = 2;
		pthread_cond_broadcast(&m->cond);
	}
	strcpy(m->text, "from the helper");
	pthread_mutex_unlock(&m->lock);

	/* What was written to the file as a file is in the memory. */
	if (pread(fd, word, 9, 4096 * 3) != 9 || memcmp(word, "as a file", 9))
		return 4;
	if (memcmp((char *)m + 4096 * 3, "as a file", 9))
		return 5;
	/* And the other way round. */
	memcpy((char *)m + 4096 * 5, "as memory", 9);
	if (pread(fd, word, 9, 4096 * 5) != 9 || memcmp(word, "as memory", 9))
		return 6;

	/* Twice the size, the new half filled. */
	if (ftruncate(fd, 2 * SIZE))
		return 7;
	more = mmap(NULL, 2 * SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (more == MAP_FAILED)
		return 8;
	if (strcmp(((struct shared *)more)->text, "from the helper"))
		return 9;
	for (int i = 0; i < SIZE; i++)
		more[SIZE + i] = (uint8_t)(i * 7 + 3);
	m->sums[0] = sum(more + SIZE, SIZE);
	return 0;
}

int main(int argc, char **argv)
{
	char *again[] = { argv[0], "helper", NULL };
	pthread_mutexattr_t ma;
	pthread_condattr_t ca;
	struct shared *m;
	uint8_t *more;
	int fd, status = -1;
	pid_t pid;

	if (argc > 1)
		return helper();
	setvbuf(stdout, NULL, _IONBF, 0);
	unlink(NAME);
	fd = open(NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || ftruncate(fd, SIZE)) {
		fail("making the file");
		return 1;
	}
	/* Before it is memory, something in it as a file. */
	if (pwrite(fd, "as a file", 9, 4096 * 3) != 9)
		fail("writing the file");
	m = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		fail("mapping the file");
		return 1;
	}
	pthread_mutexattr_init(&ma);
	pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
	pthread_mutex_init(&m->lock, &ma);
	pthread_condattr_init(&ca);
	pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
	pthread_cond_init(&m->cond, &ca);
	strcpy(m->text, "from the client");

	if (posix_spawn(&pid, argv[0], NULL, NULL, again, environ)) {
		fail("starting the helper");
		return 1;
	}
	pthread_mutex_lock(&m->lock);
	for (int i = 0; i < 100; i++) {
		m->turn = 1;
		pthread_cond_broadcast(&m->cond);
		while (m->turn != 2)
			pthread_cond_wait(&m->cond, &m->lock);
	}
	pthread_mutex_unlock(&m->lock);
	if (m->rounds != 100)
		fail("a hundred turns through a lock and a condition in the memory");
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
		printf("FAIL: the helper (%d)\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		failed = 1;
	}
	if (strcmp(m->text, "from the helper"))
		fail("what the helper wrote last");
	if (memcmp((char *)m + 4096 * 5, "as memory", 9))
		fail("what the helper wrote elsewhere");

	/* The file is larger now: all of it, old and new. */
	more = mmap(NULL, 2 * SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (more == MAP_FAILED) {
		fail("mapping the file once it grew");
	} else {
		if (strcmp(((struct shared *)more)->text, "from the helper"))
			fail("the old part of the file once it grew");
		if (sum(more + SIZE, SIZE) != m->sums[0])
			fail("the new part of the file once it grew");
		/* And the first mapping is still the same memory. */
		((struct shared *)more)->turn = 77;
		if (m->turn != 77)
			fail("two mappings of the file in one program");
	}
	unlink(NAME);
	printf(failed ? "FAIL\n" : "PASS\n");
	return failed;
}
