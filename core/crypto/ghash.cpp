/**************************************************************************/
/*  ghash.cpp                                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "ghash.h"

#include <string.h>

// The hardware multiply. ARMv8's PMULL takes two 64-bit operands and produces a
// 128-bit one; x86-64's PCLMULQDQ does the same. The code below is the
// Gueron-Kounavis construction (the one mbedtls uses): a 128x128 bit polynomial
// multiply folded from four 64x64 ones, then a two-step reduction modulo
// x^128 + x^7 + x^2 + x + 1. ARM works in a fully bit-reversed domain (its PMULL
// pairs with RBIT), x86 in a byte-reversed one (the 0xE1 shift of the table path
// folded into the reduction constants).
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
#include <arm_neon.h>
#define GHASH_HAVE_PMULL 1
#elif (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define GHASH_HAVE_PCLMUL 1
#endif

namespace {

// Reduction polynomial of GCM: x^128 + x^7 + x^2 + x + 1. In the GCM bit order
// (block bit 0 = coefficient of x^0 = the block's most significant bit), the
// "multiply by x" step is a 128-bit right shift, and a carry out of the low bit
// folds back in as 0xE1 in the top byte.
constexpr uint64_t GHASH_R = 0xE100000000000000ULL;

inline void mul_x(uint64_t &r_hi, uint64_t &r_lo) {
	const bool carry = (r_lo & 1) != 0;
	r_lo = (r_lo >> 1) | (r_hi << 63);
	r_hi >>= 1;
	if (carry) {
		r_hi ^= GHASH_R;
	}
}

inline uint64_t load_be64(const uint8_t *p) {
	return (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) | (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
			(uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) | (uint64_t(p[6]) << 8) | uint64_t(p[7]);
}

inline void store_be64(uint64_t p_v, uint8_t *p_dst) {
	for (int i = 0; i < 8; i++) {
		p_dst[i] = uint8_t(p_v >> (56 - 8 * i));
	}
}

} // namespace

#if defined(GHASH_HAVE_PMULL)

// ARMv8: one PMULL per 64x64 product; the state and the key are kept bit-reversed
// so a block costs one RBIT plus four PMULLs plus the reduction.
static inline uint8x16_t ghash_pmull_low(uint8x16_t a, uint8x16_t b) {
	return (uint8x16_t)vmull_p64((poly64_t)vget_low_p64((poly64x2_t)a), (poly64_t)vget_low_p64((poly64x2_t)b));
}

static inline uint8x16_t ghash_pmull_high(uint8x16_t a, uint8x16_t b) {
	return (uint8x16_t)vmull_high_p64((poly64x2_t)a, (poly64x2_t)b);
}

// (a * b) reduced modulo the GCM polynomial, in the bit-reversed domain.
static inline uint8x16_t ghash_pmull_mul(uint8x16_t a, uint8x16_t b) {
	const uint8x16_t h = ghash_pmull_high(a, b);
	const uint8x16_t l = ghash_pmull_low(a, b);
	const uint8x16_t c = vextq_u8(b, b, 8);
	const uint8x16_t m = veorq_u8(ghash_pmull_high(a, c), ghash_pmull_low(a, c));
	// Reduction polynomial: x^7 + x^2 + x + 1, i.e. 0x87 in the reversed domain.
	const uint8x16_t modulo = vreinterpretq_u8_u64(vshrq_n_u64(vreinterpretq_u64_u8(vdupq_n_u8(0x87)), 56));
	const uint8x16_t e = veorq_u8(ghash_pmull_high(h, modulo), m);
	const uint8x16_t n = veorq_u8(ghash_pmull_low(h, modulo), l);
	return veorq_u8(veorq_u8(n, ghash_pmull_high(e, modulo)), vextq_u8(vdupq_n_u8(0), e, 8));
}

static void ghash_native_init(uint8_t r_key[16], const uint8_t p_key[16]) {
	vst1q_u8(r_key, vrbitq_u8(vld1q_u8(p_key)));
}

static void ghash_native_block(uint8_t r_x[16], const uint8_t p_key[16], const uint8_t p_block[16]) {
	const uint8x16_t mixed = veorq_u8(vld1q_u8(r_x), vrbitq_u8(vld1q_u8(p_block)));
	vst1q_u8(r_x, ghash_pmull_mul(mixed, vld1q_u8(p_key)));
}

static void ghash_native_finish(const uint8_t p_x[16], uint8_t r_out[16]) {
	vst1q_u8(r_out, vrbitq_u8(vld1q_u8(p_x)));
}

#elif defined(GHASH_HAVE_PCLMUL)

// x86-64: bytes are reversed for the carry-less domain, the product is shifted
// once and reduced with the [CLMUL-WP] algorithm 5 constants (0xE1, 0xC2 and
// x^7 folded in), exactly as in mbedtls' aesni.c.
static const uint8_t ghash_x86_bswap[16] = { 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };

// Every helper that touches PCLMULQDQ or SSSE3 intrinsics has to be compiled with
// those features enabled; the build itself does not pass -mpclmul, and the
// runtime check guarantees the instructions exist.
#define GHASH_X86_TARGET __attribute__((target("pclmul,ssse3")))

GHASH_X86_TARGET static inline __m128i ghash_x86_reverse(__m128i p_v) {
	return _mm_shuffle_epi8(p_v, _mm_loadu_si128((const __m128i *)ghash_x86_bswap));
}

GHASH_X86_TARGET static __m128i ghash_x86_mul(__m128i aa, __m128i bb) {
	__m128i cc = _mm_clmulepi64_si128(aa, bb, 0x00);
	__m128i dd = _mm_clmulepi64_si128(aa, bb, 0x11);
	__m128i ee = _mm_clmulepi64_si128(aa, bb, 0x10);
	__m128i ff = _mm_xor_si128(_mm_clmulepi64_si128(aa, bb, 0x01), ee);
	dd = _mm_xor_si128(dd, _mm_srli_si128(ff, 8));
	cc = _mm_xor_si128(cc, _mm_slli_si128(ff, 8));

	// Shift cc:dd one bit left (algorithm 5, step 1).
	const __m128i cc_lo = _mm_slli_epi64(cc, 1);
	const __m128i dd_lo = _mm_slli_epi64(dd, 1);
	const __m128i cc_hi = _mm_srli_epi64(cc, 63);
	const __m128i dd_hi = _mm_srli_epi64(dd, 63);
	const __m128i carry = _mm_srli_si128(cc_hi, 8);
	cc = _mm_or_si128(cc_lo, _mm_slli_si128(cc_hi, 8));
	dd = _mm_or_si128(_mm_or_si128(dd_lo, _mm_slli_si128(dd_hi, 8)), carry);

	// Reduce (algorithm 5, steps 2-4).
	const __m128i xh = _mm_slli_si128(_mm_xor_si128(_mm_xor_si128(_mm_slli_epi64(cc, 63), _mm_slli_epi64(cc, 62)), _mm_slli_epi64(cc, 57)), 8);
	const __m128i dx = _mm_xor_si128(xh, cc);
	const __m128i e = _mm_srli_epi64(dx, 1);
	const __m128i f = _mm_srli_epi64(dx, 2);
	const __m128i g = _mm_srli_epi64(dx, 7);
	const __m128i hh = _mm_srli_si128(_mm_xor_si128(_mm_xor_si128(_mm_slli_epi64(dx, 63), _mm_slli_epi64(dx, 62)), _mm_slli_epi64(dx, 57)), 8);
	const __m128i mixed = _mm_xor_si128(_mm_xor_si128(_mm_xor_si128(_mm_xor_si128(e, f), g), hh), dx);
	return _mm_xor_si128(mixed, dd);
}

GHASH_X86_TARGET static void ghash_native_init(uint8_t r_key[16], const uint8_t p_key[16]) {
	_mm_storeu_si128((__m128i *)r_key, ghash_x86_reverse(_mm_loadu_si128((const __m128i *)p_key)));
}

GHASH_X86_TARGET static void ghash_native_block(uint8_t r_x[16], const uint8_t p_key[16], const uint8_t p_block[16]) {
	const __m128i mixed = _mm_xor_si128(_mm_loadu_si128((const __m128i *)r_x), ghash_x86_reverse(_mm_loadu_si128((const __m128i *)p_block)));
	_mm_storeu_si128((__m128i *)r_x, ghash_x86_mul(mixed, _mm_loadu_si128((const __m128i *)p_key)));
}

GHASH_X86_TARGET static void ghash_native_finish(const uint8_t p_x[16], uint8_t r_out[16]) {
	_mm_storeu_si128((__m128i *)r_out, ghash_x86_reverse(_mm_loadu_si128((const __m128i *)p_x)));
}

#endif

bool ghash_native_available() {
#if defined(GHASH_HAVE_PMULL)
	return true;
#elif defined(GHASH_HAVE_PCLMUL)
	// PCLMULQDQ implies the SSE2 the reduction needs; the byte reversal uses
	// SSSE3.
	static const bool available = __builtin_cpu_supports("pclmul") && __builtin_cpu_supports("ssse3");
	return available;
#else
	return false;
#endif
}

void GHASHKey::init(const uint8_t p_hash_key[16]) {
#if defined(GHASH_HAVE_PMULL) || defined(GHASH_HAVE_PCLMUL)
	if (ghash_native_available()) {
		ghash_native_init(h_native, p_hash_key);
	}
#endif

	uint64_t hi = load_be64(p_hash_key);
	uint64_t lo = load_be64(p_hash_key + 8);

	// Byte j of a block covers coefficients x^(8j) .. x^(8j+7); the byte's
	// integer bit b (0 = LSB) is the coefficient of x^(8j + 7 - b). Each row is
	// the previous row's key times x^8, built with eight multiply-by-x steps, and
	// each of the 256 byte values is the XOR of its single-bit products.
	for (int j = 0; j < 16; j++) {
		uint64_t single[8][2];
		for (int b = 7; b >= 0; b--) {
			single[b][0] = hi;
			single[b][1] = lo;
			mul_x(hi, lo);
		}

		tab[j][0][0] = 0;
		tab[j][0][1] = 0;
		for (int v = 1; v < 256; v++) {
			// Lowest set bit of v, so each value costs exactly one XOR.
			int bit = 0;
			while ((v & (1 << bit)) == 0) {
				bit++;
			}
			const int rest = v & ~(1 << bit);
			tab[j][v][0] = tab[j][rest][0] ^ single[bit][0];
			tab[j][v][1] = tab[j][rest][1] ^ single[bit][1];
		}
	}
}

void GHASHState::start(const GHASHKey &p_key) {
	key = &p_key;
	native = ghash_native_available();
	reset();
}

void GHASHState::start_portable(const GHASHKey &p_key) {
	key = &p_key;
	native = false;
	reset();
}

void GHASHState::reset() {
	hi = 0;
	lo = 0;
	memset(x_native, 0, sizeof(x_native));
	tail_len = 0;
}

void GHASHState::update_block(const uint8_t *p_block) {
#if defined(GHASH_HAVE_PMULL) || defined(GHASH_HAVE_PCLMUL)
	if (native) {
		ghash_native_block(x_native, key->h_native, p_block);
		return;
	}
#endif
	hi ^= load_be64(p_block);
	lo ^= load_be64(p_block + 8);

	// Multiply the accumulator by H: one table row per byte position, so a block
	// costs 16 lookups, each into the 1 KiB of a row for its byte position.
	uint64_t zh = 0;
	uint64_t zl = 0;
	for (int j = 0; j < 16; j++) {
		const uint8_t byte = j < 8
				? uint8_t(hi >> (56 - 8 * j))
				: uint8_t(lo >> (56 - 8 * (j - 8)));
		zh ^= key->tab[j][byte][0];
		zl ^= key->tab[j][byte][1];
	}
	hi = zh;
	lo = zl;
}

void GHASHState::update(const uint8_t *p_data, size_t p_len) {
	if (p_len == 0) {
		return;
	}
	if (tail_len != 0) {
		size_t take = 16 - tail_len;
		if (take > p_len) {
			take = p_len;
		}
		memcpy(tail + tail_len, p_data, take);
		tail_len += uint32_t(take);
		p_data += take;
		p_len -= take;
		if (tail_len < 16) {
			return;
		}
		update_block(tail);
		tail_len = 0;
	}
	while (p_len >= 16) {
		update_block(p_data);
		p_data += 16;
		p_len -= 16;
	}
	if (p_len != 0) {
		memcpy(tail, p_data, p_len);
		tail_len = uint32_t(p_len);
	}
}

void GHASHState::finish(uint8_t r_tag[16]) {
	if (tail_len != 0) {
		// GCM zero-pads a trailing partial block.
		memset(tail + tail_len, 0, 16 - tail_len);
		update_block(tail);
		tail_len = 0;
	}
#if defined(GHASH_HAVE_PMULL) || defined(GHASH_HAVE_PCLMUL)
	if (native) {
		ghash_native_finish(x_native, r_tag);
		return;
	}
#endif
	store_be64(hi, r_tag);
	store_be64(lo, r_tag + 8);
}
