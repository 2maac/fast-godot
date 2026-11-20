/**************************************************************************/
/*  secure_memory.cpp                                                     */
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

#include "core/os/secure_memory.h"

#include <cstring>

#ifdef WINDOWS_ENABLED
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(UNIX_ENABLED) && !defined(WEB_ENABLED)
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(LINUXBSD_ENABLED) || defined(ANDROID_ENABLED)
#include <sys/prctl.h>
#endif
#endif

void SecureMemory::zero(void *p_ptr, size_t p_size) {
	if (p_ptr == nullptr || p_size == 0) {
		return;
	}
#if defined(WINDOWS_ENABLED)
	// Documented as not removable by the optimizer.
	SecureZeroMemory(p_ptr, p_size);
#else
	// A plain memset() on a buffer that is about to be freed can be dropped as a
	// dead store. The empty asm with a memory clobber makes the store observable,
	// so the compiler has to keep it, while the memset itself stays vectorized
	// (the decrypted payload can be megabytes).
	memset(p_ptr, 0, p_size);
	__asm__ __volatile__("" : : "r"(p_ptr) : "memory");
#endif
}

void SecureMemory::lock(void *p_ptr, size_t p_size) {
	if (p_ptr == nullptr || p_size == 0) {
		return;
	}
#if defined(WINDOWS_ENABLED)
	VirtualLock(p_ptr, p_size);
#elif defined(UNIX_ENABLED) && !defined(WEB_ENABLED)
	mlock(p_ptr, p_size);
#endif
}

void SecureMemory::unlock(void *p_ptr, size_t p_size) {
	if (p_ptr == nullptr || p_size == 0) {
		return;
	}
#if defined(WINDOWS_ENABLED)
	VirtualUnlock(p_ptr, p_size);
#elif defined(UNIX_ENABLED) && !defined(WEB_ENABLED)
	munlock(p_ptr, p_size);
#endif
}

void SecureMemory::exclude_from_dumps(void *p_ptr, size_t p_size) {
	if (p_ptr == nullptr || p_size == 0) {
		return;
	}
#if defined(WINDOWS_ENABLED)
	// WerRegisterExcludedMemoryBlock (Windows 10 1607+) keeps a range out of WER
	// crash dumps. Resolved at runtime so that older systems and toolchains
	// without the declaration still build and simply skip it.
	typedef HRESULT(WINAPI * WerRegisterExcludedMemoryBlockFn)(const void *, DWORD);
	static WerRegisterExcludedMemoryBlockFn register_block = []() -> WerRegisterExcludedMemoryBlockFn {
		HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		if (kernel == nullptr) {
			return nullptr;
		}
		return reinterpret_cast<WerRegisterExcludedMemoryBlockFn>(
				reinterpret_cast<void *>(GetProcAddress(kernel, "WerRegisterExcludedMemoryBlock")));
	}();
	if (register_block != nullptr) {
		register_block(p_ptr, static_cast<DWORD>(p_size));
	}
#elif defined(LINUXBSD_ENABLED) || defined(ANDROID_ENABLED)
#ifdef MADV_DONTDUMP
	madvise(p_ptr, p_size, MADV_DONTDUMP);
#endif
#else
	// macOS/iOS crash reports do not include heap contents; core dumps are
	// disabled process-wide by harden_process() instead.
#endif
}

void SecureMemory::harden_process() {
#if defined(WINDOWS_ENABLED)
	// Core dumps are opt-in on Windows (WER configuration); nothing to disable.
#elif defined(UNIX_ENABLED) && !defined(WEB_ENABLED)
	struct rlimit no_core;
	no_core.rlim_cur = 0;
	no_core.rlim_max = 0;
	setrlimit(RLIMIT_CORE, &no_core);

#if defined(LINUXBSD_ENABLED) || defined(ANDROID_ENABLED)
	// Drops the process from being dumpable: unprivileged processes can no longer
	// read /proc/<pid>/mem, attach with ptrace, or request a core dump (the kernel
	// also refuses to write one). Self-inspection keeps working.
	prctl(PR_SET_DUMPABLE, 0);
#endif
#endif
}
