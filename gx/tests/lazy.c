/* Memory that is only there when it is first used (gmem.c, lazy_fault()):
 * the first use is a fault in the middle of whatever the program was doing,
 * and the program must not see it. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 4096

static int failed;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failed = 1;
	}
}

static uint8_t *map(size_t len)
{
	uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return p == MAP_FAILED ? NULL : p;
}

int main(void)
{
	/* A memset() that starts in memory that is there and ends in memory
	 * that is not yet: what the loader does for a thread's storage. */
	for (size_t at = 1; at < PAGE; at += 61) {
		uint8_t *a = map(8 * PAGE);
		size_t len = 0x750;

		check(a != NULL, "mmap");
		if (!a)
			break;
		memset(a, 0x55, PAGE);
		memset(a + PAGE - at, 0, len);
		check(a[PAGE - at - 1] == 0x55, "the byte before is kept");
		for (size_t i = 0; i < len; i++)
			if (a[PAGE - at + i]) {
				check(0, "zeroes across the first use of a page");
				break;
			}
		/* memcpy() and a plain loop, the same way. */
		memset(a, 0x33, PAGE);
		memcpy(a + 3 * PAGE - at, a, len);
		check(!memcmp(a + 3 * PAGE - at, a, len), "a copy across the first use of a page");
		for (size_t i = 0; i < len; i++)
			a[5 * PAGE - at + i] = (uint8_t)i;
		for (size_t i = 0; i < len; i++)
			if (a[5 * PAGE - at + i] != (uint8_t)i) {
				check(0, "bytes across the first use of a page");
				break;
			}
		check(a[8 * PAGE - 1] == 0, "the end of it reads as zero");
		munmap(a, 8 * PAGE);
	}
	/* A store that moves its pointer along. (Not the instruction that
	 * zeroes a block at once, which the loader's memset() uses: qemu
	 * loses its place when that one faults, a real processor does not.) */
	for (int round = 0; round < 16; round++) {
		uint8_t *a = map(2 * PAGE), *b = map(3 * PAGE);
		uint64_t *q, left;

		if (!a || !b)
			break;
		memset(a, 0x77, 2 * PAGE);
		q = (uint64_t *)(b + 2 * PAGE - 64);
		left = 16;
		__asm__ volatile("1: stp %2, %2, [%0, #16]!\n subs %1, %1, #1\n b.hi 1b"
				 : "+r"(q), "+r"(left) : "r"(0x1122334455667788UL) : "cc", "memory");
		check((uint8_t *)q == b + 2 * PAGE - 64 + 16 * 16 && !left, "stores across the first use of a page");
		check(((uint64_t *)(b + 2 * PAGE))[0] == 0x1122334455667788UL, "what the stores left");
		munmap(a, 2 * PAGE);
		munmap(b, 3 * PAGE);
	}
	/* What was unmapped and is mapped again reads as zero. */
	for (int round = 0; round < 64; round++) {
		uint8_t *a = map(32 * PAGE);
		int clean = 1;

		if (!a)
			break;
		for (size_t i = 0; i < 32 * PAGE; i += 257)
			clean &= a[i] == 0;
		check(clean, "fresh memory is zero");
		memset(a, 0xa5, 32 * PAGE);
		munmap(a + (round % 7) * PAGE, (32 - round % 7) * PAGE);
		if (round % 7)
			munmap(a, (round % 7) * PAGE);
	}
	if (!failed)
		printf("PASS\n");
	return failed;
}
