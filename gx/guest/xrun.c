/*
 * xrun.c - runs a program that wants an X display where there is a Wayland
 * one: starts Xwayland, as one window that is the whole X screen, waits for
 * it to be ready, and becomes the program.
 *
 *   xrun <program> [arguments...]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv)
{
	/* -shm: pictures in shared memory, no GL of its own. -displayfd: it
	 * writes the display's number there once it listens. */
	char *server[] = { "/usr/bin/Xwayland", "-displayfd", "3", "-fullscreen", "-shm", "-noreset",
			   "-nolisten", "tcp", "-ac", NULL };
	posix_spawn_file_actions_t actions;
	char number[16], display[20];
	int fds[2], len = 0, n, err;
	pid_t pid;

	if (argc < 2) {
		fprintf(stderr, "usage: xrun <program> [arguments...]\n");
		return 2;
	}
	mkdir("/tmp/.X11-unix", 01777);
	if (pipe(fds)) {
		perror("xrun: pipe");
		return 1;
	}
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, fds[1], 3);
	posix_spawn_file_actions_addclose(&actions, fds[0]);
	if (fds[1] != 3)
		posix_spawn_file_actions_addclose(&actions, fds[1]);
	err = posix_spawn(&pid, server[0], &actions, NULL, server, environ);
	if (err) {
		fprintf(stderr, "xrun: %s: %s\n", server[0], strerror(err));
		return 1;
	}
	close(fds[1]);
	while (len < (int)sizeof(number) - 1 && (n = read(fds[0], number + len, 1)) == 1 &&
	       number[len] != '\n')
		len++;
	number[len] = 0;
	close(fds[0]);
	if (!len) {
		fprintf(stderr, "xrun: the X server did not start\n");
		return 1;
	}
	snprintf(display, sizeof(display), ":%s", number);
	setenv("DISPLAY", display, 1);
	fprintf(stderr, "xrun: X display %s\n", display);
	execv(argv[1], argv + 1);
	fprintf(stderr, "xrun: %s: %s\n", argv[1], strerror(errno));
	return 1;
}
