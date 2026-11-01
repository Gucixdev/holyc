extends SceneTree


func _init() -> void:
	var sum: int = 0
	for i: int in 20000000:
		if i % 3 == 0 or i % 5 == 0:
			sum += i
	print(sum)
	quit()
