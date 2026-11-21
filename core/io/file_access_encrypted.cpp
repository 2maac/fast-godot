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
	lazy_verify = false;
	verify_active = false;
	verify_next = 0;

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
	lazy_verify = false;
	verify_active = false;
	verify_next = 0;

	return _read_encrypted(p_base, p_with_magic, p_verify_at_open);
}

Error FileAccessEncrypted::_read_encrypted(Ref<FileAccess> p_base, bool p_with_magic, bool p_verify_at_open) {
	use_magic = p_with_magic;

	if (use_magic) {
		uint32_t magic = p_base->get_32();
		ERR_FAIL_COND_V(magic != ENCRYPTED_HEADER_MAGIC, ERR_FILE_UNRECOGNIZED);
	}

	unsigned char md5d[16];
	p_base->get_buffer(md5d, 16);
	length = p_base->get_64();

	iv.resize(16);
	p_base->get_buffer(iv.ptrw(), 16);

	base = p_base->get_position();
	ERR_FAIL_COND_V(p_base->get_length() < base + length, ERR_FILE_CORRUPT);

	if (block_encrypt != nullptr) {
		// Keystream build: decode blocks on demand, never build the whole plaintext.
		// The pack directory is verified at open (it is small, and a template/pack
		// mismatch must stop the pack from loading); every other file is verified
		// while it is read, so a file that is opened and read once is decoded once
		// instead of twice.
		streaming = true;
		file = p_base;
		if (p_verify_at_open) {
			if (!_verify_stream_md5(md5d)) {
				file.unref();
				streaming = false;
				ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "The MD5 sum of the decrypted file does not match the expected value, or the decryption keystream is unavailable (a debugger may be attached). The file is corrupt or the template does not match the pack.");
			}
			return OK;
		}
		memcpy(verify_digest, md5d, sizeof(verify_digest));
		verify_next = 0;
		verify_active = verify_ctx.start() == OK;
		lazy_verify = true;
		return OK;
	}

	uint64_t ds = length;
	if (ds % 16) {
		ds += 16 - (ds % 16);
	}
	data.resize(ds);

	uint64_t blen = p_base->get_buffer(data.ptrw(), ds);
	ERR_FAIL_COND_V(blen != ds, ERR_FILE_CORRUPT);

	{
		CryptoCore::AESContext ctx;

		ctx.set_encode_key(key.ptrw(), 256); // Due to the nature of CFB, same key schedule is used for both encryption and decryption!
		ctx.decrypt_cfb(ds, iv.ptrw(), data.ptrw(), data.ptrw());
	}

	data.resize(length);
	SecureMemory::exclude_from_dumps(data.ptrw(), data.size());

	unsigned char hash[16];
	ERR_FAIL_COND_V(CryptoCore::md5(data.ptr(), data.size(), hash) != OK, ERR_BUG);

	ERR_FAIL_COND_V_MSG(String::md5(hash) != String::md5(md5d), ERR_FILE_CORRUPT, "The MD5 sum of the decrypted file does not match the expected value. It could be that the file is corrupt, or that the provided decryption key is invalid.");

	file = p_base;

	return OK;
}

bool FileAccessEncrypted::_verify_stream_md5(const unsigned char p_expected[16]) {
	// One pass over the payload: decrypt each window through the keystream provider
	// and feed it to an incremental MD5. The plaintext window is reused, so memory
	// stays bounded regardless of the file size.
	CryptoCore::MD5Context ctx;
	ERR_FAIL_COND_V(ctx.start() != OK, false);

	constexpr uint64_t chunk_size = 64 * 1024;
	Vector<uint8_t> chunk;
	chunk.resize(chunk_size);
	uint64_t done = 0;
	while (done < length) {
		const uint64_t take = MIN(chunk_size, length - done);
		if (!_stream_decrypt(chunk.ptrw(), done, take)) {
			SecureMemory::zero(chunk.ptrw(), chunk.size());
			return false;
		}
		if (ctx.update(chunk.ptr(), take) != OK) {
			SecureMemory::zero(chunk.ptrw(), chunk.size());
			return false;
		}
		done += take;
	}
	SecureMemory::zero(chunk.ptrw(), chunk.size());

	unsigned char hash[16];
	ERR_FAIL_COND_V(ctx.finish(hash) != OK, false);
	return String::md5(hash) == String::md5(p_expected);
}

bool FileAccessEncrypted::_lazy_verify_update(const uint8_t *p_data, uint64_t p_pos, uint64_t p_len) const {
	if (!verify_active) {
		return true;
	}
	if (p_pos != verify_next) {
		// The caller skipped ahead or went back, so this is not a full sequential
		// pass and the digest cannot be completed. Stock packs do not verify file
		// contents at all, so the only thing lost here is the diagnostic.
		verify_active = false;
		return true;
	}
	if (verify_ctx.update(p_data, p_len) != OK) {
		verify_active = false;
		return true;
	}
	verify_next += p_len;
	if (verify_next < length) {
		return true;
	}

	unsigned char digest[16];
	verify_active = false;
	if (verify_ctx.finish(digest) != OK) {
		return true;
	}
	if (memcmp(digest, verify_digest, sizeof(digest)) == 0) {
		return true;
	}
	verify_failed = true;
	ERR_PRINT("The MD5 sum of the decrypted file does not match the expected value, or the decryption keystream is unavailable (a debugger may be attached). The file is corrupt or the template does not match the pack.");
	return false;
}

bool FileAccessEncrypted::_stream_decrypt(uint8_t *p_dst, uint64_t p_pos, uint64_t p_len) const {
	// CFB: keystream block = E(key, previous ciphertext block); plaintext = ct XOR ks.
	// Only AES encryption is needed, which is what a white-box keystream provider
	// computes. The previous ciphertext block is readable from the base file, so a
	// read at any offset costs one block lookup, not a replay from the IV.
	ERR_FAIL_COND_V(p_len == 0, true);
	ERR_FAIL_COND_V(p_pos + p_len > length, false);

	constexpr uint64_t window_blocks = 1024; // 16 KiB of ciphertext per window.
	uint64_t done = 0;
	uint64_t offset = p_pos;
	const uint64_t end = p_pos + p_len;
	while (offset < end) {
		const uint64_t block = offset / 16;
		const uint64_t block_offset = offset % 16;
		const uint64_t window_end = MIN(end, (block + window_blocks) * 16);
		const uint64_t last_block = (window_end + 15) / 16 - 1;
		const uint64_t ct_first = block == 0 ? 0 : block - 1;
		const uint64_t window_bytes = (last_block - ct_first + 1) * 16;

		stream_ct.resize(window_bytes);
		stream_pt.resize(window_bytes);
		SecureMemory::exclude_from_dumps(stream_pt.ptrw(), window_bytes);

		file->seek(base + ct_first * 16);
		if (file->get_buffer(stream_ct.ptrw(), window_bytes) != window_bytes) {
			return false;
		}

		uint8_t keystream[16];
		for (uint64_t b = block; b <= last_block; b++) {
			const uint8_t *prev = b == 0 ? iv.ptr() : stream_ct.ptr() + (b - 1 - ct_first) * 16;
			const uint8_t *ct = stream_ct.ptr() + (b - ct_first) * 16;
			uint8_t *pt = stream_pt.ptrw() + (b - ct_first) * 16;
			if (!block_encrypt(prev, keystream)) {
				SecureMemory::zero(keystream, sizeof(keystream));
				return false;
			}
			for (int i = 0; i < 16; i++) {
				pt[i] = ct[i] ^ keystream[i];
			}
		}
		SecureMemory::zero(keystream, sizeof(keystream));

		const uint64_t copy_len = window_end - offset;
		// `stream_pt` starts at `ct_first`, which is one block before `block` for
		// every block but the first (its plaintext is there only to provide the CFB
		// feedback), so the requested bytes start one block further in.
		const uint64_t pt_offset = (block - ct_first) * 16 + block_offset;
		memcpy(p_dst + done, stream_pt.ptr() + pt_offset, copy_len);
		done += copy_len;
		offset += copy_len;
	}

	return true;
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

	if (!stream_ct.is_empty()) {
		SecureMemory::zero(stream_ct.ptrw(), stream_ct.size());
		stream_ct.clear();
	}
	if (!stream_pt.is_empty()) {
		SecureMemory::zero(stream_pt.ptrw(), stream_pt.size());
		stream_pt.clear();
	}

	if (file.is_null()) {
		streaming = false;
		block_encrypt = nullptr;
		return;
	}

	if (writing) {
		LocalVector<uint8_t> compressed;
		uint64_t len = data.size();
		if (len % 16) {
			len += 16 - (len % 16);
		}

		unsigned char hash[16];
		ERR_FAIL_COND(CryptoCore::md5(data.ptr(), data.size(), hash) != OK); // Bug?

		compressed.resize(len);
		memcpy(compressed.ptr(), data.ptr(), data.size());
		memset(compressed.ptr() + data.size(), 0, len - data.size());

		CryptoCore::AESContext ctx;
		ctx.set_encode_key(key.ptrw(), 256);

		if (use_magic) {
			file->store_32(ENCRYPTED_HEADER_MAGIC);
		}

		file->store_buffer(hash, 16);
		file->store_64(data.size());
		file->store_buffer(iv.ptr(), 16);

		ctx.encrypt_cfb(len, iv.ptrw(), compressed.ptr(), compressed.ptr());

		file->store_buffer(compressed.ptr(), compressed.size());
		SecureMemory::zero(data.ptrw(), data.size());
		data.clear();
	}

	streaming = false;
	lazy_verify = false;
	verify_active = false;
	verify_next = 0;
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
			// The integrity check already failed: keep handing out nothing rather
			// than garbage from a later read.
			eofed = true;
			return 0;
		}
		if (to_copy && !_stream_decrypt(p_dst, pos, to_copy)) {
			eofed = true;
			return 0;
		}
		if (to_copy && lazy_verify && !_lazy_verify_update(p_dst, pos, to_copy)) {
			// The digest failed with this chunk: refuse to hand it back so the caller
			// sees a short read, and get_error() reports ERR_FILE_CORRUPT.
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
