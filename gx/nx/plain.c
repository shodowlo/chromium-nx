/*
 * The C library's memset() and friends, without the vector registers.
 *
 * While a thread handles a fault (nx/exception.S), Horizon does not keep its
 * vector registers when it takes the processor away for a moment: it kept
 * the ones the fault interrupted and gives those back at the end, and has no
 * place for others. The C library's routines live in those registers, and
 * what handles a fault here maps memory, zeroes it and looks through tables
 * with them: a memchr() that looked through memory that never changes was
 * seen to miss what was there, and fresh pages are no good half zeroed. So
 * everything in the host gets these instead (built without those registers,
 * like gmem.c and the part of host_nx.c that faults go through).
 */
#include <stddef.h>
#include <stdint.h>

typedef uint64_t __attribute__((may_alias, aligned(1))) word;

void *memset(void *to, int byte, size_t n)
{
	uint8_t *p = to;
	uint64_t all = 0x0101010101010101UL * (uint8_t)byte;

	for (; n && ((uintptr_t)p & 7); n--)
		*p++ = (uint8_t)byte;
	for (; n >= 32; n -= 32, p += 32) {
		((word *)p)[0] = all;
		((word *)p)[1] = all;
		((word *)p)[2] = all;
		((word *)p)[3] = all;
	}
	for (; n >= 8; n -= 8, p += 8)
		*(word *)p = all;
	while (n--)
		*p++ = (uint8_t)byte;
	return to;
}

void *memcpy(void *restrict to, const void *restrict from, size_t n)
{
	uint8_t *d = to;
	const uint8_t *s = from;

	for (; n >= 32; n -= 32, d += 32, s += 32) {
		uint64_t a = ((const word *)s)[0], b = ((const word *)s)[1];
		uint64_t c = ((const word *)s)[2], e = ((const word *)s)[3];

		((word *)d)[0] = a;
		((word *)d)[1] = b;
		((word *)d)[2] = c;
		((word *)d)[3] = e;
	}
	for (; n >= 8; n -= 8, d += 8, s += 8)
		*(word *)d = *(const word *)s;
	while (n--)
		*d++ = *s++;
	return to;
}

void *memmove(void *to, const void *from, size_t n)
{
	uint8_t *d = to;
	const uint8_t *s = from;

	if (d >= s + n || s >= d + n)
		return memcpy(to, from, n);
	if (d <= s) {
		/* Forwards, each word read before it is written: memcpy() is
		 * promised that the two do not meet and may do otherwise. */
		for (; n >= 8; n -= 8, d += 8, s += 8) {
			uint64_t w = *(const word *)s;

			*(word *)d = w;
		}
		while (n--)
			*d++ = *s++;
		return to;
	}
	d += n;
	s += n;
	for (; n >= 8; n -= 8) {
		d -= 8;
		s -= 8;
		*(word *)d = *(const word *)s;
	}
	while (n--)
		*--d = *--s;
	return to;
}

void *memchr(const void *in, int byte, size_t n)
{
	const uint8_t *p = in;

	for (; n; n--, p++)
		if (*p == (uint8_t)byte)
			return (void *)p;
	return NULL;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *p = a, *q = b;

	for (; n >= 8 && *(const word *)p == *(const word *)q; n -= 8, p += 8, q += 8)
		;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p < *q ? -1 : 1;
	return 0;
}
