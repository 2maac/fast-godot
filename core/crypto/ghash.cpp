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

void GHASHKey::init(const uint8_t p_hash_key[16]) {
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
	reset();
}

void GHASHState::reset() {
	hi = 0;
	lo = 0;
	tail_len = 0;
}

void GHASHState::update_block(const uint8_t *p_block) {
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
	store_be64(hi, r_tag);
	store_be64(lo, r_tag + 8);
}
