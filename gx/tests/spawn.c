/*
 * spawn - a program that runs other programs (itself, told to do something
 * else) the ways programs do: fork and exec, vfork, posix_spawn, a script.
 *
 * What gx gives each: a number of its own, files of its own (what a child
 * writes to its standard output arrives in its parent's pipe, and the pipe
 * ends when the child does), threads that share those files, an end the
 * parent learns of. Prints PASS or what failed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static const char self[] = "/bin/spawn";
extern char **environ;
static int failed;
static volatile int children, stop;

static void fail(const char *what, const char *detail)
{
	printf("FAIL: %s (%s)\n", what, detail);
	failed = 1;
}

static void *say_from_thread(void *arg)
{
	(void)!write(1, arg, strlen(arg));
	return NULL;
}

/* What the program does when it is run by itself. */
static int child(int argc, char **argv)
{
	if (!strcmp(argv[1], "exit"))
		return atoi(argv[2]);
	if (!strcmp(argv[1], "say")) {
		for (int i = 2; i < argc; i++)
			printf("%s%s", i > 2 ? " " : "", argv[i]);
		printf("\n");
		return 0;
	}
	if (!strcmp(argv[1], "pids")) {
		printf("%d %d\n", getpid(), getppid());
		return 0;
	}
	if (!strcmp(argv[1], "thread")) {
		pthread_t t;

		/* A file opened here must not be its parent's. */
		pthread_create(&t, NULL, say_from_thread, "from a thread\n");
		pthread_join(t, NULL);
		return 0;
	}
	if (!strcmp(argv[1], "sleep")) {
		for (;;)
			usleep(20000);
	}
	if (!strcmp(argv[1], "kept")) {
		/* Told the number of a file its parent meant to keep to itself. */
		return fcntl(atoi(argv[2]), F_GETFD) == -1 && errno == EBADF ? 0 : 1;
	}
	return 99;
}

/* Runs itself with these arguments, its output in a pipe: what it wrote,
 * and how it ended in *status. */
static const char *run(char *const argv[], int *status, int with_spawn)
{
	static char out[256];
	int fds[2], n, len = 0;
	pid_t pid;

	out[0] = 0;
	if (pipe(fds))
		return "no pipe";
	if (with_spawn) {
		posix_spawn_file_actions_t actions;

		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
		posix_spawn_file_actions_addclose(&actions, fds[0]);
		posix_spawn_file_actions_addclose(&actions, fds[1]);
		if (posix_spawn(&pid, argv[0], &actions, NULL, argv, environ))
			return "posix_spawn failed";
		posix_spawn_file_actions_destroy(&actions);
	} else {
		pid = fork();
		if (pid < 0)
			return "fork failed";
		if (!pid) {
			dup2(fds[1], 1);
			close(fds[0]);
			close(fds[1]);
			execv(argv[0], argv);
			_exit(errno == ENOENT ? 44 : 45);
		}
	}
	close(fds[1]);
	/* To the end of the pipe: there is one only if the child's files are
	 * closed when it ends. */
	while (len < (int)sizeof(out) - 1 && (n = read(fds[0], out + len, sizeof(out) - 1 - len)) > 0)
		len += n;
	out[len] = 0;
	close(fds[0]);
	if (waitpid(pid, status, 0) != pid)
		return "waitpid failed";
	return out;
}

static void on_child(int sig)
{
	(void)sig;
	children++;
}

static void *count(void *arg)
{
	volatile long *n = arg;

	while (!stop)
		(*n)++;
	return NULL;
}

int main(int argc, char **argv)
{
	char *exit7[] = { (char *)self, "exit", "7", NULL };
	char *say[] = { (char *)self, "say", "hello", "there", NULL };
	char *pids[] = { (char *)self, "pids", NULL };
	char *thread[] = { (char *)self, "thread", NULL };
	char *missing[] = { "/bin/not-there", NULL };
	char *script[] = { "/dev/shm/script", "last", NULL };
	char *sleeper[] = { (char *)self, "sleep", NULL };
	char *kept[] = { (char *)self, "kept", NULL, NULL };
	char text[64], number[16];
	struct sigaction sa;
	volatile long counter = 0;
	volatile int mine = 1;
	pthread_t counting;
	const char *out;
	int status = -1, fd, a, b;
	pid_t pid;

	if (argc > 1)
		return child(argc, argv);
	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_child;
	sigaction(SIGCHLD, &sa, NULL);
	/* What a forked child changes on its way to its program is not
	 * changed here: that much is certain while there is one thread. */
	{
		static int number = 5;
		char *heap = strdup("parent");

		pid = fork();
		if (!pid) {
			number = 6;
			strcpy(heap, "child");
			free(heap);
			free(malloc(1 << 20));
			execv(self, exit7);
			_exit(1);
		}
		if (waitpid(pid, &status, 0) != pid || WEXITSTATUS(status) != 7)
			fail("a child that changes things", "its end");
		if (number != 5 || strcmp(heap, "parent"))
			fail("a child that changes things", "they are changed for its parent");
		free(heap);
	}

	/* Another thread, which goes on while children come and go. */
	pthread_create(&counting, NULL, count, (void *)&counter);

	out = run(exit7, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 7)
		fail("fork, exec, exit 7", out);

	out = run(say, &status, 0);
	if (strcmp(out, "hello there\n") || status)
		fail("what a child writes", out);

	out = run(say, &status, 1);
	if (strcmp(out, "hello there\n") || status)
		fail("what a spawned child writes", out);

	out = run(pids, &status, 0);
	if (sscanf(out, "%d %d", &a, &b) != 2 || a == getpid() || b != getpid())
		fail("a child's number and its parent's", out);

	out = run(thread, &status, 1);
	if (strcmp(out, "from a thread\n") || status)
		fail("a thread of a child writes to the child's files", out);

	out = run(missing, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 44)
		fail("a program that is not there", out);
	if (posix_spawn(&pid, missing[0], NULL, NULL, missing, environ) != ENOENT)
		fail("spawning a program that is not there", "no ENOENT");

	/* A script: run by the program its first line names. */
	fd = open(script[0], O_WRONLY | O_CREAT | O_TRUNC, 0755);
	snprintf(text, sizeof(text), "#!%s say\n", self);
	if (fd < 0 || write(fd, text, strlen(text)) < 0)
		fail("writing a script", strerror(errno));
	close(fd);
	out = run(script, &status, 0);
	if (strcmp(out, "/dev/shm/script last\n") || status)
		fail("a script", out);

	/* A file not meant for children. */
	fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	snprintf(number, sizeof(number), "%d", fd);
	kept[2] = number;
	out = run(kept, &status, 0);
	if (status)
		fail("a file kept from a child", out);
	if (fcntl(fd, F_GETFD) == -1)
		fail("a file kept from a child", "the parent lost it");
	close(fd);

	/* vfork: the child borrows everything until it is gone. */
	pid = vfork();
	if (!pid)
		_exit(3);
	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 3)
		fail("vfork", "");

	/* What a forked child changes on its way is not changed here. */
	pid = fork();
	if (!pid) {
		mine = 2;
		close(1);
		_exit(mine);
	}
	if (waitpid(pid, &status, 0) != pid || WEXITSTATUS(status) != 2 || mine != 1)
		fail("a forked child's changes", mine == 1 ? "its end" : "they are the parent's");
	if (write(1, "", 0) < 0)
		fail("a forked child's files", "it closed its parent's output");

	/* Nor what it changes anywhere else, with the other thread running. */
	{
		static int number = 5;

		pid = fork();
		if (!pid) {
			number = 6;
			execv(self, exit7);
			_exit(1);
		}
		if (waitpid(pid, &status, 0) != pid || WEXITSTATUS(status) != 7)
			fail("a child that changes things, with threads", "its end");
		/* Where gx can undo it, which takes a host that lets it (it
		 * says so). */
		if (number != 5 && syscall(0x47580003 /* GX_SYS_FORK_KIND */) == 1)
			fail("a child that changes things, with threads", "they are changed for its parent");
		number = 5;
	}

	/* A child that does not end is ended. */
	posix_spawn(&pid, self, NULL, NULL, sleeper, environ);
	if (waitpid(pid, &status, WNOHANG) != 0)
		fail("a child that still runs", "waitpid did not say 0");
	kill(pid, SIGTERM);
	if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM)
		fail("a child that is ended", "");
	if (waitpid(-1, &status, 0) != -1 || errno != ECHILD)
		fail("no child left", "waitpid found one");

	if (children < 10) {
		snprintf(text, sizeof(text), "%d", children);
		fail("being told of children's ends", text);
	}
	if (!counter)
		fail("the other thread", "it never ran");
	stop = 1;
	pthread_join(counting, NULL);
	printf(failed ? "FAIL\n" : "PASS\n");
	return failed;
}
