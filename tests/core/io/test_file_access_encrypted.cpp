/**************************************************************************/
/*  test_file_access_encrypted.cpp                                                  */
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

TEST_FORCE_LINK(test_file_access_encrypted)

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/io/file_access_encrypted.h"
#include "tests/test_utils.h"

namespace TestFileAccessEncrypted {

// Stands in for the white-box tables: AES-256 block encryption under `keystream_key`.
static Vector<uint8_t> keystream_key;
static bool keystream_block(const uint8_t p_in[16], uint8_t r_out[16]) {
	CryptoCore::AESContext ctx;
	ERR_FAIL_COND_V(ctx.set_encode_key(keystream_key.ptrw(), 256) != OK, false);
	return ctx.encrypt_ecb(p_in, r_out) == OK;
}

static Vector<uint8_t> make_bytes(int p_count, uint8_t p_seed) {
	Vector<uint8_t> bytes;
	bytes.resize(p_count);
	for (int i = 0; i < p_count; i++) {
		bytes.write[i] = uint8_t((i * 37 + p_seed * 11 + 3) & 0xff);
	}
	return bytes;
}

static Vector<uint8_t> make_key(uint8_t p_seed) {
	Vector<uint8_t> key;
	key.resize(32);
	for (int i = 0; i < 32; i++) {
		key.write[i] = uint8_t(p_seed + i * 7);
	}
	return key;
}

// Writes `p_payload` through the key path and returns the file path.
static String write_encrypted(const Vector<uint8_t> &p_payload, const Vector<uint8_t> &p_key) {
	const String path = TestUtils::get_temp_path("fae_keystream.bin");
	Ref<FileAccess> base = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(base.is_valid());
	Ref<FileAccessEncrypted> writer;
	writer.instantiate();
	REQUIRE(writer->open_and_parse(base, p_key, FileAccessEncrypted::MODE_WRITE_AES256, true) == OK);
	REQUIRE(writer->store_buffer(p_payload.ptr(), p_payload.size()));
	writer->close();
	REQUIRE(base->get_error() == OK);
	base->close();
	return path;
}

TEST_CASE("[FileAccessEncrypted] Keystream decoding matches the key path") {
	const Vector<uint8_t> key = make_key(0x10);
	// Deliberately not a multiple of the AES block size.
	const Vector<uint8_t> payload = make_bytes(1000, 0x20);
	const String path = write_encrypted(payload, key);

	keystream_key = key;

	// Sequential read through the keystream provider.
	{
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);
		REQUIRE(reader->get_length() == payload.size());

		Vector<uint8_t> got;
		got.resize(payload.size());
		REQUIRE(reader->get_buffer(got.ptrw(), got.size()) == payload.size());
		REQUIRE(got == payload);
		REQUIRE(reader->eof_reached() == false);

		// A read past the end returns the remainder and marks EOF.
		reader->seek(990);
		uint8_t tail[64];
		REQUIRE(reader->get_buffer(tail, sizeof(tail)) == 10);
		REQUIRE(memcmp(tail, payload.ptr() + 990, 10) == 0);
		REQUIRE(reader->eof_reached() == true);
	}

	// Random access: CFB only needs the previous ciphertext block, so any offset
	// must decode without replaying from the IV.
	{
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);

		const uint64_t offsets[] = { 0, 1, 15, 16, 17, 255, 500, 993, 999 };
		for (uint64_t offset : offsets) {
			const uint64_t want = MIN<uint64_t>(48, payload.size() - offset);
			uint8_t chunk[48];
			reader->seek(offset);
			REQUIRE(reader->get_buffer(chunk, want) == want);
			REQUIRE(memcmp(chunk, payload.ptr() + offset, want) == 0);
			// Backwards, to prove the seek does not depend on read order.
			reader->seek(offset);
			REQUIRE(reader->get_buffer(chunk, want) == want);
			REQUIRE(memcmp(chunk, payload.ptr() + offset, want) == 0);
		}

		// Same file through the key path must produce identical bytes.
		Ref<FileAccess> keyed_base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(keyed_base.is_valid());
		Ref<FileAccessEncrypted> keyed;
		keyed.instantiate();
		REQUIRE(keyed->open_and_parse(keyed_base, key, FileAccessEncrypted::MODE_READ, true) == OK);
		Vector<uint8_t> keyed_all;
		keyed_all.resize(payload.size());
		REQUIRE(keyed->get_buffer(keyed_all.ptrw(), keyed_all.size()) == payload.size());
		REQUIRE(keyed_all == payload);
	}

	// A provider that decrypts with another key must fail the integrity check. Pack
	// files verify while they are read, so with this payload the check has already
	// completed (one read covers the whole file) and the failing chunk is refused.
	// The engine logs the failure, which is expected here.
	keystream_key = make_key(0x90);
	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);
		Vector<uint8_t> got;
		got.resize(payload.size());
		REQUIRE(reader->get_buffer(got.ptrw(), got.size()) == 0);
		REQUIRE(reader->get_error() == ERR_FILE_CORRUPT);
		ERR_PRINT_ON;
	}
	// The pack-directory form verifies at open and must fail there.
	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true, true) == ERR_FILE_CORRUPT);
		ERR_PRINT_ON;
	}
	keystream_key.clear();
}

TEST_CASE("[FileAccessEncrypted] Keystream integrity check is lazy for pack files") {
	const Vector<uint8_t> key = make_key(0x30);
	// Larger than one decode window: the old open-time pass would have decoded the
	// whole payload once before returning.
	const Vector<uint8_t> payload = make_bytes(200 * 1024, 0x40);
	const String path = write_encrypted(payload, key);
	keystream_key = key;

	// Chunk size deliberately neither a divisor of the payload nor a block multiple.
	constexpr uint64_t chunk = 7919;

	// Sequential reads are hashed as they decode: the bytes are identical, the check
	// passes at the end of the file, and the error is a plain EOF.
	{
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);
		REQUIRE(reader->get_length() == payload.size());

		Vector<uint8_t> got;
		got.resize(payload.size());
		uint64_t done = 0;
		while (done < got.size()) {
			const uint64_t want = MIN<uint64_t>(chunk, got.size() - done);
			const uint64_t read = reader->get_buffer(got.ptrw() + done, want);
			REQUIRE(read == want);
			done += read;
		}
		REQUIRE(got == payload);

		uint8_t extra = 0;
		REQUIRE(reader->get_buffer(&extra, 1) == 0);
		REQUIRE(reader->eof_reached() == true);
		REQUIRE(reader->get_error() == ERR_FILE_EOF);
	}

	// A seek breaks the sequential pass: the data stays correct, but the check is
	// dropped instead of decoding bytes the caller did not ask for.
	{
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);

		uint8_t head[64];
		REQUIRE(reader->get_buffer(head, sizeof(head)) == sizeof(head));
		REQUIRE(memcmp(head, payload.ptr(), sizeof(head)) == 0);

		reader->seek(150000);
		uint8_t mid[4096];
		REQUIRE(reader->get_buffer(mid, sizeof(mid)) == sizeof(mid));
		REQUIRE(memcmp(mid, payload.ptr() + 150000, sizeof(mid)) == 0);
		REQUIRE(reader->get_error() == OK);
	}

	// A wrong provider is caught at the end of the read: the failing chunk is
	// refused, get_error() reports corruption, and later reads stay empty.
	keystream_key = make_key(0x91);
	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);

		Vector<uint8_t> got;
		got.resize(payload.size());
		REQUIRE(reader->get_buffer(got.ptrw(), got.size()) == 0);
		REQUIRE(reader->get_error() == ERR_FILE_CORRUPT);

		uint8_t again[16];
		REQUIRE(reader->get_buffer(again, sizeof(again)) == 0);
		REQUIRE(reader->get_error() == ERR_FILE_CORRUPT);
		ERR_PRINT_ON;
	}

	// The pack-directory form verifies at open, so a template/pack mismatch cannot
	// load the pack at all.
	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true, true) == ERR_FILE_CORRUPT);
		ERR_PRINT_ON;
	}
	keystream_key.clear();
}

TEST_CASE("[FileAccessEncrypted] Large keystream files decode in parallel windows") {
	// Above the parallel threshold (1 MiB) and not a multiple of the block size, so
	// the head, the middle windows and the trailing bytes are all exercised.
	const Vector<uint8_t> key = make_key(0x50);
	const Vector<uint8_t> payload = make_bytes(3 * 1024 * 1024 + 7, 0x60);
	const String path = write_encrypted(payload, key);
	keystream_key = key;

	// One read of the whole file: aligned head, parallel middle, 7-byte tail. The
	// lazy digest is fed from the plaintext and compared at EOF.
	{
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);
		REQUIRE(reader->get_length() == payload.size());

		Vector<uint8_t> got;
		got.resize(payload.size());
		REQUIRE(reader->get_buffer(got.ptrw(), got.size()) == payload.size());
		REQUIRE(got == payload);
		REQUIRE(reader->get_error() != ERR_FILE_CORRUPT);
	}

	// A read that starts and ends inside blocks: one decoded head block, parallel
	// windows in the middle, one decoded tail block.
	{
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);

		const uint64_t offset = 11;
		const uint64_t want = 2 * 1024 * 1024 + 5;
		Vector<uint8_t> got;
		got.resize(want);
		reader->seek(offset);
		REQUIRE(reader->get_buffer(got.ptrw(), got.size()) == want);
		REQUIRE(memcmp(got.ptr(), payload.ptr() + offset, want) == 0);
	}

	// Sequentially in odd chunks, every read above the threshold: the data must be
	// identical and the trailing MD5 must still pass.
	{
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);

		Vector<uint8_t> got;
		got.resize(payload.size());
		uint64_t done = 0;
		const uint64_t chunk = 1024 * 1024 + 3;
		while (done < got.size()) {
			const uint64_t take = MIN<uint64_t>(chunk, got.size() - done);
			const uint64_t read = reader->get_buffer(got.ptrw() + done, take);
			REQUIRE(read == take);
			done += read;
		}
		REQUIRE(got == payload);
		REQUIRE(reader->get_error() != ERR_FILE_CORRUPT);
	}

	// A wrong provider must still be caught after the parallel decode.
	keystream_key = make_key(0x93);
	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);
		Vector<uint8_t> got;
		got.resize(payload.size());
		REQUIRE(reader->get_buffer(got.ptrw(), got.size()) == 0);
		REQUIRE(reader->get_error() == ERR_FILE_CORRUPT);
		ERR_PRINT_ON;
	}
	keystream_key.clear();
}

} // namespace TestFileAccessEncrypted
