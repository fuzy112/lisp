#ifndef FLS_H
#define FLS_H

#include <stddef.h>
#include <stdint.h>

/**
 * __fls - find last (most-significant) set bit in a long word
 * @word: the word to search
 *
 * Undefined if no set bit exists, so code should check against 0 first.
 */
static __always_inline unsigned long __fls(unsigned long word)
{
	int num = __WORDSIZE - 1;

#if __WORDSIZE == 64
	if (!(word & (~0ul << 32))) {
		num -= 32;
		word <<= 32;
	}
#endif
	if (!(word & (~0ul << (__WORDSIZE-16)))) {
		num -= 16;
		word <<= 16;
	}
	if (!(word & (~0ul << (__WORDSIZE-8)))) {
		num -= 8;
		word <<= 8;
	}
	if (!(word & (~0ul << (__WORDSIZE-4)))) {
		num -= 4;
		word <<= 4;
	}
	if (!(word & (~0ul << (__WORDSIZE-2)))) {
		num -= 2;
		word <<= 2;
	}
	if (!(word & (~0ul << (__WORDSIZE-1))))
		num -= 1;
	return num;
}


/**
 * fls - find last (most-significant) bit set
 * @x: the word to search
 *
 * This is defined the same way as ffs.
 * Note fls(0) = 0, fls(1) = 1, fls(0x80000000) = 32.
 */

static __always_inline int fls(unsigned int x)
{
	int r = 32;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		x <<= 1;
		r -= 1;
	}
	return r;
}


/**
 * fls64 - find last set bit in a 64-bit word
 * @x: the word to search
 *
 * This is defined in a similar way as the libc and compiler builtin
 * ffsll, but returns the position of the most significant set bit.
 *
 * fls64(value) returns 0 if value is 0 or the position of the last
 * set bit if value is nonzero. The last (most significant) bit is
 * at position 64.
 */
#if __WORDSIZE == 32
static __always_inline int fls64(uint64_t x)
{
	uint32_t h = x >> 32;
	if (h)
		return fls(h) + 32;
	return fls(x);
}
#elif __WORDSIZE == 64
static __always_inline int fls64(uint64_t x)
{
	if (x == 0)
		return 0;
	return __fls(x) + 1;
}
#else
#error __WORDSIZE not 32 or 64
#endif


static inline unsigned fls_long(unsigned long l)
{
	if (sizeof(l) == 4)
		return fls(l);
	return fls64(l);
}

#endif // FLS_H