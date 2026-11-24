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
	print("WEBGPU_TEST_READY driver=%s adapter=%s" % [driver, adapter])
	if OS.has_feature("web"):
		JavaScriptBridge.eval("window.__webgpu_test_ready = true; console.log('WEBGPU_TEST_JS_READY');")

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
