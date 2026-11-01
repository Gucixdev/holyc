extends SceneTree


class Particle:
	var x: float
	var y: float
	var vx: float
	var vy: float

	func _init(x_: float, y_: float, vx_: float, vy_: float) -> void:
		x = x_
		y = y_
		vx = vx_
		vy = vy_


var seed_: int = 1


func rand_() -> int:
	seed_ = seed_ * 16807 % 2147483647
	return seed_


func _init() -> void:
	var ps: Array[Particle] = []
	for i: int in 10000:
		var x: float = float(rand_() % 1000)
		var y: float = float(rand_() % 1000)
		var vx: float = float(rand_() % 21 - 10)
		var vy: float = float(rand_() % 21 - 10)
		ps.append(Particle.new(x, y, vx, vy))
	for frame: int in 600:
		for p: Particle in ps:
			p.x += p.vx * 0.5
			p.y += p.vy * 0.5
			if p.x < 0.0 or p.x > 1000.0:
				p.vx = 0.0 - p.vx
			if p.y < 0.0 or p.y > 1000.0:
				p.vy = 0.0 - p.vy
	var sum: float = 0.0
	for p: Particle in ps:
		sum += p.x + p.y
	print(int(sum))
	quit()
