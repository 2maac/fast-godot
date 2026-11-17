extends Node

func typed_ints() -> Array[int]:
	return [1, 2]

func typed_strings() -> Array[String]:
	return ["a", "b"]

func call_repeat(p_node: Node) -> String:
	return p_node._repeat("xy")

func typed_dict() -> Dictionary[String, int]:
	return {"a": 1}
