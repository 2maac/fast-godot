/**************************************************************************/
/*  test_fae_bench.cpp                                                    */
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

TEST_FORCE_LINK(test_fae_bench)

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/file_access_encrypted.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "tests/test_utils.h"

namespace TestFaeBench {

static Vector<uint8_t> bench_key;
// The provider stands in for the white-box tables; keep the key schedule per
// thread so the benchmark measures the reader, not a per-block AES setup.
static thread_local Vector<uint8_t> bench_cached_key;
static thread_local CryptoCore::AESContext bench_ctx;
static bool bench_block(const uint8_t p_in[16], uint8_t r_out[16]) {
	// Called from worker threads too. Read the shared global through its const
	// pointer (ptrw() may copy-on-write and is not thread safe) and keep the
	// per-thread copy that the key schedule is built from.
	if (bench_key.size() != 32) {
		ERR_FAIL_V_MSG(false, "The keystream provider has no key.");
	}
	if (bench_cached_key.size() != 32 || memcmp(bench_cached_key.ptr(), bench_key.ptr(), 32) != 0) {
		uint8_t raw[32];
		memcpy(raw, bench_key.ptr(), 32);
		ERR_FAIL_COND_V(bench_ctx.set_encode_key(raw, 256) != OK, false);
		bench_cached_key.resize(32);
		memcpy(bench_cached_key.ptrw(), raw, 32);
	}
	return bench_ctx.encrypt_ecb(p_in, r_out) == OK;
}

TEST_CASE("[FileAccessEncrypted] bench") {
	const OS *os = OS::get_singleton();
	if (os->get_environment("GRIT_FAE_BENCH") != "1") {
		return;
	}
	const uint64_t mb = os->get_environment("GRIT_FAE_BENCH_MB").is_empty() ? 256 : os->get_environment("GRIT_FAE_BENCH_MB").to_int();
	const uint64_t size = mb * 1024 * 1024;

	bench_key.resize(32);
	for (int i = 0; i < 32; i++) {
		bench_key.write[i] = uint8_t(i * 7 + 1);
	}

	const String path = TestUtils::get_temp_path("fae_bench.bin");
	{
		Vector<uint8_t> payload;
		payload.resize(size);
		for (uint64_t i = 0; i < size; i++) {
			payload.write[i] = uint8_t(i * 31 + (i >> 13));
		}
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::WRITE);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> writer;
		writer.instantiate();
		REQUIRE(writer->open_and_parse(base, bench_key, FileAccessEncrypted::MODE_WRITE_AES256, true) == OK);
		REQUIRE(writer->store_buffer(payload.ptr(), payload.size()));
		writer->close();
		base->close();
	}

	Vector<uint8_t> buffer;
	buffer.resize(size);

	for (int run = 0; run < 3; run++) {
		Ref<FileAccess> base = FileAccess::open(path, FileAccess::READ);
		REQUIRE(base.is_valid());
		Ref<FileAccessEncrypted> reader;
		reader.instantiate();

		const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
		REQUIRE(reader->open_and_parse_keystream(base, bench_block, true) == OK);
		const uint64_t t1 = Time::get_singleton()->get_ticks_usec();
		const uint64_t got = reader->get_buffer(buffer.ptrw(), buffer.size());
		const uint64_t t2 = Time::get_singleton()->get_ticks_usec();
		REQUIRE(got == size);
		REQUIRE(reader->get_error() != ERR_FILE_CORRUPT);
		reader->close();

		const double open_s = double(t1 - t0) / 1e6;
		const double read_s = double(t2 - t1) / 1e6;
		print_line(vformat("GRIT_FAE_BENCH run=%d mb=%d threads=%s open=%.3f s read=%.3f s total=%.3f s rate=%.1f MiB/s",
				run, (int64_t)mb, OS::get_singleton()->get_environment("GRIT_DECODE_THREADS"),
				open_s, read_s, open_s + read_s, double(mb) / read_s));
	}
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
		if (file.is_valid()) {
			print_line(vformat("GRIT_FAE_BENCH on-disk=%d bytes (payload %d)", (int64_t)file->get_length(), (int64_t)size));
		}
	}
	DirAccess::remove_absolute(path);
}

} // namespace TestFaeBench
