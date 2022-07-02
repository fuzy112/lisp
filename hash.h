#ifndef HASH_H
#define HASH_H

#include <stdint.h>
#include <stddef.h>

#if __WORDSIZE == 32
#define hash_long(val, bits) hash_32(val, bits)
#elif __WORDSIZE == 64
#define hash_long(val, bits) hash_64(val, bits)
#else
#error Wordsize not 32 or 64
#endif

/*
 * This hash multiplies the input by a large odd number and takes the
 * high bits.  Since multiplication propagates changes to the most
 * significant end only, it is essential that the high bits of the
 * product be used for the hash value.
 *
 * Chuck Lever verified the effectiveness of this technique:
 * http://www.citi.umich.edu/techreports/reports/citi-tr-00-1.pdf
 *
 * Although a random odd number will do, it turns out that the golden
 * ratio phi = (sqrt(5)-1)/2, or its negative, has particularly nice
 * properties.  (See Knuth vol 3, section 6.4, exercise 9.)
 *
 * These are the negative, (1 - phi) = phi**2 = (3 - sqrt(5))/2,
 * which is very slightly easier to multiply by and makes no
 * difference to the hash distribution.
 */
#define GOLDEN_RATIO_32 0x61C88647
#define GOLDEN_RATIO_64 0x61C8864680B583EBull

#define __hash_32 __hash_32_generic
static inline uint32_t __hash_32_generic(uint32_t val)
{
	return val * GOLDEN_RATIO_32;
}

static inline uint32_t hash_32(uint32_t val, unsigned int bits)
{
	/* High bits are more random, so use them. */
	return __hash_32(val) >> (32 - bits);
}

#define hash_64 hash_64_generic
static __always_inline uint32_t hash_64_generic(uint64_t val, unsigned int bits)
{
#if __WORDSIZE == 64
	/* 64x64-bit multiply is efficient on all 64-bit processors */
	return val * GOLDEN_RATIO_64 >> (64 - bits);
#else
	/* Hash 64 bits using only 32x32-bit multiply. */
	return hash_32((uint32_t)val ^ __hash_32(val >> 32), bits);
#endif
}

#endif
