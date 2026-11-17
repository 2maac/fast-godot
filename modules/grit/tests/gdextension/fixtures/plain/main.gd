extends Node2D

signal counted(value)

var total := 0
var label := "plain"

func _ready() -> void:
	print("plain start")
	for i in range(4):
		total += i * 2
	print("plain total: ", total)
	var items = ["x", "y"]
	for item in items:
		print("plain item: ", item)
	if total == 12:
		print("plain branch ok")
	else:
		print("plain branch unexpected")
	counted.emit(total)
	_apply(5)
	print("plain label: ", label, " total: ", total)
	print("PLAIN DONE")
	get_tree().quit()

func _apply(p_amount: int) -> void:
	total += p_amount
	label = label + "!"
