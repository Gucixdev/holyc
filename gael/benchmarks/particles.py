class Particle:
    __slots__ = ("x", "y", "vx", "vy")

    def __init__(self, x, y, vx, vy):
        self.x, self.y, self.vx, self.vy = x, y, vx, vy


seed = 1


def rand():
    global seed
    seed = seed * 16807 % 2147483647
    return seed


ps = []
for i in range(10000):
    x = float(rand() % 1000)
    y = float(rand() % 1000)
    vx = float(rand() % 21 - 10)
    vy = float(rand() % 21 - 10)
    ps.append(Particle(x, y, vx, vy))
for frame in range(600):
    for p in ps:
        p.x += p.vx * 0.5
        p.y += p.vy * 0.5
        if p.x < 0.0 or p.x > 1000.0:
            p.vx = 0.0 - p.vx
        if p.y < 0.0 or p.y > 1000.0:
            p.vy = 0.0 - p.vy
total = 0.0
for p in ps:
    total += p.x + p.y
print(int(total))
