// Lit 2D surface for the WebGPU smoke test.
//
// The shader itself only sets COLOR: lighting (and therefore the 2D shadow
// atlas lookups in canvas.glsl's built-in light path, including the
// textureGather-based `texture_shadow()`) is added by the engine because the
// item is covered by a PointLight2D with shadows enabled.
shader_type canvas_item;

uniform vec4 floor_color : source_color = vec4(0.06, 0.09, 0.15, 1.0);

void fragment() {
	COLOR = floor_color;
}
