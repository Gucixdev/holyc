extends SceneTree


func fib(n: int) -> int:
	return n if n < 2 else fib(n - 1) + fib(n - 2)


func _init() -> void:
	print(fib(34))
	quit()
