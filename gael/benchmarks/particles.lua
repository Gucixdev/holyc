local seed = 1
local function rand()
  seed = seed * 16807 % 2147483647
  return seed
end

local ps = {}
for i = 1, 10000 do
  local x = rand() % 1000 + 0.0
  local y = rand() % 1000 + 0.0
  local vx = rand() % 21 - 10 + 0.0
  local vy = rand() % 21 - 10 + 0.0
  ps[i] = { x = x, y = y, vx = vx, vy = vy }
end
for frame = 1, 600 do
  for i = 1, #ps do
    local p = ps[i]
    p.x = p.x + p.vx * 0.5
    p.y = p.y + p.vy * 0.5
    if p.x < 0.0 or p.x > 1000.0 then p.vx = 0.0 - p.vx end
    if p.y < 0.0 or p.y > 1000.0 then p.vy = 0.0 - p.vy end
  end
end
local sum = 0.0
for i = 1, #ps do sum = sum + ps[i].x + ps[i].y end
print(string.format("%d", math.floor(sum)))
