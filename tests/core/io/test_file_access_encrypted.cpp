/**************************************************************************/
/*  test_file_access_encrypted.cpp                                        */
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

// The old (MD5) header magic, kept only to build a file the reader must reject.
constexpr uint32_t V1_HEADER_MAGIC = 0x43454447;

// V2 header layout, with the magic:
//   0 magic u32 | 4 signature[16] | 20 length u64 | 28 chunk_count u64
//   36 chunk_blocks u32 | 40 reserved u32 | 44 iv[16] | 60 tags[16 * chunks]
constexpr uint64_t V2_LENGTH_OFFSET = 20;
constexpr uint64_t V2_CHUNK_COUNT_OFFSET = 28;
constexpr uint64_t V2_TAGS_OFFSET = 60;
constexpr uint64_t CHUNK_BYTES = 64 * 1024;

// Stands in for the white-box tables: AES-256 block encryption under `keystream_key`.
static Vector<uint8_t> keystream_key;
// Per-thread key schedule: the provider is called per block, and on worker
// threads too, so it must not rebuild the AES context every time.
static thread_local Vector<uint8_t> keystream_cached_key;
static thread_local CryptoCore::AESContext keystream_ctx;
static bool keystream_block(const uint8_t p_in[16], uint8_t r_out[16]) {
	// Called from worker threads too. Read the shared global through its const
	// pointer (ptrw() may copy-on-write and is not thread safe) and keep the
	// per-thread copy that the key schedule is built from.
	if (keystream_key.size() != 32) {
		ERR_FAIL_V_MSG(false, "The keystream provider has no key.");
	}
	if (keystream_cached_key.size() != 32 || memcmp(keystream_cached_key.ptr(), keystream_key.ptr(), 32) != 0) {
		uint8_t raw[32];
		memcpy(raw, keystream_key.ptr(), 32);
		ERR_FAIL_COND_V(keystream_ctx.set_encode_key(raw, 256) != OK, false);
		keystream_cached_key.resize(32);
		memcpy(keystream_cached_key.ptrw(), raw, 32);
	}
	return keystream_ctx.encrypt_ecb(p_in, r_out) == OK;
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

// Writes `p_payload` through the key path (the writer always emits V2) and
// returns the file path.
static String write_encrypted(const Vector<uint8_t> &p_payload, const Vector<uint8_t> &p_key, bool p_with_magic = true) {
	const String path = TestUtils::get_temp_path("fae_keystream.bin");
	Ref<FileAccess> base = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(base.is_valid());
	Ref<FileAccessEncrypted> writer;
	writer.instantiate();
	REQUIRE(writer->open_and_parse(base, p_key, FileAccessEncrypted::MODE_WRITE_AES256, p_with_magic) == OK);
	REQUIRE(writer->store_buffer(p_payload.ptr(), p_payload.size()));
	writer->close();
	REQUIRE(base->get_error() == OK);
	base->close();
	return path;
}

// Builds an old (MD5, V1) file by hand, which the reader now refuses.
static String write_v1(const Vector<uint8_t> &p_payload, const Vector<uint8_t> &p_key) {
	const String path = TestUtils::get_temp_path("fae_v1.bin");
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(file.is_valid());

	unsigned char hash[16];
	REQUIRE(CryptoCore::md5(p_payload.ptr(), p_payload.size(), hash) == OK);

	file->store_32(V1_HEADER_MAGIC);
	file->store_buffer(hash, 16);
	file->store_64(p_payload.size());

	uint8_t iv[16];
	for (int i = 0; i < 16; i++) {
		iv[i] = uint8_t(0x80 + i);
	}
	file->store_buffer(iv, 16);

	uint64_t ct_len = p_payload.size();
	if (ct_len % 16) {
		ct_len += 16 - (ct_len % 16);
	}
	Vector<uint8_t> ciphertext;
	ciphertext.resize(ct_len);
	if (p_payload.size() != 0) {
		memcpy(ciphertext.ptrw(), p_payload.ptr(), p_payload.size());
	}
	if (ct_len > p_payload.size()) {
		memset(ciphertext.ptrw() + p_payload.size(), 0, ct_len - p_payload.size());
	}

	CryptoCore::AESContext ctx;
	Vector<uint8_t> key_copy = p_key;
	REQUIRE(ctx.set_encode_key(key_copy.ptrw(), 256) == OK);
	uint8_t cfb_iv[16];
	memcpy(cfb_iv, iv, 16);
	if (ct_len != 0) {
		uint8_t *ct_ptr = ciphertext.ptrw();
		REQUIRE(ctx.encrypt_cfb(ct_len, cfb_iv, ct_ptr, ct_ptr) == OK);
	}
	file->store_buffer(ciphertext.ptr(), ciphertext.size());
	file->close();
	return path;
}

static void patch_bytes(const String &p_path, uint64_t p_offset, const uint8_t *p_bytes, uint64_t p_len) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ_WRITE);
	REQUIRE(file.is_valid());
	file->seek(p_offset);
	file->store_buffer(p_bytes, p_len);
	file->close();
}

// XORs one byte in place, so the same call undoes it (an overwrite would be a
// no-op if the byte already had that value).
static void xor_byte(const String &p_path, uint64_t p_offset, uint8_t p_mask) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ_WRITE);
	REQUIRE(file.is_valid());
	file->seek(p_offset);
	const uint8_t value = file->get_8();
	file->seek(p_offset);
	file->store_8(uint8_t(value ^ p_mask));
	file->close();
}

static void patch_offset(const String &p_path, uint64_t p_offset, uint64_t p_value) {
	uint8_t bytes[8];
	for (int i = 0; i < 8; i++) {
		bytes[i] = uint8_t(p_value >> (8 * i));
	}
	patch_bytes(p_path, p_offset, bytes, 8);
}

static uint64_t v2_ciphertext_offset(uint64_t p_chunks) {
	return V2_TAGS_OFFSET + p_chunks * 16;
}

// Reads the whole file through the keystream path. Returns false when the open or
// the read failed (including a failed tag check).
static bool read_all_keystream(const String &p_path, const Vector<uint8_t> &p_key, Vector<uint8_t> &r_data, bool p_eager = false, bool p_with_magic = true) {
	keystream_key = p_key;
	Ref<FileAccess> base = FileAccess::open(p_path, FileAccess::READ);
	if (base.is_null()) {
		return false;
	}
	Ref<FileAccessEncrypted> reader;
	reader.instantiate();
	if (reader->open_and_parse_keystream(base, keystream_block, p_with_magic, p_eager) != OK) {
		return false;
	}
	r_data.resize(reader->get_length());
	if (r_data.size() != 0 && reader->get_buffer(r_data.ptrw(), r_data.size()) != uint64_t(r_data.size())) {
		return false;
	}
	return reader->get_error() != ERR_FILE_CORRUPT;
}

TEST_CASE("[FileAccessEncrypted] Keystream decoding matches the key path") {
	const Vector<uint8_t> key = make_key(0x10);
	// Deliberately not a multiple of the AES block size.
	const Vector<uint8_t> payload = make_bytes(1000, 0x20);
	const String path = write_encrypted(payload, key);

	// The writer emits the authenticated V2 header.
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
		REQUIRE(file.is_valid());
		CHECK(file->get_32() == ENCRYPTED_HEADER_MAGIC);
		uint8_t signature[16];
		file->get_buffer(signature, 16);
		CHECK(memcmp(signature, "GDE2GMAC", 8) == 0);
		CHECK(file->get_64() == 1000);
		CHECK(file->get_64() == 1);
		CHECK(file->get_32() == ENCRYPTED_CHUNK_BLOCKS);
	}

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
	// must decode without replaying from the IV. Every touched chunk is verified.
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

	// A provider that decrypts with another key must fail authentication. Pack
	// files verify chunk by chunk as they are read.
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
	// The same file with the right provider but the wrong key on the key path.
	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse(base, make_key(0x91), FileAccessEncrypted::MODE_READ, true) == ERR_FILE_CORRUPT);
		ERR_PRINT_ON;
	}
	keystream_key.clear();
}

TEST_CASE("[FileAccessEncrypted] Empty payload round-trips") {
	const Vector<uint8_t> key = make_key(0x12);
	const Vector<uint8_t> payload;
	const String path = write_encrypted(payload, key);

	Vector<uint8_t> got;
	CHECK(read_all_keystream(path, key, got));
	CHECK(got.size() == 0);
	CHECK(read_all_keystream(path, key, got, true));
	keystream_key.clear();
}

TEST_CASE("[FileAccessEncrypted] Chunk tags authenticate partial reads and stay independent") {
	// Three full 64 KiB chunks plus a short fourth one.
	const Vector<uint8_t> key = make_key(0x30);
	const Vector<uint8_t> payload = make_bytes(int(3 * CHUNK_BYTES + 8 * 1024), 0x40);
	const Vector<uint8_t> pristine = payload;
	const String path = write_encrypted(payload, key);
	const uint64_t chunks = 4;
	keystream_key = key;

	// Flip one ciphertext byte in the second chunk.
	const uint64_t damaged = v2_ciphertext_offset(chunks) + CHUNK_BYTES + 100;
	xor_byte(path, damaged, 0xFF);

	// A read that only covers the third chunk is verified against its own tag and
	// succeeds: a damaged chunk does not poison the rest of the file.
	{
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);
		uint8_t chunk[4096];
		reader->seek(2 * CHUNK_BYTES);
		REQUIRE(reader->get_buffer(chunk, sizeof(chunk)) == sizeof(chunk));
		REQUIRE(memcmp(chunk, pristine.ptr() + 2 * CHUNK_BYTES, sizeof(chunk)) == 0);
		REQUIRE(reader->get_error() == OK);
	}

	// A *partial* read inside the damaged chunk still fails: the whole chunk is
	// decoded and authenticated before any of its bytes are handed back, so a seek
	// pattern cannot dodge the check.
	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		REQUIRE(reader->open_and_parse_keystream(base, keystream_block, true) == OK);
		uint8_t small[16];
		reader->seek(CHUNK_BYTES + 100);
		REQUIRE(reader->get_buffer(small, sizeof(small)) == 0);
		REQUIRE(reader->get_error() == ERR_FILE_CORRUPT);
		ERR_PRINT_ON;
	}

	// The whole file fails, and the eager (pack directory) form fails at open.
	{
		ERR_PRINT_OFF;
		Vector<uint8_t> got;
		CHECK_FALSE(read_all_keystream(path, key, got));
		CHECK_FALSE(read_all_keystream(path, key, got, true));
		ERR_PRINT_ON;
	}

	// Restoring the byte makes it verify again.
	xor_byte(path, damaged, 0xFF);
	Vector<uint8_t> got;
	CHECK(read_all_keystream(path, key, got));
	CHECK(got == pristine);
	keystream_key.clear();
}

TEST_CASE("[FileAccessEncrypted] Tampered tags and geometry are refused") {
	const Vector<uint8_t> key = make_key(0x50);
	const Vector<uint8_t> payload = make_bytes(int(CHUNK_BYTES + 1000), 0x60);
	const uint64_t chunks = 2;
	const String path = write_encrypted(payload, key);

	Vector<uint8_t> tags;
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
		REQUIRE(file.is_valid());
		file->seek(V2_TAGS_OFFSET);
		tags.resize(chunks * 16);
		file->get_buffer(tags.ptrw(), tags.size());
	}

	// A flipped tag byte.
	{
		Vector<uint8_t> bad = tags;
		bad.write[0] ^= 0x01;
		patch_bytes(path, V2_TAGS_OFFSET, bad.ptr(), 16);
		ERR_PRINT_OFF;
		Vector<uint8_t> got;
		CHECK_FALSE(read_all_keystream(path, key, got));
		ERR_PRINT_ON;
	}

	// Swapping the two tags must fail too: each tag is bound to its chunk index.
	{
		uint8_t swapped[32];
		memcpy(swapped, tags.ptr() + 16, 16);
		memcpy(swapped + 16, tags.ptr(), 16);
		patch_bytes(path, V2_TAGS_OFFSET, swapped, 32);
		ERR_PRINT_OFF;
		Vector<uint8_t> got;
		CHECK_FALSE(read_all_keystream(path, key, got));
		ERR_PRINT_ON;
	}
	patch_bytes(path, V2_TAGS_OFFSET, tags.ptr(), tags.size());

	// The plaintext length is part of every tag, so changing it by one byte is
	// caught even though the chunk geometry stays the same.
	patch_offset(path, V2_LENGTH_OFFSET, payload.size() + 1);
	{
		ERR_PRINT_OFF;
		Vector<uint8_t> got;
		CHECK_FALSE(read_all_keystream(path, key, got));
		ERR_PRINT_ON;
	}
	patch_offset(path, V2_LENGTH_OFFSET, payload.size());

	// A chunk count that does not match the length is refused at open.
	patch_offset(path, V2_CHUNK_COUNT_OFFSET, chunks + 1);
	{
		ERR_PRINT_OFF;
		Vector<uint8_t> got;
		CHECK_FALSE(read_all_keystream(path, key, got));
		ERR_PRINT_ON;
	}
	patch_offset(path, V2_CHUNK_COUNT_OFFSET, chunks);

	// A truncated payload is refused at open.
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ_WRITE);
		REQUIRE(file.is_valid());
		const uint64_t size = file->get_length();
		REQUIRE(file->resize(int64_t(size - 16)) == OK);
		file->close();
	}
	{
		ERR_PRINT_OFF;
		Vector<uint8_t> got;
		CHECK_FALSE(read_all_keystream(path, key, got));
		ERR_PRINT_ON;
	}
	keystream_key.clear();
}

TEST_CASE("[FileAccessEncrypted] Files without the magic use the same tags") {
	const Vector<uint8_t> key = make_key(0x52);
	const Vector<uint8_t> payload = make_bytes(int(CHUNK_BYTES + 3), 0x62);
	const String path = write_encrypted(payload, key, false);

	Vector<uint8_t> got;
	CHECK(read_all_keystream(path, key, got, false, false));
	CHECK(got == payload);
	keystream_key.clear();
}

TEST_CASE("[FileAccessEncrypted] MD5-era files from older builds are refused") {
	// The MD5 header form is gone. A pack from an older build has to be exported
	// again; it must fail loudly instead of being read as if it were current.
	const Vector<uint8_t> key = make_key(0x70);
	const Vector<uint8_t> payload = make_bytes(1000, 0x80);
	const String path = write_v1(payload, key);

	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		CHECK(reader->open_and_parse(base, key, FileAccessEncrypted::MODE_READ, true) == ERR_FILE_UNRECOGNIZED);
		ERR_PRINT_ON;
	}
	{
		ERR_PRINT_OFF;
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();
		// Without the magic the header is recognised by its signature, so an old
		// file reads as corrupt rather than as unrecognised.
		CHECK(reader->open_and_parse(base, key, FileAccessEncrypted::MODE_READ, false) == ERR_FILE_CORRUPT);
		ERR_PRINT_ON;
	}
	keystream_key.clear();
}

TEST_CASE("[FileAccessEncrypted] Large keystream files decode and authenticate in parallel windows") {
	// Above the parallel threshold (1 MiB) and not a multiple of the block size, so
	// the head, the middle chunk windows and the trailing bytes are all exercised.
	const Vector<uint8_t> key = make_key(0x50);
	const Vector<uint8_t> payload = make_bytes(3 * 1024 * 1024 + 7, 0x60);
	const String path = write_encrypted(payload, key);
	keystream_key = key;

	// One read of the whole file: aligned head, parallel middle, 7-byte tail.
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

	// A read that starts and ends inside chunks: the head and tail go through the
	// single-chunk cache, the middle through the worker pool.
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

	// Sequentially in odd chunks: the data must be identical and every chunk
	// authenticates.
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
			if (read == 0) {
				// A failed read must fail the case, not spin on the same offset.
				break;
			}
			done += read;
		}
		REQUIRE(got == payload);
		REQUIRE(reader->get_error() != ERR_FILE_CORRUPT);
	}

	// A wrong provider must be caught (inside the parallel decode too).
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

	// A damaged chunk in the middle of a parallel read fails the read.
	const uint64_t chunks = (payload.size() + CHUNK_BYTES - 1) / CHUNK_BYTES;
	xor_byte(path, v2_ciphertext_offset(chunks) + 10 * CHUNK_BYTES + 7, 0x01);
	keystream_key = key;
	{
		ERR_PRINT_OFF;
		Vector<uint8_t> got;
		CHECK_FALSE(read_all_keystream(path, key, got));
		ERR_PRINT_ON;
	}
	keystream_key.clear();
}

} // namespace TestFileAccessEncrypted
