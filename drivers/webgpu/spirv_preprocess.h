/**************************************************************************/
/*  spirv_preprocess.h                                                    */
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

#include "core/templates/vector.h"

#include <cstdint>

namespace spirv_preprocess {

// Result struct for fix_depth2_images, which returns both the modified
// SPIR-V bytes and a list of (image_type_id, dim, arrayed) tuples for
// any depth=2 images that were changed.
struct DepthImageFixResult {
	Vector<uint8_t> bytes;
	struct DepthImageInfo {
		uint32_t image_type_id;
		uint32_t dim;
		uint32_t arrayed;
	};
	Vector<DepthImageInfo> depth_images;
};

// Evaluate OpSpecConstantOp instructions with default values and replace them
// with regular OpConstant instructions (WGSL has no computed overrides).
//
// When p_freeze_scalars is false (the default) plain OpSpecConstant,
// OpSpecConstantTrue and OpSpecConstantFalse instructions keep their SpecId
// decorations, so Tint emits `@id(N) override` declarations and the WebGPU
// driver can specialise pipelines with pipeline constants instead of building
// and compiling a brand-new shader module for every specialisation. Modules
// containing spec-constant composites are always fully frozen (a constant
// composite cannot reference an override).
Vector<uint8_t> freeze_spec_constant_ops(const Vector<uint8_t> &p_bytes, bool p_freeze_scalars = false);

// Convert push-constant variables to storage buffer (read-only) at
// descriptor set 3, binding 120 (the ring-buffer slot used by the
// WebGPU backend).
Vector<uint8_t> convert_push_constants_to_uniforms(const Vector<uint8_t> &p_bytes);

// Rewrite OpCopyLogical (SPIR-V 1.4+) to OpCopyObject.
// OpCopyLogical copies between logically equivalent struct types with
// different decorations; OpCopyObject is the simpler equivalent.
Vector<uint8_t> rewrite_copy_logical(const Vector<uint8_t> &p_bytes);

// Rewrite OpTerminateInvocation (SPV_KHR_terminate_invocation) to OpKill.
// OpTerminateInvocation has defined helper-invocation semantics but some
// frontends do not support it; OpKill is the SPIR-V 1.0 equivalent.
Vector<uint8_t> rewrite_terminate_invocation(const Vector<uint8_t> &p_bytes);

// Split combined image+sampler variables into separate image and sampler
// variables. Rewrites bindings (original binding N becomes sampler=N*2,
// image=N*2+1), inserts new types/variables, and replaces OpLoad of
// combined vars with separate loads + OpSampledImage.
Vector<uint8_t> split_combined_samplers(const Vector<uint8_t> &p_bytes);

// Fix OpTypeImage with depth=2 (unknown) by setting depth=1 (explicit
// depth). Returns the modified bytes and info about changed image types.
DepthImageFixResult fix_depth2_images(const Vector<uint8_t> &p_bytes);

// Negate the Y component of gl_Position in vertex shaders.
// Compensates for the difference between Vulkan's Y-down NDC (which
// Godot's GLSL shaders target) and WebGPU's Y-up NDC.
// Without this, all rendered content appears flipped vertically.
// Tint has no built-in coordinate space adjustment option, so this
// is done as a SPIR-V preprocessing pass instead.
Vector<uint8_t> negate_position_y(const Vector<uint8_t> &p_bytes);

// Mark images as depth images (OpTypeImage Depth=1) so Tint emits
// texture_depth_* instead of float types and the real depth attachment can be
// bound. WebGPU forbids binding a depth-format texture to a float sample type.
// In fragment-stage modules all multisampled float images are marked (the
// mobile renderer's MSAA depth resolve); p_force_depth_images marks every float
// image in the module, for the dedicated depth-copy shader.
Vector<uint8_t> fix_depth_images(const Vector<uint8_t> &p_bytes, bool p_force_depth_images);

// Strip OpDecorate/OpMemberDecorate for decorations unsupported by Tint:
// Restrict (19) — memory hint from glslang, no WGSL equivalent.
// InputAttachmentIndex (43) — Vulkan subpass inputs, no WebGPU equivalent.
Vector<uint8_t> strip_restrict_decoration(const Vector<uint8_t> &p_bytes);

// Replace OpMemoryBarrier with OpNop. Tint does not support
// OpMemoryBarrier (SPIR-V 225); WGSL has no direct equivalent.
// workgroupBarrier() (from OpControlBarrier) covers synchronization.
Vector<uint8_t> strip_memory_barrier(const Vector<uint8_t> &p_bytes);

// Replace non-finite (infinity, NaN) float constants with FLT_MAX/MIN.
// Tint asserts std::isfinite on all float literal values.
Vector<uint8_t> fix_nonfinite_literals(const Vector<uint8_t> &p_bytes);

// Unwrap arrays of handle types (images, samplers, sampled images)
// into single variables. Tint does not support arrays of handle types.
// Rewrites pointer types, removes access chains, and updates loads.
Vector<uint8_t> flatten_binding_arrays(const Vector<uint8_t> &p_bytes);

// Infer read-only storage buffers by analyzing write operations.
// Adds OpDecorate NonWritable to StorageBuffer variables that are never
// written to (no OpStore, OpAtomicStore, OpCopyMemory, etc.). This allows
// Tint to emit var<storage, read> instead of var<storage, read_write>.
Vector<uint8_t> infer_readonly_storage(const Vector<uint8_t> &p_bytes);

// Strip NonReadable from storage-buffer variables so Tint's WGSL writer emits
// `read_write` access instead of the invalid `write` access.
Vector<uint8_t> strip_storage_nonreadable(const Vector<uint8_t> &p_bytes);

// Inline user functions so handle (texture/sampler) parameters never reach
// Tint's image lowering pass. Workaround for the vendored Tint snapshot.
Vector<uint8_t> inline_user_functions(const Vector<uint8_t> &p_bytes, bool p_exhaustive = false);

} // namespace spirv_preprocess
