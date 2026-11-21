/**************************************************************************/
/*  file_access_encrypted.h                                               */
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

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"

#define ENCRYPTED_HEADER_MAGIC 0x43454447

class FileAccessEncrypted : public FileAccess {
	GDSOFTCLASS(FileAccessEncrypted, FileAccess);

public:
	// AES-256 block encryption under the pack key: E(key, p_in) -> r_out.
	typedef bool (*BlockEncryptFunc)(const uint8_t p_in[16], uint8_t r_out[16]);

	enum Mode : int32_t {
		MODE_READ,
		MODE_WRITE_AES256,
		MODE_MAX
	};

private:
	Vector<uint8_t> iv;
	Vector<uint8_t> key;
	bool writing = false;
	mutable Ref<FileAccess> file;
	uint64_t base = 0;
	uint64_t length = 0;
	Vector<uint8_t> data;
	mutable uint64_t pos = 0;
	mutable bool eofed = false;
	bool use_magic = true;

	void _close();

	static CryptoCore::RandomGenerator *_fae_static_rng;

	// A white-box build has no pack key to hand out, so CFB decoding asks for one
	// AES block encryption at a time instead (the CFB keystream). When this is set,
	// `key` stays empty and the file is not buffered: blocks are decoded on demand
	// from the base file, so only the requested window is ever in memory.
	BlockEncryptFunc block_encrypt = nullptr;
	bool streaming = false;
	mutable Vector<uint8_t> stream_ct;
	mutable Vector<uint8_t> stream_pt;

	// Integrity check for the streaming path. Decoding the whole payload up front
	// only to compare its MD5 doubles the cost of every file that is opened and
	// read once (the tables run at ~21 MiB/s), so pack files verify as they are
	// read instead: plaintext read sequentially from offset 0 is hashed on the way
	// through and compared when the file reaches its end. A random-access pattern
	// simply turns the check off rather than decoding bytes nobody asked for (stock
	// packs never check file contents at all). The pack directory opts into the
	// eager form, because a pack/template mismatch must fail the pack immediately.
	mutable CryptoCore::MD5Context verify_ctx;
	mutable uint64_t verify_next = 0;
	mutable bool verify_active = false;
	mutable bool verify_failed = false;
	unsigned char verify_digest[16] = {};
	bool lazy_verify = false;

	Error _read_encrypted(Ref<FileAccess> p_base, bool p_with_magic, bool p_verify_at_open);
	bool _verify_stream_md5(const unsigned char p_expected[16]);
	bool _stream_decrypt(uint8_t *p_dst, uint64_t p_pos, uint64_t p_len) const;
	// Large reads: independent CFB windows decoded by the thread pool (the caller
	// thread keeps doing the file I/O). Same blocks, same providers, same tables.
	bool _stream_decrypt_parallel(uint8_t *p_dst, uint64_t p_pos, uint64_t p_len, int p_tasks) const;
	bool _stream_decrypt_block(uint64_t p_block, uint8_t *r_dst) const;
	bool _lazy_verify_update(const uint8_t *p_data, uint64_t p_pos, uint64_t p_len) const;

public:
	Error open_and_parse(Ref<FileAccess> p_base, const Vector<uint8_t> &p_key, Mode p_mode, bool p_with_magic = true, const Vector<uint8_t> &p_iv = Vector<uint8_t>());
	// Read mode only: decodes CFB through the keystream provider instead of a key,
	// so the pack key itself never exists in this process.
	//
	// With `p_verify_at_open` the whole payload is decoded and its MD5 checked
	// before returning (the pack directory uses this, since it is small and a
	// mismatch there has to stop the pack from loading). Otherwise the check runs
	// during the caller's sequential reads and is compared at EOF, so opening a
	// large encrypted file no longer decodes it once up front.
	Error open_and_parse_keystream(Ref<FileAccess> p_base, BlockEncryptFunc p_block_encrypt, bool p_with_magic = true, bool p_verify_at_open = false);
	Error open_and_parse_password(Ref<FileAccess> p_base, const String &p_key, Mode p_mode);

	Vector<uint8_t> get_iv() const { return iv; }

	virtual Error open_internal(const String &p_path, int p_mode_flags) override; ///< open a file
	virtual bool is_open() const override; ///< true when file is open

	virtual String get_path() const override; /// returns the path for the current open file
	virtual String get_path_absolute() const override; /// returns the absolute path for the current open file

	virtual void seek(uint64_t p_position) override; ///< seek to a given position
	virtual void seek_end(int64_t p_position = 0) override; ///< seek from the end of file
	virtual uint64_t get_position() const override; ///< get position in the file
	virtual uint64_t get_length() const override; ///< get size of the file

	virtual bool eof_reached() const override; ///< reading passed EOF

	virtual uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override;

	virtual Error get_error() const override; ///< get last error

	virtual Error resize(int64_t p_length) override { return ERR_UNAVAILABLE; }
	virtual void flush() override;
	virtual bool store_buffer(const uint8_t *p_src, uint64_t p_length) override; ///< store an array of bytes

	virtual bool file_exists(const String &p_name) override; ///< return true if a file exists

	virtual uint64_t _get_modified_time(const String &p_file) override;
	virtual uint64_t _get_access_time(const String &p_file) override;
	virtual int64_t _get_size(const String &p_file) override;
	virtual BitField<FileAccess::UnixPermissionFlags> _get_unix_permissions(const String &p_file) override;
	virtual Error _set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions) override;

	virtual bool _get_hidden_attribute(const String &p_file) override;
	virtual Error _set_hidden_attribute(const String &p_file, bool p_hidden) override;
	virtual bool _get_read_only_attribute(const String &p_file) override;
	virtual Error _set_read_only_attribute(const String &p_file, bool p_ro) override;

	virtual void close() override;

	static void deinitialize();

	~FileAccessEncrypted();
};
