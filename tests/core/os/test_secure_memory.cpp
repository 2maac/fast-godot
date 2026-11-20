/**************************************************************************/
/*  test_secure_memory.cpp                                                */
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

TEST_FORCE_LINK(test_secure_memory)

#include "core/os/secure_memory.h"

#include <cstring>

namespace TestSecureMemory {

TEST_CASE("[SecureMemory] zero() clears the whole range") {
	uint8_t buffer[64];
	memset(buffer, 0xAB, sizeof(buffer));

	SecureMemory::zero(buffer, sizeof(buffer));

	for (size_t i = 0; i < sizeof(buffer); i++) {
		CHECK_MESSAGE(buffer[i] == 0, "Byte ", i, " should have been wiped.");
	}
}

TEST_CASE("[SecureMemory] zero() ignores empty and null ranges") {
	SecureMemory::zero(nullptr, 16);

	uint8_t buffer = 0x5A;
	SecureMemory::zero(&buffer, 0);
	CHECK_MESSAGE(buffer == 0x5A, "A zero-length wipe must not touch the buffer.");
}

TEST_CASE("[SecureMemory] lock() and exclude_from_dumps() keep the contents intact") {
	uint8_t key[32];
	memset(key, 0x3C, sizeof(key));

	SecureMemory::lock(key, sizeof(key));
	SecureMemory::exclude_from_dumps(key, sizeof(key));

	CHECK_MESSAGE(key[0] == 0x3C, "Locking must not modify the memory.");
	CHECK_MESSAGE(key[31] == 0x3C, "Excluding from dumps must not modify the memory.");

	SecureMemory::unlock(key, sizeof(key));
	CHECK_MESSAGE(key[16] == 0x3C, "Unlocking must not modify the memory.");

	SecureMemory::zero(key, sizeof(key));
}

} // namespace TestSecureMemory
