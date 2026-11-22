/**************************************************************************/
/*  test_ghash.cpp                                                        */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_ghash)

#include "core/crypto/crypto_core.h"
#include "core/crypto/ghash.h"

namespace TestGhash {

// GF(2^128) multiplication written straight from the GCM spec (right-shift
// algorithm). The implementation under test is table driven, so this is an
// independent reference for it rather than a second copy of the same code.
static void reference_mul(const uint8_t p_x[16], const uint8_t p_y[16], uint8_t r_z[16]) {
	uint8_t v[16];
	memcpy(v, p_y, 16);
	memset(r_z, 0, 16);
	for (int i = 0; i < 128; i++) {
		if (p_x[i / 8] & (0x80 >> (i % 8))) {
			for (int j = 0; j < 16; j++) {
				r_z[j] ^= v[j];
			}
		}
		const bool carry = (v[15] & 1) != 0;
		for (int j = 15; j > 0; j--) {
			v[j] = uint8_t((v[j] >> 1) | (v[j - 1] << 7));
		}
		v[0] >>= 1;
		if (carry) {
			v[0] ^= 0xE1;
		}
	}
}

static void reference_ghash(const uint8_t p_h[16], const uint8_t *p_data, size_t p_len, uint8_t r_tag[16]) {
	uint8_t acc[16] = {};
	for (size_t offset = 0; offset < p_len; offset += 16) {
		uint8_t block[16] = {};
		const size_t take = MIN<size_t>(16, p_len - offset);
		memcpy(block, p_data + offset, take);
		for (int i = 0; i < 16; i++) {
			acc[i] ^= block[i];
		}
		uint8_t next[16];
		reference_mul(acc, p_h, next);
		memcpy(acc, next, 16);
	}
	memcpy(r_tag, acc, 16);
}

static void from_hex(const char *p_hex, uint8_t *r_out, int p_len) {
	for (int i = 0; i < p_len; i++) {
		uint8_t byte = 0;
		for (int nibble = 0; nibble < 2; nibble++) {
			const char c = p_hex[2 * i + nibble];
			byte <<= 4;
			if (c >= '0' && c <= '9') {
				byte |= uint8_t(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				byte |= uint8_t(c - 'a' + 10);
			}
		}
		r_out[i] = byte;
	}
}

static String to_hex(const uint8_t p_data[16]) {
	static const char *digits = "0123456789abcdef";
	String out;
	for (int i = 0; i < 16; i++) {
		out += String::chr(digits[p_data[i] >> 4]);
		out += String::chr(digits[p_data[i] & 0x0f]);
	}
	return out;
}

TEST_CASE("[GHASH] Matches an independent bitwise GCM multiply") {
	for (int t = 0; t < 64; t++) {
		uint8_t h[16];
		uint8_t data[16 * 7 + 5];
		for (int i = 0; i < 16; i++) {
			h[i] = uint8_t((t * 37 + i * 11 + 3) & 0xff);
		}
		for (size_t i = 0; i < sizeof(data); i++) {
			data[i] = uint8_t((t * 53 + int(i) * 29 + 7) & 0xff);
		}

		GHASHKey key;
		key.init(h);
		GHASHState state;
		state.start(key);
		// Fed in two pieces so the partial-block tail is exercised as well.
		state.update(data, 13);
		state.update(data + 13, sizeof(data) - 13);
		uint8_t got[16];
		state.finish(got);

		uint8_t want[16];
		reference_ghash(h, data, sizeof(data), want);
		CHECK(memcmp(got, want, 16) == 0);
	}
}

TEST_CASE("[GHASH] Reproduces the NIST GCM test vectors") {
	// Test case 1 (AES-128, all-zero key and IV, empty plaintext): the tag is
	// E(K, 0) and GHASH over nothing is zero.
	CryptoCore::AESContext aes;
	uint8_t zero_key[16] = {};
	REQUIRE(aes.set_encode_key(zero_key, 128) == OK);
	uint8_t zero_block[16] = {};
	uint8_t h[16];
	REQUIRE(aes.encrypt_ecb(zero_block, h) == OK);
	CHECK(to_hex(h) == "66e94bd4ef8a2c3b884cfa59ca342b2e");

	GHASHKey key;
	key.init(h);
	GHASHState state;
	state.start(key);
	uint8_t empty_tag[16];
	state.finish(empty_tag);
	uint8_t zeros[16] = {};
	CHECK(memcmp(empty_tag, zeros, 16) == 0);

	// Test case 2 (AES-128, all-zero key, empty IV, one zero block of plaintext):
	// the published tag is ab6e47d42cec13bdf53a67b21257bddf, the ciphertext is
	// 0388dace60b6a392f328c2b971b2fe78 and the tag is
	// GHASH(H, C || [0]_64 || [128]_64) XOR E(K, J0) with J0 = 0^96 || 1.
	uint8_t ciphertext[16];
	from_hex("0388dace60b6a392f328c2b971b2fe78", ciphertext, 16);
	uint8_t message[32] = {};
	memcpy(message, ciphertext, 16);
	message[31] = 128; // len(C) in bits, big endian.

	state.start(key);
	state.update(message, sizeof(message));
	uint8_t s[16];
	state.finish(s);

	uint8_t j0[16] = {};
	j0[15] = 1;
	uint8_t mask[16];
	REQUIRE(aes.encrypt_ecb(j0, mask) == OK);
	for (int i = 0; i < 16; i++) {
		s[i] ^= mask[i];
	}
	CHECK(to_hex(s) == "ab6e47d42cec13bdf53a67b21257bddf");
}

TEST_CASE("[GHASH] Resets cleanly and keeps states independent") {
	uint8_t h[16];
	from_hex("b83b533708bf535d0aa6e52980d53b78", h, 16);
	uint8_t data[16 * 4];
	for (size_t i = 0; i < sizeof(data); i++) {
		data[i] = uint8_t(i * 17 + 1);
	}

	GHASHKey key;
	key.init(h);
	GHASHState a;
	a.start(key);
	a.update(data, sizeof(data));
	uint8_t first[16];
	a.finish(first);
	uint8_t want[16];
	reference_ghash(h, data, sizeof(data), want);
	CHECK(memcmp(first, want, 16) == 0);

	// Reusing the same state without start() must continue the running hash, and
	// start() must give a fresh one.
	a.start(key);
	a.update(data, sizeof(data));
	uint8_t again[16];
	a.finish(again);
	CHECK(memcmp(again, first, 16) == 0);

	GHASHState b;
	b.start(key);
	b.update(data, 16);
	uint8_t prefix[16];
	b.finish(prefix);
	uint8_t want_prefix[16];
	reference_ghash(h, data, 16, want_prefix);
	CHECK(memcmp(prefix, want_prefix, 16) == 0);
}

} // namespace TestGhash
