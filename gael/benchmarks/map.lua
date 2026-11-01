local m, n = {}, 0
for i = 0, 1000000 - 1 do m[i * 7919 % 1000003] = i end
for _ in pairs(m) do n = n + 1 end             -- a Lua table does not keep its count
local sum = 0
for i = 0, 1000000 - 1 do
  local v = m[i]
  if v then sum = sum + v else sum = sum - 1 end
end
print(string.format("%d %d", n, sum))
