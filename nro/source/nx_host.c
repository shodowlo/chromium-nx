/*
 * nx_host.c - LKL host operations para Horizon OS (Nintendo Switch, libnx)
 *
 * Implementa struct lkl_host_operations usando primitivas do libnx:
 * threads/semáforos/mutex/condvar do kernel Horizon, ticks ARM para tempo,
 * console framebuffer para print.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <switch.h>

#include <lkl_host.h>
#include "iomem.h"
#include "jmp_buf.h"

#define NX_THREAD_STACK  0x40000  /* 256 KB por thread do kernel */
#define NX_THREAD_PRIO   0x2C
#define NX_THREAD_CORE   (-2)

/*
 * Distribuicao de core (v13): todas as threads do kernel em core -2/mesma prio
 * nunca preemptavam umas as outras no escalonador do Horizon — quem roda so cede
 * o core ao BLOQUEAR. O fflush do oplog (I/O real no SD) forcava esse bloqueio;
 * sem ele, uma thread monopolizava o unico core (starvation → boot trava).
 * Espalhar por 3 cores permite paralelismo real. Round-robin atomico.
 */
static int nx_next_core;

static int nx_pick_core(void)
{
	return __atomic_fetch_add(&nx_next_core, 1, __ATOMIC_RELAXED) % 3;
}

/* ---------- oplog: ring buffer de operacoes despejado no SD a cada 1s ----------
 * ponytail: instrumentacao de debug do boot; remover quando o kernel subir */

#define OPLOG_MAX 4000

static unsigned long long oplog_ctr;
static Mutex oplog_mutex;
static FILE *oplog_file;

/* v13: oplog DESLIGADO. Testa a hipotese pura de starvation de core — se bootar
 * so com a distribuicao de cores (sem I/O de debug), e a correcao definitiva. */
static const int oplog_enabled;

static void oplog(const char *op, void *p)
{
	if (!oplog_enabled)
		return;
	mutexLock(&oplog_mutex);
	oplog_ctr++;
	mutexUnlock(&oplog_mutex);
	(void)op;
	(void)p;
}

/* ---------- print / panic ---------- */

/*
 * Regra: SO a thread de display (nx_display_thread, em main.c) chama
 * consoleUpdate(). consoleUpdate submete o framebuffer via libnx e NAO e
 * thread-safe entre threads concorrentes — o print roda em qualquer thread do
 * kernel, entao chamar consoleUpdate aqui (como no v6) causa race na submissao
 * grafica e trava o boot. nx_print apenas escreve no buffer de texto do console
 * (barato, sob mutex) e no log do SD; a thread de display faz o flush.
 */
static Mutex print_mutex;
static FILE *boot_log;

static void nx_print(const char *str, int len)
{
	mutexLock(&print_mutex);
	if (!boot_log)
		boot_log = fopen("sdmc:/lkl-boot.log", "w");
	if (boot_log) {
		fwrite(str, 1, len, boot_log);
		fflush(boot_log);
	}
	fwrite(str, 1, len, stdout); /* escreve no buffer do console, sem flush GPU */
	mutexUnlock(&print_mutex);
}

static void nx_panic(void)
{
	const char msg[] = "\n*** LKL KERNEL PANIC (aperte HOME para sair) ***\n";

	oplog("PANIC", NULL);
	nx_print(msg, sizeof(msg) - 1);
	/* a thread de display continua fazendo o flush; so dormimos aqui */
	while (1)
		svcSleepThread(100000000ULL); /* 100 ms */
}

/* What the kernel asks of the host, counted: see bench_syscalls() in gx. */
unsigned long nx_counts[6];   /* sem_up, sem_down, sem_down that waited, timer_set, mutex_lock, thread_create */
#define COUNT(i) __atomic_add_fetch(&nx_counts[i], 1, __ATOMIC_RELAXED)

/* ---------- semáforos ---------- */

/*
 * Semaforo de CONTAGEM com mutex+condvar (espelha o pthread do POSIX host, que
 * funciona). O Semaphore do libnx tinha semantica que nao batia com o que o LKL
 * espera: sem_up que chega antes do sem_down precisa ACUMULAR (count++), senao a
 * thread dorme pra sempre — deadlock classico de porte de LKL, e a causa do
 * travamento no fim do boot (a sinalizacao do /init se perdia).
 */
struct lkl_sem {
	Mutex m;
	CondVar cv;
	int count;
};

static struct lkl_sem *nx_sem_alloc(int count)
{
	struct lkl_sem *s = malloc(sizeof(*s));

	if (!s)
		return NULL;
	mutexInit(&s->m);
	condvarInit(&s->cv);
	s->count = count;
	oplog("sem_alloc", s);
	return s;
}

static void nx_sem_free(struct lkl_sem *s)
{
	oplog("sem_free", s);
	free(s);
}

static void nx_sem_up(struct lkl_sem *s)
{
	oplog("sem_up", s);
	COUNT(0);
	mutexLock(&s->m);
	s->count++;
	condvarWakeOne(&s->cv);
	mutexUnlock(&s->m);
}

static void nx_sem_down(struct lkl_sem *s)
{
	oplog("sem_down>", s);
	COUNT(1);
	mutexLock(&s->m);
	if (s->count == 0)
		COUNT(2);
	while (s->count == 0)
		condvarWait(&s->cv, &s->m);
	s->count--;
	mutexUnlock(&s->m);
	oplog("sem_down<", s);
}

/* ---------- mutex ---------- */

struct lkl_mutex {
	int recursive;
	Mutex m;
	RMutex rm;
};

static struct lkl_mutex *nx_mutex_alloc(int recursive)
{
	struct lkl_mutex *mx = malloc(sizeof(*mx));

	if (!mx)
		return NULL;
	mx->recursive = recursive;
	if (recursive)
		rmutexInit(&mx->rm);
	else
		mutexInit(&mx->m);
	oplog("mutex_alloc", mx);
	return mx;
}

static void nx_mutex_free(struct lkl_mutex *mx)
{
	oplog("mutex_free", mx);
	free(mx);
}

static void nx_mutex_lock(struct lkl_mutex *mx)
{
	oplog("mutex_lock>", mx);
	COUNT(4);
	if (mx->recursive)
		rmutexLock(&mx->rm);
	else
		mutexLock(&mx->m);
	oplog("mutex_lock<", mx);
}

static void nx_mutex_unlock(struct lkl_mutex *mx)
{
	oplog("mutex_unlock", mx);
	if (mx->recursive)
		rmutexUnlock(&mx->rm);
	else
		mutexUnlock(&mx->m);
}

/* ---------- threads ---------- */

static lkl_thread_t nx_thread_create(void (*f)(void *), void *arg)
{
	Thread *t = malloc(sizeof(*t));
	Result rc;

	COUNT(5);
	if (!t)
		return 0;
	rc = threadCreate(t, f, arg, NULL, NX_THREAD_STACK, NX_THREAD_PRIO,
			  NX_THREAD_CORE);
	if (R_FAILED(rc)) {
		oplog("thread_create FAIL rc", (void *)(uintptr_t)rc);
		free(t);
		return 0;
	}
	rc = threadStart(t);
	if (R_FAILED(rc)) {
		oplog("thread_start FAIL rc", (void *)(uintptr_t)rc);
		threadClose(t);
		free(t);
		return 0;
	}
	oplog("thread_create", t);
	return (lkl_thread_t)t;
}

static void nx_thread_detach(void)
{
	/* ponytail: sem detach no Horizon; Thread structs de threads
	 * não-joinadas vazam (poucos KB, threads do kernel vivem até o fim) */
	oplog("thread_detach", NULL);
}

static void nx_thread_exit(void)
{
	oplog("thread_exit", NULL);
	threadExit();
}

static int nx_thread_join(lkl_thread_t tid)
{
	Thread *t = (Thread *)tid;

	oplog("thread_join>", t);
	if (R_FAILED(threadWaitForExit(t)))
		return -1;
	threadClose(t);
	free(t);
	oplog("thread_join<", t);
	return 0;
}

static lkl_thread_t nx_thread_self(void)
{
	return (lkl_thread_t)threadGetSelf();
}

static int nx_thread_equal(lkl_thread_t a, lkl_thread_t b)
{
	return a == b;
}

static void *nx_thread_stack(unsigned long *size)
{
	Thread *t = threadGetSelf();

	if (!t)
		return NULL;
	if (size)
		*size = t->stack_sz;
	return t->stack_mirror;
}

/* ---------- TLS ---------- */

struct lkl_tls_key {
	s32 slot;
};

static struct lkl_tls_key *nx_tls_alloc(void (*destructor)(void *))
{
	struct lkl_tls_key *k = malloc(sizeof(*k));

	if (!k)
		return NULL;
	k->slot = threadTlsAlloc(destructor);
	oplog("tls_alloc", (void *)(intptr_t)k->slot);
	if (k->slot < 0) {
		free(k);
		return NULL;
	}
	return k;
}

static void nx_tls_free(struct lkl_tls_key *k)
{
	oplog("tls_free", (void *)(intptr_t)k->slot);
	threadTlsFree(k->slot);
	free(k);
}

static int nx_tls_set(struct lkl_tls_key *k, void *data)
{
	threadTlsSet(k->slot, data);
	return 0;
}

static void *nx_tls_get(struct lkl_tls_key *k)
{
	return threadTlsGet(k->slot);
}

/* ---------- memória ---------- */

static void *nx_mem_alloc(unsigned long size)
{
	void *p = malloc(size);

	oplog("mem_alloc", (void *)size);
	return p;
}

static void nx_mem_free(void *p)
{
	free(p);
}

static void *nx_page_alloc(unsigned long size)
{
	void *p = memalign(0x1000, size);

	oplog("page_alloc", (void *)size);
	return p;
}

static void nx_page_free(void *addr, unsigned long size)
{
	free(addr);
}

/* ---------- tempo ---------- */

static unsigned long long nx_time(void)
{
	return armTicksToNs(armGetSystemTick());
}

/* ---------- timers (thread + condvar por timer) ---------- */

struct nx_timer {
	Mutex m;
	CondVar cv;
	Thread thread;
	int armed;
	int stop;
	u64 delta_ns;
	void (*fn)(void);
};

static void nx_timer_thread(void *arg)
{
	struct nx_timer *t = arg;

	mutexLock(&t->m);
	while (!t->stop) {
		if (!t->armed) {
			condvarWait(&t->cv, &t->m);
			continue;
		}
		t->armed = 0;
		/* timeout => disparo; sinal => reavalia armed/stop */
		if (R_FAILED(condvarWaitTimeout(&t->cv, &t->m, t->delta_ns)) &&
		    !t->stop && !t->armed) {
			mutexUnlock(&t->m);
			oplog("timer_fire", t);
			t->fn();
			mutexLock(&t->m);
		}
	}
	mutexUnlock(&t->m);
}

void gx_know_thread(Handle handle, const char *what) __attribute__((weak));

static void *nx_timer_alloc(void (*fn)(void))
{
	struct nx_timer *t = calloc(1, sizeof(*t));
	Result rc;

	if (!t)
		return NULL;
	mutexInit(&t->m);
	condvarInit(&t->cv);
	t->fn = fn;
	/*
	 * Timer com prioridade MAIS ALTA (0x20 < 0x2C) que as threads do kernel.
	 * O timer e o tick do escalonador do Linux: cada disparo destrava a proxima
	 * troca de contexto. Na mesma prio das threads do kernel, ele nunca era
	 * escalonado quando elas monopolizavam o core, e o tick nao chegava (boot
	 * travava). O fflush do oplog mascarava isso ao forcar as threads a bloquear.
	 */
	rc = threadCreate(&t->thread, nx_timer_thread, t, NULL, 0x8000,
			  0x20, NX_THREAD_CORE);
	if (R_FAILED(rc)) {
		free(t);
		return NULL;
	}
	/* gx: told of the thread, for its reports of who is where. */
	if (gx_know_thread)
		gx_know_thread(t->thread.handle, "timer");
	if (R_FAILED(threadStart(&t->thread))) {
		threadClose(&t->thread);
		free(t);
		return NULL;
	}
	oplog("timer_alloc", t);
	return t;
}

static int nx_timer_set_oneshot(void *timer, unsigned long delta)
{
	struct nx_timer *t = timer;

	oplog("timer_set", (void *)delta);
	COUNT(3);
	mutexLock(&t->m);
	t->delta_ns = delta;
	t->armed = 1;
	condvarWakeOne(&t->cv);
	mutexUnlock(&t->m);
	return 0;
}

static void nx_timer_free(void *timer)
{
	struct nx_timer *t = timer;

	oplog("timer_free", t);
	mutexLock(&t->m);
	t->stop = 1;
	condvarWakeAll(&t->cv);
	mutexUnlock(&t->m);
	threadWaitForExit(&t->thread);
	threadClose(&t->thread);
	free(t);
}

/* ---------- jmp_buf (com oplog) ---------- */

static void nx_jmp_buf_set(struct lkl_jmp_buf *jmpb, void (*f)(void))
{
	oplog("jmp_set", jmpb);
	jmp_buf_set(jmpb, f);
}

static void nx_jmp_buf_longjmp(struct lkl_jmp_buf *jmpb, int val)
{
	oplog("jmp_longjmp", jmpb);
	jmp_buf_longjmp(jmpb, val);
}

/* ---------- memcpy & cia ---------- */

static void *nx_memcpy(void *dest, const void *src, unsigned long count)
{
	return memcpy(dest, src, count);
}

static void *nx_memset(void *s, int c, unsigned long count)
{
	return memset(s, c, count);
}

static void *nx_memmove(void *dest, const void *src, unsigned long count)
{
	return memmove(dest, src, count);
}

/* ---------- tabela de operações ---------- */

struct lkl_host_operations lkl_host_ops = {
	.virtio_devices = NULL,

	.print = nx_print,
	.panic = nx_panic,

	.sem_alloc = nx_sem_alloc,
	.sem_free = nx_sem_free,
	.sem_up = nx_sem_up,
	.sem_down = nx_sem_down,

	.mutex_alloc = nx_mutex_alloc,
	.mutex_free = nx_mutex_free,
	.mutex_lock = nx_mutex_lock,
	.mutex_unlock = nx_mutex_unlock,

	.thread_create = nx_thread_create,
	.thread_detach = nx_thread_detach,
	.thread_exit = nx_thread_exit,
	.thread_join = nx_thread_join,
	.thread_self = nx_thread_self,
	.thread_equal = nx_thread_equal,
	.thread_stack = nx_thread_stack,

	.tls_alloc = nx_tls_alloc,
	.tls_free = nx_tls_free,
	.tls_set = nx_tls_set,
	.tls_get = nx_tls_get,

	.mem_alloc = nx_mem_alloc,
	.mem_free = nx_mem_free,
	.page_alloc = nx_page_alloc,
	.page_free = nx_page_free,

	.time = nx_time,

	.timer_alloc = nx_timer_alloc,
	.timer_set_oneshot = nx_timer_set_oneshot,
	.timer_free = nx_timer_free,

	.ioremap = lkl_ioremap,
	.iomem_access = lkl_iomem_access,

	.jmp_buf_set = nx_jmp_buf_set,
	.jmp_buf_longjmp = nx_jmp_buf_longjmp,

	.memcpy = nx_memcpy,
	.memset = nx_memset,
	.memmove = nx_memmove,

	/* mmap/munmap/shmem: LKL sem MMU não usa; adicionar se o link cobrar */
};
