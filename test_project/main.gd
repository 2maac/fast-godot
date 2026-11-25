extends Node3D

@onready var box: MeshInstance3D = $Box
@onready var hud: Label = $HUD/Label

var _time := 0.0
var _frames := 0
var _fps_time := 0.0
var _fps := 0

func _ready() -> void:
	var driver := RenderingServer.get_current_rendering_driver_name()
	var adapter := RenderingServer.get_video_adapter_name()
	$Camera3D.look_at_from_position(Vector3(0.0, 2.2, 5.0), Vector3(0.0, 0.8, 0.0), Vector3.UP)
	_apply_checker_texture()
	_build_depth_scan_panel()
	_build_soft_particles()
	_build_2d_shadow_demo()
	_setup_depth_of_field()
	print("WEBGPU_TEST_READY driver=%s adapter=%s" % [driver, adapter])
	if OS.has_feature("web"):
		JavaScriptBridge.eval("window.__webgpu_test_ready = true; console.log('WEBGPU_TEST_JS_READY');")

# Give the shaded cube a procedural checker albedo texture so the smoke test also
# exercises material texture sampling/filtering on top of the lighting path.
func _apply_checker_texture() -> void:
	var size := 64
	var cell := 8
	var img := Image.create(size, size, false, Image.FORMAT_RGBA8)
	for y in size:
		for x in size:
			var light := ((x / cell) + (y / cell)) % 2 == 0
			img.set_pixel(x, y, Color(0.95, 0.95, 0.95) if light else Color(0.35, 0.5, 0.4))
	var tex := ImageTexture.create_from_image(img)
	var mat: StandardMaterial3D = box.get_surface_override_material(0)
	if mat != null:
		mat = mat.duplicate()
		mat.albedo_texture = tex
		mat.uv1_scale = Vector3(3.0, 3.0, 1.0)
		box.set_surface_override_material(0, mat)

# Transparent panel that samples the (resolved) depth + screen textures. On the
# mobile renderer this is what forces the MSAA depth resolve path.
func _build_depth_scan_panel() -> void:
	var quad := QuadMesh.new()
	quad.size = Vector2(1.9, 1.05)
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/depth_scan.gdshader")
	var panel := MeshInstance3D.new()
	panel.name = "DepthScanPanel"
	panel.mesh = quad
	panel.material_override = material
	panel.position = Vector3(-1.55, 1.5, 1.35)
	add_child(panel)

# Billboards with proximity fade: BaseMaterial3D adds a hint_depth_texture
# sampler for proximity fade, so particles exercise depth texture sampling from
# the transparent pass (and the GPU particle compute path).
func _build_soft_particles() -> void:
	var process := ParticleProcessMaterial.new()
	process.direction = Vector3(0.0, 1.0, 0.0)
	process.spread = 30.0
	process.initial_velocity_min = 0.15
	process.initial_velocity_max = 0.45
	process.gravity = Vector3(0.0, 0.12, 0.0)
	process.scale_min = 0.4
	process.scale_max = 0.9

	var quad := QuadMesh.new()
	quad.size = Vector2(0.28, 0.28)
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.billboard_mode = BaseMaterial3D.BILLBOARD_PARTICLES
	material.vertex_color_use_as_albedo = true
	material.albedo_color = Color(1.0, 0.55, 0.9, 0.85)
	material.albedo_texture = _make_soft_dot_texture()
	material.proximity_fade_enabled = true
	material.proximity_fade_distance = 0.55
	quad.material = material

	var particles := GPUParticles3D.new()
	particles.name = "SoftParticles"
	particles.amount = 32
	particles.lifetime = 2.6
	particles.preprocess = 1.2
	particles.process_material = process
	particles.draw_pass_1 = quad
	particles.position = Vector3(0.35, 0.05, 1.35)
	particles.visibility_aabb = AABB(Vector3(-1.5, -0.5, -1.5), Vector3(3.0, 3.0, 3.0))
	# Trails re-render the same proximity-fade material from transform history.
	particles.trail_enabled = true
	particles.trail_lifetime = 0.35
	add_child(particles)
	particles.emitting = true

# Small radial sprite used as the particle albedo so the quads read as dots.
func _make_soft_dot_texture() -> ImageTexture:
	var size := 32
	var img := Image.create(size, size, false, Image.FORMAT_RGBA8)
	var centre := Vector2(size * 0.5, size * 0.5)
	for y in size:
		for x in size:
			var d := Vector2(x + 0.5, y + 0.5).distance_to(centre) / (size * 0.5)
			img.set_pixel(x, y, Color(1, 1, 1, clampf(1.0 - d, 0.0, 1.0)))
	return ImageTexture.create_from_image(img)

# Far-field depth of field: bokeh_dof_raster samples the depth texture at the
# post-process stage, so this exercises the resolved/copied depth a second time
# (and the raster DOF path instead of the compute one).
func _setup_depth_of_field() -> void:
	var attributes := CameraAttributesPractical.new()
	attributes.dof_blur_far_enabled = true
	attributes.dof_blur_far_distance = 5.5
	attributes.dof_blur_far_transition = 3.0
	attributes.dof_blur_amount = 0.18
	$Camera3D.attributes = attributes

# 2D layer: a lit ColorRect covered by a PointLight2D with shadows and a
# LightOccluder2D. This drives canvas.glsl's shadow-atlas sampling path
# (texture_shadow -> textureGather on the R32F 2D shadow atlas).
func _build_2d_shadow_demo() -> void:
	var layer := CanvasLayer.new()
	layer.name = "ShadowDemo2D"
	layer.layer = 2
	add_child(layer)

	var floor_rect := ColorRect.new()
	floor_rect.name = "LitFloor"
	floor_rect.color = Color(0.06, 0.09, 0.15, 1.0)
	floor_rect.position = Vector2(0.0, 400.0)
	floor_rect.size = Vector2(960.0, 140.0)
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/canvas_shadow.gdshader")
	floor_rect.material = material
	layer.add_child(floor_rect)

	var gradient := Gradient.new()
	gradient.offsets = PackedFloat32Array([0.0, 1.0])
	gradient.colors = PackedColorArray([Color(1, 1, 1, 1), Color(1, 1, 1, 0)])
	var light_texture := GradientTexture2D.new()
	light_texture.width = 128
	light_texture.height = 128
	light_texture.fill = GradientTexture2D.FILL_RADIAL
	light_texture.fill_from = Vector2(0.5, 0.5)
	light_texture.fill_to = Vector2(1.0, 0.5)
	light_texture.gradient = gradient

	var light := PointLight2D.new()
	light.name = "ShadowLight"
	light.texture = light_texture
	light.position = Vector2(300.0, 470.0)
	light.energy = 1.8
	light.texture_scale = 3.0
	light.shadow_enabled = true
	light.shadow_filter = Light2D.SHADOW_FILTER_PCF5
	# Light2D only affects items in its range_layer_min..max window, which
	# defaults to the base canvas (layer 0). This demo lives in CanvasLayer 2.
	light.range_layer_min = 2
	light.range_layer_max = 2
	light.shadow_filter_smooth = 2.0
	layer.add_child(light)

	var occluder_polygon := OccluderPolygon2D.new()
	occluder_polygon.polygon = PackedVector2Array([
		Vector2(-9.0, -70.0), Vector2(9.0, -70.0),
		Vector2(9.0, 70.0), Vector2(-9.0, 70.0),
	])
	var occluder := LightOccluder2D.new()
	occluder.name = "ShadowCaster"
	occluder.occluder = occluder_polygon
	occluder.position = Vector2(470.0, 470.0)
	layer.add_child(occluder)

func _process(delta: float) -> void:
	_time += delta
	box.rotation.y = _time * 0.8
	box.rotation.x = sin(_time * 0.6) * 0.25
	_frames += 1
	_fps_time += delta
	if _fps_time >= 0.5:
		_fps = int(round(_frames / _fps_time))
		_frames = 0
		_fps_time = 0.0
		var draws := RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)
		hud.text = "Fast_Godot + WebGPU smoke test\ndriver: webgpu\nadapter: %s\nfps: %d\ndraws: %d" % [RenderingServer.get_video_adapter_name(), _fps, draws]
		if OS.has_feature("web"):
			# Expose real numbers for the headless Playwright smoke test.
			JavaScriptBridge.eval("window.__webgpu_fps = %d; window.__webgpu_draws = %d;" % [_fps, draws])
