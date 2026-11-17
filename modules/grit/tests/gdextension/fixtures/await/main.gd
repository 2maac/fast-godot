extends Node2D

signal value_ready(value)
signal pair(a, b)

var ticks := 0

func _ready() -> void:
	print("T1 timer")
	await get_tree().create_timer(0.05).timeout
	for i in 3:
		await get_tree().process_frame
		ticks += 1
	print("T1 ticks: ", ticks)

	print("T2 nested loops")
	var total = 0
	for i in 2:
		for j in 3:
			await get_tree().process_frame
			total += i * j + 1
	print("T2 total: ", total)

	print("T3 while break/continue")
	var n = 0
	while true:
		n += 1
		if n % 2 == 0:
			await get_tree().process_frame
			continue
		if n > 5:
			break
		await get_tree().process_frame
	print("T3 n: ", n)

	print("T4 concurrent")
	_wait_and_report("A", 0.02)
	_wait_and_report("B", 0.01)
	await get_tree().create_timer(0.1).timeout
	print("T4 joined")

	print("T5 own signal value")
	call_deferred("_emit_value", 42)
	var received = await value_ready
	print("T5 value: ", received)

	print("T6 multi-arg signal")
	call_deferred("_emit_pair", 3, 4)
	var both = await pair
	print("T6 array: ", both, " size: ", both.size())

	print("T7 generated coroutine called with await")
	await _early()
	print("T7 returned (generated coroutines resume immediately)")

	print("T8 non-signal await")
	var direct = await 99
	print("T8 direct: ", direct)

	print("T9 GDScript coroutine")
	var helper = get_node("Helper")
	var doubled = await helper.later(21)
	print("T9 doubled: ", doubled)

	print("T11 two waiters on one signal")
	_wait_for_ping("A")
	_wait_for_ping("B")
	call_deferred("_emit_value", 9)
	await get_tree().create_timer(0.05).timeout
	print("T11 waiters done")

	print("T10 plain function in the same class")
	print("T10 sum: ", _sum(2, 3))

	print("ALL DONE")
	get_tree().quit()

func _wait_and_report(p_name: String, p_delay: float) -> void:
	await get_tree().create_timer(p_delay).timeout
	print("concurrent ", p_name)

func _emit_value(p_value: int) -> void:
	await get_tree().create_timer(0.02).timeout
	value_ready.emit(p_value)

func _emit_pair(p_a: int, p_b: int) -> void:
	await get_tree().create_timer(0.02).timeout
	pair.emit(p_a, p_b)

func _wait_for_ping(p_name: String) -> void:
	var got = await value_ready
	print("pinged ", p_name, ": ", got)

func _sum(p_a: int, p_b: int) -> int:
	return p_a + p_b

func _early() -> void:
	await get_tree().process_frame
	return
