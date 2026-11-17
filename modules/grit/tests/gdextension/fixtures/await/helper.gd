extends Node

func later(p_value: int) -> int:
	await get_tree().create_timer(0.03).timeout
	return p_value * 2

func call_back(p_node: Node) -> int:
	var value = await p_node._delayed_value(21)
	return value
