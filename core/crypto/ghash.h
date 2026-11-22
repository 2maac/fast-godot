/**************************************************************************/
/*  ghash.h                                                               */
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

#pragma once

#include <stddef.h>
#include <stdint.h>

// GHASH over GF(2^128) in the GCM (NIST SP 800-38D) bit order.
//
// This is the universal hash half of GMAC: `tag = GHASH_H(msg) XOR E(K, nonce)`.
// It is used to authenticate encrypted pack files without a whole-file pass:
// the hash is linear over the input, so each independently decoded CFB window
// can be hashed and verified by its own worker, and nothing has to be hashed in
// order at the end of a read.
//
// `GHASHKey` holds the 8-bit precomputed multiplication table for one hash key
// (64 KiB, one row per byte position); build it once per key and share it
// between readers. Measured 1.7 GiB/s on an M5 versus 0.7 GiB/s for the 4-bit
// form, and half the table lookups per block (16 instead of 32).
//
// The table is not constant time: lookup indices are the accumulator's bytes,
// which come from the ciphertext the caller already has, not from the key, so
// an attacker observing cache activity learns nothing it did not supply. `GHASHState` is a tiny per-reader accumulator that
// points at that table, so worker threads do not copy or rebuild it.
//
// The multiply has two implementations behind one API: a portable 8-bit table
// path, and a hardware path using the carry-less multiply instruction (PMULL on
// ARMv8 crypto, PCLMULQDQ on x86-64). The choice is made once per process from
// the CPU features and can be forced to the table path for known-answer tests.
// Both compute exactly the same GHASH; the table path stays because carry-less
// multiplication is not available everywhere (notably wasm, and ARMv8 cores
// built without the crypto extension).

struct GHASHKey {
	// tab[byte_position][value][0] = high 64 bits, [..][1] = low 64 bits of
	// value * x^(8 * byte_position) * H, with value's bit b the coefficient of
	// x^(7 - b).
	uint64_t tab[16][256][2] = {};

	// H in the representation the hardware multiply wants (bit-reversed on
	// ARMv8, byte-reversed on x86); the table path does not use it.
	uint8_t h_native[16] = {};

	void init(const uint8_t p_hash_key[16]);
};

// True when this CPU and build have a carry-less multiply the hardware path can
// use. Evaluated once per process.
bool ghash_native_available();

struct GHASHState {
	const GHASHKey *key = nullptr;
	bool native = false;
	uint64_t hi = 0;
	uint64_t lo = 0;
	// Accumulator of the hardware path, in its own representation (opaque here).
	uint8_t x_native[16] = {};
	uint8_t tail[16] = {};
	uint32_t tail_len = 0;

	void start(const GHASHKey &p_key);
	// Same hash, always through the table path. Used by the known-answer test to
	// prove the two implementations agree.
	void start_portable(const GHASHKey &p_key);
	void reset();
	void update(const uint8_t *p_data, size_t p_len);
	void finish(uint8_t r_tag[16]);

	// Absorbs exactly one 16-byte block. Callers must have no pending partial
	// block (i.e. call it only from a clean state); the pack decoder hashes whole
	// blocks and never needs the tail machinery.
	void update_block(const uint8_t *p_block);
};
