extends Node2D

var hits := 0

func _ready() -> void:
	print("free start")
	call_deferred("queue_free")
	await get_tree().create_timer(0.2).timeout
	print("NEVER PRINTED")
	hits += 1
