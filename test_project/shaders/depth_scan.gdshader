// Depth-based scanline overlay for the WebGPU smoke test.
//
// This is the material-side test for the mobile renderer's MSAA depth resolve:
// asking for `hint_depth_texture` makes the renderer copy/resolve the depth
// buffer into RB_TEX_BACK_DEPTH before the transparent pass, and
// `hint_screen_texture` forces the colour resolve alongside it. The depth read
// below then runs against the resolved R32F copy while the panel itself is
// drawn in the transparent pass.
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;

uniform sampler2D depth_texture : hint_depth_texture, repeat_disable, filter_nearest;
uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear;
uniform vec3 tint : source_color = vec3(0.15, 0.85, 1.0);
uniform float bands = 10.0;

void fragment() {
	float raw_depth = texture(depth_texture, SCREEN_UV).r;
	vec3 ndc = vec3(SCREEN_UV * 2.0 - 1.0, raw_depth);
	vec4 view = INV_PROJECTION_MATRIX * vec4(ndc, 1.0);
	view.xyz /= view.w;
	float linear_depth = -view.z;

	float band = smoothstep(0.35, 0.65, fract(linear_depth * bands - TIME * 1.5));
	vec3 scene = texture(screen_texture, SCREEN_UV).rgb;
	ALBEDO = mix(scene * 0.4, tint, band);
	ALPHA = 0.30 + band * 0.45;
}
