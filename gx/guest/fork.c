/*
 * fork.c - libgxfork.so, loaded ahead of programs (LD_PRELOAD) that start
 * other programs under gx.
 *
 * A process made under gx runs in its parent's memory until it runs a program
 * of its own (see gsys.c). The C library's fork() does not expect that: in
 * the child it takes the process to have one thread left and tidies up after
 * the others, whose stacks and locks are, here, still in use by the parent.
 * This one makes the syscall and little else, which is all a child needs
 * that goes on to run a program.
 *
 * The little else is the environment. A child often sets its own before it
 * runs its program (a few variables, or a whole new list put in `environ`,
 * which may be on its stack), and `environ` is one variable for everybody:
 * - the child gets a list of its own to change, and the parent's is put back
 *   when the child is out of its memory, which is when the syscall returns
 *   in the parent;
 * - meanwhile the other threads of the parent, which go on, are answered
 *   from the parent's list when they look a variable up: getenv() is ours
 *   too, for as long as a child is on its way;
 * - one child at a time: a second thread that forks waits for the first.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define GX_SYS_FORK_KIND 0x47580003

extern char **environ;

static pthread_mutex_t one_at_a_time = PTHREAD_MUTEX_INITIALIZER;
static char **volatile parents;     /* the parent's list while a child is on its way */
static volatile pid_t child;        /* that child's thread, once it runs */

static int is_the_child(void)
{
	return child && child == (pid_t)syscall(SYS_gettid);
}

pid_t fork(void)
{
	char **mine, **copy = NULL;
	size_t count = 0;
	pid_t pid;

	/* Where gx undoes all a child does in its parent's memory, the C
	 * library's own is the right one after all. */
	if (syscall(GX_SYS_FORK_KIND) == 1) {
		static pid_t (*real)(void);

		if (!real)
			real = (pid_t (*)(void))dlsym(RTLD_NEXT, "fork");
		if (real)
			return real();
	}
	/* A child that forks again does it plainly: it holds the turn. */
	if (parents && is_the_child())
		return syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
	pthread_mutex_lock(&one_at_a_time);
	mine = environ;
	if (mine) {
		while (mine[count])
			count++;
		/* Never freed: a thread may be reading it when the child is done. */
		copy = malloc((count + 1) * sizeof(*copy));
		if (copy)
			memcpy(copy, mine, (count + 1) * sizeof(*copy));
	}
	child = 0;
	parents = mine;
	pid = syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
	if (!pid) {
		child = syscall(SYS_gettid);
		if (copy)
			environ = copy;
		return 0;
	}
	environ = mine;
	parents = NULL;
	child = 0;
	pthread_mutex_unlock(&one_at_a_time);
	return pid;
}

static char *look_up(char **list, const char *name)
{
	size_t len = strlen(name);

	for (; list && *list; list++)
		if (!strncmp(*list, name, len) && (*list)[len] == '=')
			return *list + len + 1;
	return NULL;
}

char *getenv(const char *name)
{
	char **list = parents;

	if (list && !is_the_child())
		return look_up(list, name);
	return look_up(environ, name);
}

char *secure_getenv(const char *name)
{
	return getenv(name);
}
