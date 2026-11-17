extends Node2D

func _ready() -> void:
	print("types start")
	var number: Variant = 5
	var real: Variant = 2.75
	print("is int: ", number is int)
	print("is String: ", number is String)
	var items: Variant = [1, 2]
	print("is Array: ", items is Array)
	var helper = get_node("Helper")
	var ints: Variant = helper.typed_ints()
	var strings: Variant = helper.typed_strings()
	var mapping: Variant = helper.typed_dict()
	print("is Array[int]: ", ints is Array[int])
	print("is Array[String] on ints: ", ints is Array[String])
	print("is Array[String]: ", strings is Array[String])
	print("is Dictionary[String,int]: ", mapping is Dictionary[String, int])
	print("is Dictionary[String,String]: ", mapping is Dictionary[String, String])
	var tree: Variant = get_tree()
	print("tree is MainLoop: ", tree is MainLoop)
	print("tree is Node: ", tree is Node)
	print("tree as Node is null: ", (tree as Node) == null)
	print("real as int: ", real as int)
	print("number as String: ", number as String)
	assert(true, "fine")
	_assert_helper()
	print("colour: ", Color.from_hsv(0.5, 1.0, 1.0))
	print("direction: ", Vector2.from_angle(1.5707964))
	var parent = super.get_parent()
	print("super parent set: ", (parent is Window) or (parent is Node))
	print("TYPES DONE")
	get_tree().quit()

func _assert_helper() -> void:
	assert(false, "boom")
	print("helper continued")
