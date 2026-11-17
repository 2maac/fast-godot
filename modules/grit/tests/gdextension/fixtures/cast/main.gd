extends Node2D

const HelperScript = preload("res://helper.gd")
const OtherScript = preload("res://other.gd")

var scores: Array[int] = [1]

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
	print("typed locals start")
	var built: Array[int] = [1, 2]
	print("built is Array[int]: ", built is Array[int])
	built.append(3)
	print("built size: ", built.size())
	var words: Array[String] = ["a"]
	print("words is Array[String]: ", words is Array[String])
	var table: Dictionary[String, int] = {"a": 1}
	print("table is Dictionary[String,int]: ", table is Dictionary[String,int])
	var empty: Array[int]
	print("empty is Array[int]: ", empty is Array[int], " size: ", empty.size())
	scores = [7, 8]
	print("scores is Array[int]: ", scores is Array[int], " size: ", scores.size())
	print("param size: ", _count_ints([1, 2, 3]))
	print("optional params start")
	print("defaults: ", _repeat("ab"))
	print("one arg: ", _repeat("ab", 3))
	print("two args: ", _repeat("ab", 1, "?"))
	print("float default: ", _scale(2.0))
	print("float given: ", _scale(2.0, 0.5))
	print("script-caller default: ", get_node("Helper").call_repeat(self))
	print("script types start")
	print("helper is HelperScript: ", helper is HelperScript)
	print("helper is OtherScript: ", helper is OtherScript)
	print("helper as HelperScript: ", (helper as HelperScript) == helper)
	print("helper as OtherScript is null: ", (helper as OtherScript) == null)
	print("colour: ", Color.from_hsv(0.5, 1.0, 1.0))
	print("direction: ", Vector2.from_angle(1.5707964))
	var parent = super.get_parent()
	print("super parent set: ", (parent is Window) or (parent is Node))
	print("TYPES DONE")
	get_tree().quit()

func _repeat(p_text: String, p_times: int = 2, p_mark: String = "!") -> String:
	var result = ""
	for i in p_times:
		result += p_text + p_mark
	return result

func _scale(p_value: float, p_factor: float = 3.0) -> float:
	return p_value * p_factor

func _count_ints(p_values: Array[int]) -> int:
	return p_values.size()

func _assert_helper() -> void:
	assert(false, "boom")
	print("helper continued")
