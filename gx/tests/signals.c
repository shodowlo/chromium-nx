/*
 * signals - a program that sends itself signals, from thread to thread.
 *
 * What gx delivers: a signal a thread sends itself, one sent to a thread that
 * sleeps (on a condition, which is a futex), and the one the C library sends
 * every thread when the program changes its group, which is how the Steam
 * client was first seen to need all this. Prints PASS or what failed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int got_usr1, got_usr2, handler_thread_ok;
static pthread_t sleeper;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int asleep, release;

static void on_usr1(int sig)
{
	got_usr1 += sig == SIGUSR1;
}

static void on_usr2(int sig, siginfo_t *info, void *context)
{
	(void)context;
	handler_thread_ok = pthread_equal(pthread_self(), sleeper);
	got_usr2 += sig == SIGUSR2 && info && info->si_signo == SIGUSR2;
}

static void *sleep_until_released(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&lock);
	asleep = 1;
	pthread_cond_broadcast(&cond);
	while (!release)
		pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);
	return NULL;
}

int main(void)
{
	struct sigaction sa;
	int failed = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_usr2;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR2, &sa, NULL);

	/* To itself: handled before raise() returns. */
	raise(SIGUSR1);
	if (got_usr1 != 1) {
		printf("FAIL: a signal to oneself (%d)\n", got_usr1);
		failed = 1;
	}

	/* Signal 0 is a question, not a signal. */
	if (kill(getpid(), 0) || (kill(getpid() + 12345, 0) != -1 || errno != ESRCH)) {
		printf("FAIL: signal 0\n");
		failed = 1;
	}

	/* To a thread that sleeps: it handles it, and goes on sleeping. */
	pthread_create(&sleeper, NULL, sleep_until_released, NULL);
	pthread_mutex_lock(&lock);
	while (!asleep)
		pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);
	pthread_kill(sleeper, SIGUSR2);
	for (int i = 0; i < 200 && !got_usr2; i++)
		usleep(10000);
	if (got_usr2 != 1 || !handler_thread_ok) {
		printf("FAIL: a signal to a sleeping thread (%d, on its thread: %d)\n", got_usr2,
		       handler_thread_ok);
		failed = 1;
	}

	/* A blocked signal waits. */
	{
		sigset_t set, old;

		sigemptyset(&set);
		sigaddset(&set, SIGUSR1);
		pthread_sigmask(SIG_BLOCK, &set, &old);
		raise(SIGUSR1);
		if (got_usr1 != 1) {
			printf("FAIL: a blocked signal was delivered\n");
			failed = 1;
		}
		pthread_sigmask(SIG_SETMASK, &old, NULL);
		if (got_usr1 != 2) {
			printf("FAIL: a signal was not delivered once unblocked (%d)\n", got_usr1);
			failed = 1;
		}
	}

	/* The C library has every thread make this call, by a signal of its
	 * own to each, and waits for all of them. */
	if (setgid(getgid())) {
		printf("FAIL: setgid with two threads: %s\n", strerror(errno));
		failed = 1;
	}

	pthread_mutex_lock(&lock);
	release = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
	pthread_join(sleeper, NULL);
	printf(failed ? "FAIL\n" : "PASS\n");
	return failed;
}
