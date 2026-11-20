/**************************************************************************/
/*  secure_memory.h                                                       */
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

#include "core/typedefs.h"

// Best-effort protections for key material and the plaintext it unlocks.
//
// None of this is a guarantee: an attacker who controls the machine can read any
// page the process can address. The point is to remove the cheap wins — swap
// copies, core dumps, `/proc/<pid>/mem` scraping by the same user, and stale
// secrets left in freed allocations — and to make the remaining attack (a
// privileged memory dump or emulating the code) obvious work rather than a
// one-line script.
namespace SecureMemory {

// Zero the range in a way the optimizer cannot elide. A plain `memset()` on a
// buffer that is about to be freed or overwritten can be removed as a dead
// store; volatile writes cannot.
void zero(void *p_ptr, size_t p_size);

// Lock the range into RAM (mlock/VirtualLock) so it is never written to swap.
// Best effort: the per-process locked-memory limit is small on most systems,
// and a failure here is not fatal.
void lock(void *p_ptr, size_t p_size);
void unlock(void *p_ptr, size_t p_size);

// Keep the range out of core/crash dumps where the OS supports it
// (MADV_DONTDUMP on Linux/Android, WER memory-block exclusion on Windows).
void exclude_from_dumps(void *p_ptr, size_t p_size);

// Process-wide hardening for release builds: disable core dumps, and on Linux
// make the process non-dumpable so unprivileged processes cannot open
// `/proc/<pid>/mem`, attach a debugger, or trigger a core dump. Called once
// from `Main::setup()` for non-editor, non-dev builds only: editors and dev
// builds stay debuggable.
void harden_process();

} // namespace SecureMemory
