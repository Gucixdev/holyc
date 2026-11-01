local function sieve(n)
  local flags = {}
  for i = 0, n - 1 do flags[i] = true end
  local count = 0
  for i = 2, n - 1 do
    if flags[i] then
      count = count + 1
      local k = i * i
      while k < n do
        flags[k] = false
        k = k + i
      end
    end
  end
  return count
end

print(sieve(5000000))
