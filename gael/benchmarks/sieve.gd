extends SceneTree


func sieve(n: int) -> int:
	var flags: Array[bool] = []
	flags.resize(n)
	flags.fill(true)
	var count: int = 0
	for i: int in range(2, n):
		if flags[i]:
			count += 1
			var k: int = i * i
			while k < n:
				flags[k] = false
				k += i
	return count


func _init() -> void:
	print(sieve(5000000))
	quit()
