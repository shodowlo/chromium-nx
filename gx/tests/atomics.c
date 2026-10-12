/*
 * atomics - a program built for a processor newer than the one it runs on:
 * its atomic operations are single instructions of ARMv8.1 (swap, compare
 * and swap, add, and, or, exclusive or, on 1, 2, 4 and 8 bytes), which gx
 * replaces when it loads the program if the processor has none of them. Run
 * on such a processor (qemu's cortex-a57), this checks that each still does
 * what it should, and that they are still atomic. Prints PASS or what failed.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static int failed;

#define CHECK(what, cond) do { if (!(cond)) { printf("FAIL: %s\n", what); failed = 1; } } while (0)

#define TRY(type, name) \
	static __attribute__((noinline)) void try_##name(void) \
	{ \
		volatile type v = (type)0x1122334455667788ULL, old, want; \
		type start = v; \
		\
		old = __atomic_fetch_add(&v, 5, __ATOMIC_SEQ_CST); \
		CHECK(#name " add", old == start && v == (type)(start + 5)); \
		old = __atomic_fetch_sub(&v, 5, __ATOMIC_SEQ_CST); \
		CHECK(#name " sub", old == (type)(start + 5) && v == start); \
		old = __atomic_fetch_or(&v, 0x0f, __ATOMIC_SEQ_CST); \
		CHECK(#name " or", old == start && v == (type)(start | 0x0f)); \
		old = __atomic_fetch_and(&v, (type)~0x0fULL, __ATOMIC_SEQ_CST); \
		CHECK(#name " and", old == (type)(start | 0x0f) && v == (type)(start & ~0x0fULL)); \
		v = start; \
		old = __atomic_fetch_xor(&v, 0x33, __ATOMIC_SEQ_CST); \
		CHECK(#name " xor", old == start && v == (type)(start ^ 0x33)); \
		old = __atomic_exchange_n(&v, 7, __ATOMIC_SEQ_CST); \
		CHECK(#name " swap", old == (type)(start ^ 0x33) && v == 7); \
		want = 7; \
		CHECK(#name " compare and swap", \
		      __atomic_compare_exchange_n(&v, (type *)&want, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && \
		      v == 9 && want == 7); \
		want = 7; \
		CHECK(#name " compare and not swap", \
		      !__atomic_compare_exchange_n(&v, (type *)&want, 11, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && \
		      v == 9 && want == 9); \
		__atomic_store_n(&v, 0, __ATOMIC_SEQ_CST); \
		__atomic_add_fetch(&v, 1, __ATOMIC_SEQ_CST); \
		CHECK(#name " add, result not wanted", v == 1); \
	}

/* The ones a compiler has no name for: written out. A pair compared and
 * swapped as one, and the larger of two kept in memory. */
static __attribute__((noinline)) void try_pairs_and_larger(void)
{
	__attribute__((aligned(16))) uint64_t pair[2] = { 1, 2 };
	uint64_t is_lo = 1, is_hi = 2, flags;
	uint16_t half = 40000;
	int16_t signed_half = -5;
	uint32_t old;

	{
		register uint64_t x2 __asm__("x2") = is_lo, x3 __asm__("x3") = is_hi;
		register uint64_t x6 __asm__("x6") = 10, x7 __asm__("x7") = 20;

		__asm__ volatile(".arch armv8.1-a\n caspal x2, x3, x6, x7, [%2]"
				 : "+r"(x2), "+r"(x3) : "r"(pair), "r"(x6), "r"(x7) : "memory");
		is_lo = x2;
		is_hi = x3;
	}
	CHECK("a pair compared and swapped", is_lo == 1 && is_hi == 2 && pair[0] == 10 && pair[1] == 20);
	{
		register uint64_t x2 __asm__("x2") = 1, x3 __asm__("x3") = 20;
		register uint64_t x6 __asm__("x6") = 7, x7 __asm__("x7") = 8;

		__asm__ volatile(".arch armv8.1-a\n caspal x2, x3, x6, x7, [%2]"
				 : "+r"(x2), "+r"(x3) : "r"(pair), "r"(x6), "r"(x7) : "memory");
		is_lo = x2;
		is_hi = x3;
	}
	CHECK("a pair compared and not swapped", is_lo == 10 && is_hi == 20 && pair[0] == 10 && pair[1] == 20);

	/* The flags are the program's: a comparison before, a look after. */
	__asm__ volatile(".arch armv8.1-a\n cmp xzr, xzr\n ldumaxah %w1, %w0, [%2]\n mrs %3, nzcv"
			 : "=&r"(old), "+r"((uint32_t){ 50000 }), "+r"((uint16_t *){ &half }), "=&r"(flags) : : "memory", "cc");
	CHECK("the larger of two, unsigned", old == 40000 && half == 50000 && flags == 0x60000000);
	__asm__ volatile(".arch armv8.1-a\n ldumaxah %w1, %w0, [%2]"
			 : "=&r"(old), "+r"((uint32_t){ 3 }), "+r"((uint16_t *){ &half }) : : "memory");
	CHECK("the larger of two, unsigned, kept", old == 50000 && half == 50000);
	__asm__ volatile(".arch armv8.1-a\n ldsmaxah %w1, %w0, [%2]"
			 : "=&r"(old), "+r"((uint32_t){ 3 }), "+r"((int16_t *){ &signed_half }) : : "memory");
	CHECK("the larger of two, signed", old == 0xfffb && signed_half == 3);
	__asm__ volatile(".arch armv8.1-a\n ldsminah %w1, %w0, [%2]"
			 : "=&r"(old), "+r"((uint32_t){ 0xfff0 }), "+r"((int16_t *){ &signed_half }) : : "memory");
	CHECK("the smaller of two, signed", old == 3 && signed_half == -16);
}

TRY(uint8_t, bytes)
TRY(uint16_t, halves)
TRY(uint32_t, words)
TRY(uint64_t, longs)

static uint64_t counter;
static uint32_t lock_word;
static uint64_t guarded;

static void *count(void *arg)
{
	(void)arg;
	for (int i = 0; i < 50000; i++) {
		uint32_t free_word = 0;

		__atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
		/* A lock made of compare and swap, around a plain addition. */
		while (!__atomic_compare_exchange_n(&lock_word, &free_word, 1, 0, __ATOMIC_SEQ_CST,
						    __ATOMIC_SEQ_CST))
			free_word = 0;
		guarded++;
		__atomic_exchange_n(&lock_word, 0, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

int main(void)
{
	pthread_t threads[4];

	try_bytes();
	try_halves();
	try_words();
	try_longs();
	try_pairs_and_larger();
	for (int i = 0; i < 4; i++)
		pthread_create(&threads[i], NULL, count, NULL);
	for (int i = 0; i < 4; i++)
		pthread_join(threads[i], NULL);
	CHECK("additions from four threads", counter == 200000);
	CHECK("a lock of compare and swap", guarded == 200000);
	printf(failed ? "FAIL\n" : "PASS\n");
	return failed;
}
