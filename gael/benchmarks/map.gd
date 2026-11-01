extends SceneTree


func _init() -> void:
	var m: Dictionary[int, int] = {}
	for i: int in 1000000:
		m[i * 7919 % 1000003] = i
	var sum: int = 0
	for i: int in 1000000:
		sum += m.get(i, -1)
	print("%d %d" % [m.size(), sum])
	quit()
