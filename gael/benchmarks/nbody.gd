extends SceneTree

const PI_: float = 3.141592653589793
const SOLAR_MASS: float = 4.0 * PI_ * PI_
const DAYS: float = 365.24


class Body:
	var x: float
	var y: float
	var z: float
	var vx: float
	var vy: float
	var vz: float
	var mass: float

	func _init(x_: float, y_: float, z_: float, vx_: float, vy_: float, vz_: float, mass_: float) -> void:
		x = x_
		y = y_
		z = z_
		vx = vx_
		vy = vy_
		vz = vz_
		mass = mass_


var bodies: Array[Body] = []


func energy() -> float:
	var e: float = 0.0
	var n: int = bodies.size()
	for i: int in n:
		var b: Body = bodies[i]
		e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz)
		for j: int in range(i + 1, n):
			var c: Body = bodies[j]
			var dx: float = b.x - c.x
			var dy: float = b.y - c.y
			var dz: float = b.z - c.z
			e -= (b.mass * c.mass) / sqrt(dx * dx + dy * dy + dz * dz)
	return e


func advance(dt: float) -> void:
	var n: int = bodies.size()
	for i: int in n:
		var b: Body = bodies[i]
		for j: int in range(i + 1, n):
			var c: Body = bodies[j]
			var dx: float = b.x - c.x
			var dy: float = b.y - c.y
			var dz: float = b.z - c.z
			var d2: float = dx * dx + dy * dy + dz * dz
			var mag: float = dt / (d2 * sqrt(d2))
			b.vx -= dx * c.mass * mag
			b.vy -= dy * c.mass * mag
			b.vz -= dz * c.mass * mag
			c.vx += dx * b.mass * mag
			c.vy += dy * b.mass * mag
			c.vz += dz * b.mass * mag
	for b: Body in bodies:
		b.x += dt * b.vx
		b.y += dt * b.vy
		b.z += dt * b.vz


func _init() -> void:
	bodies = [
		Body.new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS),
		Body.new(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03 * DAYS,
			7.69901118419740425e-03 * DAYS, -6.90460016972063023e-05 * DAYS, 9.54791938424326609e-04 * SOLAR_MASS),
		Body.new(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03 * DAYS,
			4.99852801234917238e-03 * DAYS, 2.30417297573763929e-05 * DAYS, 2.85885980666130812e-04 * SOLAR_MASS),
		Body.new(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03 * DAYS,
			2.37847173959480950e-03 * DAYS, -2.96589568540237556e-05 * DAYS, 4.36624404335156298e-05 * SOLAR_MASS),
		Body.new(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03 * DAYS,
			1.62824170038242295e-03 * DAYS, -9.51592254519715870e-05 * DAYS, 5.15138902046611451e-05 * SOLAR_MASS),
	]
	var px: float = 0.0
	var py: float = 0.0
	var pz: float = 0.0
	for b: Body in bodies:
		px += b.vx * b.mass
		py += b.vy * b.mass
		pz += b.vz * b.mass
	bodies[0].vx = 0.0 - px / SOLAR_MASS
	bodies[0].vy = 0.0 - py / SOLAR_MASS
	bodies[0].vz = 0.0 - pz / SOLAR_MASS
	var before: float = energy()
	for k: int in 200000:
		advance(0.01)
	print("%d %d" % [int(before * 1000000000.0), int(energy() * 1000000000.0)])
	quit()
