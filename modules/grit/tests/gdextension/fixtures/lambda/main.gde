class_name LambdaMain
extends Node2D

signal hit(value)

func _ready() -> void:
	print("lambda start")
	var values = [3, 1, 2]
	values.sort_custom(func(a, b): return a > b)
	print("sorted: ", values)
	var factor = 10
	var scaled = values.map(func(v): return v * factor)
	print("scaled: ", scaled)
	var parent_getter = func(): return get_parent()
	print("self lambda: ", parent_getter.call() == get_parent())
	hit.connect(func(v): print("hit ", v))
	hit.emit(7)
	print("LAMBDA DONE")
	get_tree().quit()
