extends Node

func later(p_value: int) -> int:
	await get_tree().create_timer(0.03).timeout
	return p_value * 2
