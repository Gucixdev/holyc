let seed = 1;
function rand() { seed = seed * 16807 % 2147483647; return seed; }

const ps = [];
for (let i = 0; i < 10000; i++) {
  const x = rand() % 1000, y = rand() % 1000, vx = rand() % 21 - 10, vy = rand() % 21 - 10;
  ps.push({ x, y, vx, vy });
}
for (let frame = 0; frame < 600; frame++) {
  for (const p of ps) {
    p.x += p.vx * 0.5;
    p.y += p.vy * 0.5;
    if (p.x < 0.0 || p.x > 1000.0) p.vx = 0.0 - p.vx;
    if (p.y < 0.0 || p.y > 1000.0) p.vy = 0.0 - p.vy;
  }
}
let sum = 0.0;
for (const p of ps) sum += p.x + p.y;
console.log(Math.trunc(sum));
