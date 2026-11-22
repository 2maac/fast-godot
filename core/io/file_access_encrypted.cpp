/**************************************************************************/
/*  file_access_encrypted.cpp                                             */
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

#include "file_access_encrypted.h"

#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/secure_memory.h"
#include "core/variant/variant.h"

namespace {

// Keeps the key out of swap and crash dumps while it is live, and wipes the key
// and the decrypted payload on every exit path of `_close()` — including the
// early returns — so a failed open cannot leave secrets behind either.
struct SecretWiper {
	Vector<uint8_t> &key;
	Vector<uint8_t> &data;

	~SecretWiper() {
		if (!key.is_empty()) {
			SecureMemory::unlock(key.ptrw(), key.size());
			SecureMemory::zero(key.ptrw(), key.size());
			key.clear();
		}
		if (!data.is_empty()) {
			SecureMemory::zero(data.ptrw(), data.size());
		}
	}
};

void protect_key(Vector<uint8_t> &p_key) {
	SecureMemory::lock(p_key.ptrw(), p_key.size());
	SecureMemory::exclude_from_dumps(p_key.ptrw(), p_key.size());
}

} // namespace

namespace {

// V2 format identifier, written where V1 kept the plaintext MD5. A reader that
// sees exactly these 16 bytes has an authenticated file; anything else is not one
// (an MD5 header from an older build no longer loads).
constexpr uint8_t ENCRYPTED_SIGNATURE_V2[16] = { 'G', 'D', 'E', '2', 'G', 'M', 'A', 'C', 0, 0, 0, 0, 0, 0, 0, 1 };

void store_le64(uint8_t *p_dst, uint64_t p_value) {
	for (int i = 0; i < 8; i++) {
		p_dst[i] = uint8_t(p_value >> (8 * i));
	}
}

// The GMAC table for the current hash key is 64 KiB, which is too much to rebuild
// for every opened file (7-8 us, and a pack can open many files). The hash key is
// E(K, 0), the same for every file of a pack, so each thread keeps the table for
// the last hash key it saw. Workers verify chunks on their own thread, so they
// each build it once and then only compare the 16-byte key.
struct GHashTableCache {
	uint8_t key[16] = {};
	bool valid = false;
	GHASHKey table;
};
thread_local GHashTableCache ghash_table_cache;

const GHASHKey *ghash_table_for(const uint8_t p_hash_key[16]) {
	if (!ghash_table_cache.valid || memcmp(ghash_table_cache.key, p_hash_key, 16) != 0) {
		ghash_table_cache.table.init(p_hash_key);
		memcpy(ghash_table_cache.key, p_hash_key, 16);
		ghash_table_cache.valid = true;
	}
	return &ghash_table_cache.table;
}

// GMAC of one ciphertext chunk, bound to its position and to the file's shape so
// chunks cannot be reordered, dropped or spliced in from another file:
//   tag = GHASH_H(index || chunk_count || plaintext_length || C) XOR E(K, IV XOR (index + 1))
// The tag covers ciphertext only, so it can be checked without decrypting, and
// each chunk has its own mask (the GCM nonce), so tags of different chunks of the
// same file leak nothing about each other or about H.
bool compute_chunk_tag(FileAccessEncrypted::BlockEncryptFunc p_provider, CryptoCore::AESContext *p_aes,
		const uint8_t p_hash_key[16], const uint8_t p_iv[16], uint64_t p_chunk_count,
		uint64_t p_plaintext_length, uint64_t p_index, const uint8_t *p_ct, uint64_t p_ct_len,
		uint8_t r_tag[16]) {
	uint8_t nonce[16];
	memcpy(nonce, p_iv, 16);
	const uint64_t counter = p_index + 1;
	for (int i = 0; i < 8; i++) {
		nonce[8 + i] ^= uint8_t(counter >> (8 * i));
	}
	uint8_t mask[16];
	const bool mask_ok = p_provider != nullptr ? p_provider(nonce, mask) : (p_aes->encrypt_ecb(nonce, mask) == OK);
	SecureMemory::zero(nonce, sizeof(nonce));
	if (!mask_ok) {
		SecureMemory::zero(mask, sizeof(mask));
		return false;
	}

	GHASHState state;
	state.start(*ghash_table_for(p_hash_key));
	uint8_t bind[48] = {};
	store_le64(bind, p_index);
	store_le64(bind + 16, p_chunk_count);
	store_le64(bind + 32, p_plaintext_length);
	state.update(bind, sizeof(bind));
	state.update(p_ct, p_ct_len);
	state.finish(r_tag);

	for (int i = 0; i < 16; i++) {
		r_tag[i] ^= mask[i];
	}
	SecureMemory::zero(mask, sizeof(mask));
	return true;
}

bool tags_equal(const uint8_t *p_a, const uint8_t *p_b) {
	uint8_t diff = 0;
	for (int i = 0; i < 16; i++) {
		diff |= p_a[i] ^ p_b[i];
	}
	return diff == 0;
}

// One worker-side unit of the parallel streaming decode. CFB decryption of a
// block needs only the previous *ciphertext* block, which is already in the file,
// so independent windows can be decoded concurrently: the calling thread does the
// file I/O, the tasks run exactly the same white-box lookups. Nothing about the
// tables, the amount of work per byte or the order of the keystream changes.
struct StreamDecodeChunk {
	FileAccessEncrypted::BlockEncryptFunc block_encrypt = nullptr;
	const uint8_t *ciphertext = nullptr; // Blocks [ct_base .. first_block + block_count).
	const uint8_t *iv = nullptr; // Previous block for block 0 (the file IV).
	uint8_t *plaintext = nullptr;
	uint64_t ct_base = 0;
	uint64_t first_block = 0;
	uint64_t block_count = 0;
	bool ok = true;

	// The chunks this task decodes are authenticated in the same task.
	const uint8_t *tags = nullptr;
	const uint8_t *hash_key = nullptr;
	uint64_t first_chunk = 0;
	uint64_t task_chunks = 0;
	uint64_t total_chunks = 0;
	uint64_t plaintext_length = 0;
	uint64_t chunk_blocks = 0;
	bool verify_ok = true;
};

void _decode_stream_chunk(void *p_userdata) {
	StreamDecodeChunk *chunk = static_cast<StreamDecodeChunk *>(p_userdata);
	uint8_t keystream[16];
	for (uint64_t j = 0; j < chunk->block_count; j++) {
		const uint64_t block = chunk->first_block + j;
		const uint8_t *prev = block == 0 ? chunk->iv : chunk->ciphertext + (block - 1 - chunk->ct_base) * 16;
		const uint8_t *ct = chunk->ciphertext + (block - chunk->ct_base) * 16;
		uint8_t *pt = chunk->plaintext + j * 16;
		if (!chunk->block_encrypt(prev, keystream)) {
			SecureMemory::zero(keystream, sizeof(keystream));
			chunk->ok = false;
			return;
		}
		for (int i = 0; i < 16; i++) {
			pt[i] = ct[i] ^ keystream[i];
		}
	}
	SecureMemory::zero(keystream, sizeof(keystream));

	// The ciphertext of every chunk this task decoded is in the slab it read, so
	// authentication costs no extra I/O and runs at the same time as the decoding
	// of the other workers.
	for (uint64_t c = 0; c < chunk->task_chunks; c++) {
		const uint64_t index = chunk->first_chunk + c;
		const uint64_t first = index * chunk->chunk_blocks;
		const uint8_t *ct = chunk->ciphertext + (first - chunk->ct_base) * 16;
		uint8_t tag[16];
		if (!compute_chunk_tag(chunk->block_encrypt, nullptr, chunk->hash_key, chunk->iv,
					chunk->total_chunks, chunk->plaintext_length, index, ct,
					chunk->chunk_blocks * 16, tag)) {
			ERR_PRINT(vformat("Grit: the pack's block-encryption tables refused the tag of chunk %d (a debugger may be attached).", int64_t(index)));
			chunk->verify_ok = false;
			return;
		}
		if (!tags_equal(tag, chunk->tags + index * 16)) {
			ERR_PRINT(vformat("Grit: authentication failed for encrypted chunk %d of %d; the file is corrupt or the template does not match the pack.", int64_t(index), int64_t(chunk->total_chunks)));
			chunk->verify_ok = false;
			return;
		}
	}
}

// Below this the pool hand-off costs more than the decode itself, so small reads
// (the common case: configs, scripts) stay on the calling thread.
constexpr uint64_t STREAM_PARALLEL_MIN_BYTES = 1024 * 1024;
constexpr uint64_t STREAM_PARALLEL_CHUNK_BLOCKS = 16 * 1024; // 256 KiB of blocks.
// Eight decoders where the pool has them. With the per-chunk tags verified inside
// the decode windows the read is purely CPU bound, so more workers keep paying:
// measured on an M5 with an exported encrypted game, a 256 MiB asset took
// 1.93 / 1.00 / 0.50 / 0.45 / 0.40 / 0.40 s at 1 / 2 / 4 / 6 / 8 / 10 threads and
// a 1 GiB one 7.66 / 1.99 / 1.78 / 1.59 / 1.60 s at 1 / 4 / 6 / 8 / 10 (the
// efficiency cores stop helping at eight). A game can cap it with
// GRIT_DECODE_THREADS (1..8).
constexpr int STREAM_PARALLEL_DEFAULT_TASKS = 8;
constexpr int STREAM_PARALLEL_MAX_TASKS = 8;

// Decode workers for a large read (the V2 chunk decoder). The pool's size decides
// the default; the running game can cap it with GRIT_DECODE_THREADS (1 decodes on
// the calling thread), which is also how the default was tuned.
int stream_parallel_task_limit() {
	static int cached = -1;
	if (cached >= 0) {
		return cached;
	}
	const int pool = WorkerThreadPool::get_singleton() ? WorkerThreadPool::get_singleton()->get_thread_count() : 1;
	int limit = MIN(pool, STREAM_PARALLEL_DEFAULT_TASKS);
	const OS *os = OS::get_singleton();
	if (os != nullptr) {
		const String requested = os->get_environment("GRIT_DECODE_THREADS").strip_edges();
		if (!requested.is_empty() && requested.is_valid_int()) {
			limit = CLAMP(requested.to_int(), 1, STREAM_PARALLEL_MAX_TASKS);
		}
	}
	cached = MAX(limit, 1);
	return cached;
}

} // namespace

CryptoCore::RandomGenerator *FileAccessEncrypted::_fae_static_rng = nullptr;

void FileAccessEncrypted::deinitialize() {
	if (_fae_static_rng) {
		memdelete(_fae_static_rng);
		_fae_static_rng = nullptr;
	}
}

Error FileAccessEncrypted::open_and_parse(Ref<FileAccess> p_base, const Vector<uint8_t> &p_key, Mode p_mode, bool p_with_magic, const Vector<uint8_t> &p_iv) {
	ERR_FAIL_COND_V_MSG(file.is_valid(), ERR_ALREADY_IN_USE, vformat("Can't open file while another file from path '%s' is open.", file->get_path_absolute()));
	ERR_FAIL_COND_V(p_key.size() != 32, ERR_INVALID_PARAMETER);

	pos = 0;
	eofed = false;
	use_magic = p_with_magic;
	verify_failed = false;

	if (p_mode == MODE_WRITE_AES256) {
		data.clear();
		writing = true;
		streaming = false;
		block_encrypt = nullptr;
		file = p_base;
		key = p_key;
		protect_key(key);
		if (p_iv.is_empty()) {
			iv.resize(16);
			if (unlikely(!_fae_static_rng)) {
				_fae_static_rng = memnew(CryptoCore::RandomGenerator);
				if (_fae_static_rng->init() != OK) {
					memdelete(_fae_static_rng);
					_fae_static_rng = nullptr;
					ERR_FAIL_V_MSG(FAILED, "Failed to initialize random number generator.");
				}
			}
			Error err = _fae_static_rng->get_random_bytes(iv.ptrw(), 16);
			ERR_FAIL_COND_V(err != OK, err);
		} else {
			ERR_FAIL_COND_V(p_iv.size() != 16, ERR_INVALID_PARAMETER);
			iv = p_iv;
		}

	} else if (p_mode == MODE_READ) {
		writing = false;
		streaming = false;
		block_encrypt = nullptr;
		key = p_key;
		protect_key(key);

		return _read_encrypted(p_base, p_with_magic, false);
	}

	return OK;
}

Error FileAccessEncrypted::open_and_parse_keystream(Ref<FileAccess> p_base, BlockEncryptFunc p_block_encrypt, bool p_with_magic, bool p_verify_at_open) {
	ERR_FAIL_COND_V_MSG(file.is_valid(), ERR_ALREADY_IN_USE, vformat("Can't open file while another file from path '%s' is open.", file->get_path_absolute()));
	ERR_FAIL_COND_V(p_block_encrypt == nullptr, ERR_INVALID_PARAMETER);

	pos = 0;
	eofed = false;
	writing = false;
	block_encrypt = p_block_encrypt;
	// No key at all in this mode: the keystream comes from the provider.
	key.clear();
	verify_failed = false;

	return _read_encrypted(p_base, p_with_magic, p_verify_at_open);
}

Error FileAccessEncrypted::_read_encrypted(Ref<FileAccess> p_base, bool p_with_magic, bool p_verify_at_open) {
	use_magic = p_with_magic;

	if (use_magic) {
		const uint32_t magic = p_base->get_32();
		ERR_FAIL_COND_V_MSG(magic != ENCRYPTED_HEADER_MAGIC, ERR_FILE_UNRECOGNIZED, "The encrypted file is not in the authenticated format; a pack from an older build has to be exported again.");
	}

	uint8_t signature[16];
	p_base->get_buffer(signature, sizeof(signature));
	ERR_FAIL_COND_V_MSG(memcmp(signature, ENCRYPTED_SIGNATURE_V2, sizeof(ENCRYPTED_SIGNATURE_V2)) != 0, ERR_FILE_CORRUPT, "The encrypted file has no authentication header: it is corrupt or was written by an older build.");

	length = p_base->get_64();
	chunk_count = p_base->get_64();
	const uint32_t chunk_blocks = p_base->get_32();
	p_base->get_32(); // Reserved.
	iv.resize(16);
	p_base->get_buffer(iv.ptrw(), 16);

	ERR_FAIL_COND_V(chunk_blocks != ENCRYPTED_CHUNK_BLOCKS, ERR_FILE_CORRUPT);
	// A corrupt header must not make the reader allocate or read unbounded amounts:
	// the ciphertext is at least as long as the plaintext, and the chunk table has
	// a fixed size for a given length.
	ERR_FAIL_COND_V(length > (uint64_t(1) << 55), ERR_FILE_CORRUPT);
	const uint64_t ct_length = length + ((16 - (length % 16)) % 16);
	ERR_FAIL_COND_V(ct_length > p_base->get_length(), ERR_FILE_CORRUPT);
	const uint64_t expected_chunks = (ct_length / 16 + ENCRYPTED_CHUNK_BLOCKS - 1) / ENCRYPTED_CHUNK_BLOCKS;
	ERR_FAIL_COND_V(chunk_count != expected_chunks, ERR_FILE_CORRUPT);
	// The tag table is a Vector, so keep the allocation representable.
	ERR_FAIL_COND_V(chunk_count > uint64_t(INT32_MAX) / 16, ERR_FILE_CORRUPT);

	chunk_bytes = uint64_t(ENCRYPTED_CHUNK_BLOCKS) * 16;
	chunk_tags.resize_initialized(chunk_count * 16);
	if (chunk_count != 0) {
		ERR_FAIL_COND_V(p_base->get_buffer(chunk_tags.ptrw(), chunk_tags.size()) != uint64_t(chunk_tags.size()), ERR_FILE_CORRUPT);
	}
	base = p_base->get_position();
	ERR_FAIL_COND_V(p_base->get_length() < base + ct_length, ERR_FILE_CORRUPT);

	if (block_encrypt != nullptr) {
		// Keystream build: the hash key comes from the same tables as the keystream,
		// so no key is needed for the authentication either.
		streaming = true;
		file = p_base;
		uint8_t zero[16] = {};
		if (!block_encrypt(zero, hash_key)) {
			file.unref();
			streaming = false;
			ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "The pack's block-encryption tables refused to run (a debugger may be attached), so the pack cannot be authenticated.");
		}
		// The pack directory verifies eagerly: a template/pack mismatch has to stop
		// the pack from loading. Other files authenticate as they are read.
		if (p_verify_at_open && !_verify_all_chunks()) {
			file.unref();
			streaming = false;
			ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "An authentication tag of the encrypted pack directory does not match: the file is corrupt or the template does not match the pack.");
		}
		return OK;
	}

	// Editor / key build: the key is here, so verify over the buffered ciphertext
	// (before decrypting, like encrypt-then-MAC) and decrypt after.
	CryptoCore::AESContext ctx;
	ctx.set_encode_key(key.ptrw(), 256);
	uint8_t zero[16] = {};
	ERR_FAIL_COND_V(ctx.encrypt_ecb(zero, hash_key) != OK, ERR_BUG);
	const uint64_t ds = ct_length;
	data.resize(ds);
	ERR_FAIL_COND_V(p_base->get_buffer(data.ptrw(), ds) != ds, ERR_FILE_CORRUPT);
	if (!_verify_all_chunks_key(ctx)) {
		SecureMemory::zero(data.ptrw(), data.size());
		data.clear();
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "An authentication tag of the encrypted file does not match: the file is corrupt or the decryption key is wrong.");
	}
	uint8_t cfb_iv[16];
	memcpy(cfb_iv, iv.ptr(), 16);
	ctx.decrypt_cfb(ds, cfb_iv, data.ptrw(), data.ptrw());
	SecureMemory::zero(cfb_iv, sizeof(cfb_iv));
	data.resize(length);
	SecureMemory::exclude_from_dumps(data.ptrw(), data.size());
	file = p_base;
	return OK;
}

uint64_t FileAccessEncrypted::_chunk_plain_bytes(uint64_t p_index) const {
	const uint64_t start = p_index * chunk_bytes;
	if (start >= length) {
		return 0;
	}
	return MIN(chunk_bytes, length - start);
}

bool FileAccessEncrypted::_verify_chunk(const uint8_t *p_ct, uint64_t p_ct_len, uint64_t p_index) const {
	uint8_t tag[16];
	if (!compute_chunk_tag(block_encrypt, nullptr, hash_key, iv.ptr(), chunk_count, length, p_index, p_ct, p_ct_len, tag)) {
		ERR_PRINT(vformat("Grit: the pack's block-encryption tables refused the tag of chunk %d (a debugger may be attached).", int64_t(p_index)));
		verify_failed = true;
		return false;
	}
	if (!tags_equal(tag, chunk_tags.ptr() + p_index * 16)) {
		ERR_PRINT(vformat("Grit: authentication failed for encrypted chunk %d of %d; the file is corrupt or the template does not match the pack.", int64_t(p_index), int64_t(chunk_count)));
		verify_failed = true;
		return false;
	}
	return true;
}

bool FileAccessEncrypted::_verify_all_chunks() const {
	const uint64_t total_blocks = (length + 15) / 16;
	if (chunk_count == 0) {
		return true;
	}
	Vector<uint8_t> ct;
	ct.resize(chunk_bytes);
	uint64_t block = 0;
	for (uint64_t i = 0; i < chunk_count; i++) {
		const uint64_t blocks = MIN<uint64_t>(ENCRYPTED_CHUNK_BLOCKS, total_blocks - block);
		file->seek(base + block * 16);
		if (file->get_buffer(ct.ptrw(), blocks * 16) != blocks * 16) {
			ERR_PRINT(vformat("Grit: encrypted read failed while verifying chunk %d.", int64_t(i)));
			return false;
		}
		if (!_verify_chunk(ct.ptr(), blocks * 16, i)) {
			return false;
		}
		block += blocks;
	}
	SecureMemory::zero(ct.ptrw(), ct.size());
	return true;
}

bool FileAccessEncrypted::_verify_all_chunks_key(CryptoCore::AESContext &p_ctx) const {
	const uint64_t total_blocks = (length + 15) / 16;
	uint64_t block = 0;
	for (uint64_t i = 0; i < chunk_count; i++) {
		const uint64_t blocks = MIN<uint64_t>(ENCRYPTED_CHUNK_BLOCKS, total_blocks - block);
		uint8_t tag[16];
		if (!compute_chunk_tag(nullptr, &p_ctx, hash_key, iv.ptr(), chunk_count, length, i, data.ptr() + block * 16, blocks * 16, tag)) {
			return false;
		}
		if (!tags_equal(tag, chunk_tags.ptr() + i * 16)) {
			return false;
		}
		block += blocks;
	}
	return true;
}

bool FileAccessEncrypted::_decode_chunk_cached(uint64_t p_index, const uint8_t **r_plain) const {
	if (auth_plain_index == int64_t(p_index)) {
		*r_plain = auth_plain.ptr();
		return true;
	}

	const uint64_t plain_len = _chunk_plain_bytes(p_index);
	const uint64_t ct_blocks = (plain_len + 15) / 16;
	const uint64_t first_block = p_index * ENCRYPTED_CHUNK_BLOCKS;
	// CFB needs the previous ciphertext block, so the chunk is read with one block
	// of feedback in front (except the very first chunk, which starts at the IV).
	const uint64_t ct_first = first_block == 0 ? 0 : first_block - 1;
	const uint64_t ct_blocks_read = ct_blocks + (first_block == 0 ? 0 : 1);

	auth_ct.resize(ct_blocks_read * 16);
	file->seek(base + ct_first * 16);
	if (file->get_buffer(auth_ct.ptrw(), ct_blocks_read * 16) != ct_blocks_read * 16) {
		ERR_PRINT(vformat("Grit: encrypted read failed in chunk %d.", int64_t(p_index)));
		return false;
	}

	const uint8_t *chunk_ct = auth_ct.ptr() + (first_block - ct_first) * 16;
	if (!_verify_chunk(chunk_ct, ct_blocks * 16, p_index)) {
		return false;
	}

	// The decode works in whole AES blocks; the last chunk of a file whose length
	// is not a multiple of 16 has one padded block beyond `plain_len`, so the
	// scratch buffer is sized for the blocks and the extra bytes are never exposed
	// (callers copy at most `plain_len` bytes).
	auth_plain.resize(ct_blocks * 16);
	uint8_t keystream[16];
	for (uint64_t b = 0; b < ct_blocks; b++) {
		const uint8_t *prev = b == 0
				? (first_block == 0 ? iv.ptr() : auth_ct.ptr())
				: chunk_ct + (b - 1) * 16;
		const uint8_t *ct = chunk_ct + b * 16;
		uint8_t *pt = auth_plain.ptrw() + b * 16;
		if (!block_encrypt(prev, keystream)) {
			ERR_PRINT(vformat("Grit: the keystream provider refused block %d of chunk %d (a debugger may be attached).", int64_t(b), int64_t(p_index)));
			SecureMemory::zero(keystream, sizeof(keystream));
			auth_plain_index = -1;
			return false;
		}
		for (int i = 0; i < 16; i++) {
			pt[i] = ct[i] ^ keystream[i];
		}
	}
	SecureMemory::zero(keystream, sizeof(keystream));
	auth_plain_index = int64_t(p_index);
	*r_plain = auth_plain.ptr();
	return true;
}

bool FileAccessEncrypted::_read_verified_serial(uint8_t *p_dst, uint64_t p_pos, uint64_t p_len) const {
	uint64_t done = 0;
	while (done < p_len) {
		const uint64_t chunk = (p_pos + done) / chunk_bytes;
		const uint64_t offset = (p_pos + done) % chunk_bytes;
		const uint64_t avail = _chunk_plain_bytes(chunk);
		if (avail <= offset) {
			ERR_PRINT(vformat("Grit: encrypted read past the end of chunk %d.", int64_t(chunk)));
			return false;
		}
		const uint64_t take = MIN(avail - offset, p_len - done);
		const uint8_t *plain = nullptr;
		if (!_decode_chunk_cached(chunk, &plain)) {
			return false;
		}
		memcpy(p_dst + done, plain + offset, take);
		done += take;
	}
	return true;
}

bool FileAccessEncrypted::_read_verified(uint8_t *p_dst, uint64_t p_pos, uint64_t p_len) const {
	ERR_FAIL_COND_V(p_len == 0, true);
	if (p_pos + p_len > length) {
		ERR_PRINT("Grit: encrypted read past the end of the file.");
		return false;
	}

#ifdef THREADS_ENABLED
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (pool != nullptr && p_len >= STREAM_PARALLEL_MIN_BYTES) {
		const int tasks = stream_parallel_task_limit();
		const uint64_t end = p_pos + p_len;
		// Only whole 64 KiB chunks go to the pool; the head and tail (and the
		// file's last, possibly short chunk) go through the single-chunk cache,
		// which authenticates them too.
		const uint64_t first_full = (p_pos + chunk_bytes - 1) / chunk_bytes;
		const uint64_t full_end = (end / chunk_bytes) * chunk_bytes;
		if (tasks > 1 && first_full * chunk_bytes < full_end) {
			const uint64_t head_len = first_full * chunk_bytes - p_pos;
			if (head_len != 0 && !_read_verified_serial(p_dst, p_pos, head_len)) {
				return false;
			}
			const uint64_t chunks = (full_end - first_full * chunk_bytes) / chunk_bytes;
			if (!_stream_decrypt_chunks(p_dst + head_len, first_full, chunks, tasks)) {
				return false;
			}
			const uint64_t tail_len = end - full_end;
			if (tail_len != 0 && !_read_verified_serial(p_dst + head_len + chunks * chunk_bytes, full_end, tail_len)) {
				return false;
			}
			return true;
		}
	}
#endif

	return _read_verified_serial(p_dst, p_pos, p_len);
}

bool FileAccessEncrypted::_stream_decrypt_chunks(uint8_t *p_dst, uint64_t p_first_chunk, uint64_t p_chunk_count, int p_tasks) const {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	ERR_FAIL_NULL_V(pool, false);

	const uint64_t chunks_per_task = STREAM_PARALLEL_CHUNK_BLOCKS / ENCRYPTED_CHUNK_BLOCKS;
	const uint64_t task_bytes = chunks_per_task * chunk_bytes;
	const uint64_t tasks_total = (p_chunk_count + chunks_per_task - 1) / chunks_per_task;
	const int buffers_count = int(MIN<uint64_t>(uint64_t(p_tasks), MIN<uint64_t>(tasks_total, STREAM_PARALLEL_MAX_TASKS)));

	Vector<uint8_t> ct_slab;
	ct_slab.resize(buffers_count * (task_bytes + 16));
	Vector<StreamDecodeChunk> tasks;
	tasks.resize(tasks_total);
	Vector<WorkerThreadPool::TaskID> task_ids;
	task_ids.resize(tasks_total);

	auto join_task = [&](uint64_t p_task) -> bool {
		if (pool->wait_for_task_completion(task_ids[p_task]) != OK) {
			ERR_PRINT(vformat("Grit: could not wait for the decode of chunk %d.", int64_t(tasks[p_task].first_chunk)));
			return false;
		}
		if (!tasks[p_task].ok) {
			ERR_PRINT(vformat("Grit: the keystream provider refused block %d (a debugger may be attached).", int64_t(tasks[p_task].first_block)));
			return false;
		}
		if (!tasks[p_task].verify_ok) {
			verify_failed = true;
			return false;
		}
		return true;
	};

	bool ok = true;
	uint64_t submitted = 0;
	uint64_t joined = 0;
	for (uint64_t t = 0; t < tasks_total; t++) {
		const int buf_index = int(t % uint64_t(buffers_count));
		// Reusing a slot waits for the task that owns it: that is both the
		// pipeline's flow control and the bound on the ciphertext memory. Tasks
		// are joined strictly in submission order, and a task that failed is
		// still marked joined, so the cleanup below never waits twice (the pool
		// frees the id when it is waited for).
		if (t >= uint64_t(buffers_count)) {
			const bool joined_ok = join_task(joined);
			joined++;
			if (!joined_ok) {
				ok = false;
				break;
			}
		}
		const uint64_t first_chunk = p_first_chunk + t * chunks_per_task;
		const uint64_t chunks = MIN<uint64_t>(chunks_per_task, p_chunk_count - t * chunks_per_task);
		const uint64_t first_block = first_chunk * ENCRYPTED_CHUNK_BLOCKS;
		const uint64_t block_count = chunks * ENCRYPTED_CHUNK_BLOCKS;
		const uint64_t ct_base = first_block == 0 ? 0 : first_block - 1;
		const uint64_t ct_blocks = block_count + (first_block == 0 ? 0 : 1);
		uint8_t *ct_buffer = ct_slab.ptrw() + uint64_t(buf_index) * (task_bytes + 16);
		file->seek(base + ct_base * 16);
		if (file->get_buffer(ct_buffer, ct_blocks * 16) != ct_blocks * 16) {
			ERR_PRINT(vformat("Grit: encrypted read failed at chunk %d (%d blocks wanted).", int64_t(first_chunk), int64_t(ct_blocks)));
			ok = false;
			break;
		}

		StreamDecodeChunk &task = tasks.write[t];
		task = StreamDecodeChunk();
		task.block_encrypt = block_encrypt;
		task.ciphertext = ct_buffer;
		task.iv = iv.ptr();
		task.plaintext = p_dst + t * task_bytes;
		task.ct_base = ct_base;
		task.first_block = first_block;
		task.block_count = block_count;
		task.tags = chunk_tags.ptr();
		task.hash_key = hash_key;
		task.first_chunk = first_chunk;
		task.task_chunks = chunks;
		task.total_chunks = chunk_count;
		task.plaintext_length = length;
		task.chunk_blocks = ENCRYPTED_CHUNK_BLOCKS;
		task_ids.write[t] = pool->add_native_task(&_decode_stream_chunk, &task, true, "Decode encrypted pack");
		submitted = t + 1;
	}

	// Every submitted task writes into p_dst, so they must all be joined before
	// this call returns (also on the error paths above). `joined` counts the
	// tasks that have already been waited for.
	for (uint64_t t = joined; t < submitted; t++) {
		if (!join_task(t)) {
			ok = false;
		}
	}
	return ok;
}

Error FileAccessEncrypted::open_and_parse_password(Ref<FileAccess> p_base, const String &p_key, Mode p_mode) {
	String cs = p_key.md5_text();
	ERR_FAIL_COND_V(cs.length() != 32, ERR_INVALID_PARAMETER);
	Vector<uint8_t> key_md5;
	key_md5.resize(32);
	for (int i = 0; i < 32; i++) {
		key_md5.write[i] = cs[i];
	}

	return open_and_parse(p_base, key_md5, p_mode);
}

Error FileAccessEncrypted::open_internal(const String &p_path, int p_mode_flags) {
	return OK;
}

void FileAccessEncrypted::_close() {
	// A failed open can still leave the key (and a decrypted window) behind, so the
	// wipers are armed before any early return.
	SecretWiper wipe = { key, data };

	if (file.is_null()) {
		streaming = false;
		block_encrypt = nullptr;
		return;
	}

	if (writing) {
		// V2: one GMAC tag per 64 KiB of ciphertext, written in front of the
		// payload. Tags are computed after the encryption because they cover the
		// ciphertext (verify-before-decrypt), exactly what the reader checks.
		LocalVector<uint8_t> compressed;
		const uint64_t plain_length = data.size();
		uint64_t len = plain_length;
		if (len % 16) {
			len += 16 - (len % 16);
		}

		compressed.resize(len);
		if (plain_length != 0) {
			memcpy(compressed.ptr(), data.ptr(), plain_length);
		}
		if (len > plain_length) {
			memset(compressed.ptr() + plain_length, 0, len - plain_length);
		}

		CryptoCore::AESContext ctx;
		ctx.set_encode_key(key.ptrw(), 256);

		uint8_t cfb_iv[16];
		memcpy(cfb_iv, iv.ptr(), 16);
		if (len != 0) {
			ctx.encrypt_cfb(len, cfb_iv, compressed.ptr(), compressed.ptr());
		}
		SecureMemory::zero(cfb_iv, sizeof(cfb_iv));

		uint8_t zero[16] = {};
		uint8_t hk[16];
		ERR_FAIL_COND(ctx.encrypt_ecb(zero, hk) != OK);

		const uint64_t blocks = len / 16;
		const uint64_t chunks = (blocks + ENCRYPTED_CHUNK_BLOCKS - 1) / ENCRYPTED_CHUNK_BLOCKS;
		Vector<uint8_t> tags;
		tags.resize_initialized(chunks * 16);
		for (uint64_t i = 0; i < chunks; i++) {
			const uint64_t first = i * ENCRYPTED_CHUNK_BLOCKS;
			const uint64_t count = MIN<uint64_t>(ENCRYPTED_CHUNK_BLOCKS, blocks - first);
			uint8_t tag[16];
			if (!compute_chunk_tag(nullptr, &ctx, hk, iv.ptr(), chunks, plain_length, i, compressed.ptr() + first * 16, count * 16, tag)) {
				ERR_FAIL_MSG("Could not compute the authentication tag of an encrypted file.");
			}
			memcpy(tags.ptrw() + i * 16, tag, 16);
			SecureMemory::zero(tag, sizeof(tag));
		}
		SecureMemory::zero(hk, sizeof(hk));

		if (use_magic) {
			file->store_32(ENCRYPTED_HEADER_MAGIC);
		}
		file->store_buffer(ENCRYPTED_SIGNATURE_V2, sizeof(ENCRYPTED_SIGNATURE_V2));
		file->store_64(plain_length);
		file->store_64(chunks);
		file->store_32(ENCRYPTED_CHUNK_BLOCKS);
		file->store_32(0);
		file->store_buffer(iv.ptr(), 16);
		if (chunks != 0) {
			file->store_buffer(tags.ptr(), tags.size());
		}
		file->store_buffer(compressed.ptr(), len);

		SecureMemory::zero(compressed.ptr(), compressed.size());
		SecureMemory::zero(data.ptrw(), data.size());
		data.clear();
	}

	chunk_count = 0;
	chunk_bytes = 0;
	chunk_tags.clear();
	SecureMemory::zero(hash_key, sizeof(hash_key));
	if (!auth_ct.is_empty()) {
		SecureMemory::zero(auth_ct.ptrw(), auth_ct.size());
		auth_ct.clear();
	}
	if (!auth_plain.is_empty()) {
		SecureMemory::zero(auth_plain.ptrw(), auth_plain.size());
		auth_plain.clear();
	}
	auth_plain_index = -1;
	streaming = false;
	block_encrypt = nullptr;
	file.unref();
}

bool FileAccessEncrypted::is_open() const {
	return file.is_valid();
}

String FileAccessEncrypted::get_path() const {
	if (file.is_valid()) {
		return file->get_path();
	} else {
		return "";
	}
}

String FileAccessEncrypted::get_path_absolute() const {
	if (file.is_valid()) {
		return file->get_path_absolute();
	} else {
		return "";
	}
}

void FileAccessEncrypted::seek(uint64_t p_position) {
	if (p_position > get_length()) {
		p_position = get_length();
	}

	pos = p_position;
	eofed = false;
}

void FileAccessEncrypted::seek_end(int64_t p_position) {
	seek(get_length() + p_position);
}

uint64_t FileAccessEncrypted::get_position() const {
	return pos;
}

uint64_t FileAccessEncrypted::get_length() const {
	return streaming ? length : data.size();
}

bool FileAccessEncrypted::eof_reached() const {
	return eofed;
}

uint64_t FileAccessEncrypted::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	ERR_FAIL_COND_V_MSG(writing, -1, "File has not been opened in read mode.");

	if (!p_length) {
		return 0;
	}

	ERR_FAIL_NULL_V(p_dst, -1);

	uint64_t to_copy = MIN(p_length, get_length() - pos);

	if (streaming) {
		if (verify_failed) {
			// A tag already failed: keep handing out nothing rather than garbage
			// from a later read.
			eofed = true;
			return 0;
		}
		// Every chunk is authenticated before its bytes are handed back, so a seek
		// pattern verifies too (a partial chunk is decoded and checked in full).
		if (to_copy && !_read_verified(p_dst, pos, to_copy)) {
			eofed = true;
			return 0;
		}
	} else {
		memcpy(p_dst, data.ptr() + pos, to_copy);
	}
	pos += to_copy;

	if (to_copy < p_length) {
		eofed = true;
	}

	return to_copy;
}

Error FileAccessEncrypted::get_error() const {
	if (verify_failed) {
		return ERR_FILE_CORRUPT;
	}
	return eofed ? ERR_FILE_EOF : OK;
}

bool FileAccessEncrypted::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL_COND_V_MSG(!writing, false, "File has not been opened in write mode.");

	if (!p_length) {
		return true;
	}

	ERR_FAIL_NULL_V(p_src, false);

	if (pos + p_length >= get_length()) {
		ERR_FAIL_COND_V(data.resize(pos + p_length) != OK, false);
	}

	memcpy(data.ptrw() + pos, p_src, p_length);
	pos += p_length;

	return true;
}

void FileAccessEncrypted::flush() {
	ERR_FAIL_COND_MSG(!writing, "File has not been opened in write mode.");

	// encrypted files keep data in memory till close()
}

bool FileAccessEncrypted::file_exists(const String &p_name) {
	Ref<FileAccess> fa = FileAccess::open(p_name, FileAccess::READ);
	if (fa.is_null()) {
		return false;
	}
	return true;
}

uint64_t FileAccessEncrypted::_get_modified_time(const String &p_file) {
	if (file.is_valid()) {
		return file->get_modified_time(p_file);
	} else {
		return 0;
	}
}

uint64_t FileAccessEncrypted::_get_access_time(const String &p_file) {
	if (file.is_valid()) {
		return file->get_access_time(p_file);
	} else {
		return 0;
	}
}

int64_t FileAccessEncrypted::_get_size(const String &p_file) {
	if (file.is_valid()) {
		return file->get_size(p_file);
	} else {
		return -1;
	}
}

BitField<FileAccess::UnixPermissionFlags> FileAccessEncrypted::_get_unix_permissions(const String &p_file) {
	if (file.is_valid()) {
		return file->_get_unix_permissions(p_file);
	}
	return 0;
}

Error FileAccessEncrypted::_set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions) {
	if (file.is_valid()) {
		return file->_set_unix_permissions(p_file, p_permissions);
	}
	return FAILED;
}

bool FileAccessEncrypted::_get_hidden_attribute(const String &p_file) {
	if (file.is_valid()) {
		return file->_get_hidden_attribute(p_file);
	}
	return false;
}

Error FileAccessEncrypted::_set_hidden_attribute(const String &p_file, bool p_hidden) {
	if (file.is_valid()) {
		return file->_set_hidden_attribute(p_file, p_hidden);
	}
	return FAILED;
}

bool FileAccessEncrypted::_get_read_only_attribute(const String &p_file) {
	if (file.is_valid()) {
		return file->_get_read_only_attribute(p_file);
	}
	return false;
}

Error FileAccessEncrypted::_set_read_only_attribute(const String &p_file, bool p_ro) {
	if (file.is_valid()) {
		return file->_set_read_only_attribute(p_file, p_ro);
	}
	return FAILED;
}

void FileAccessEncrypted::close() {
	_close();
}

FileAccessEncrypted::~FileAccessEncrypted() {
	_close();
}
