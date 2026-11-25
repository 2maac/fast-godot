/* clang-format off */
#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec2 uv_interp;
/* clang-format on */

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
	uv_interp = gl_Position.xy * 0.5 + 0.5;
}

/* clang-format off */
#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec2 uv_interp;

// Copies a depth attachment into an R32F framebuffer. WebGPU forbids binding a
// depth-format texture to a float sample type, so the WebGPU driver types this
// image as a depth texture in SPIR-V (texture_depth_2d) and redirects the
// sampler to a non-filtering alias binding in the generated WGSL. The uniform
// name and the shader name (CopyDepthToFbShaderRD) are what the driver keys on.
layout(set = 0, binding = 0) uniform sampler2D source_depth;

layout(location = 0) out float frag_color;

void main() {
	frag_color = texture(source_depth, uv_interp).r;
}
