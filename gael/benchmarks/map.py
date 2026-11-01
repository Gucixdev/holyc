m = {}
for i in range(1000000):
    m[i * 7919 % 1000003] = i
total = 0
for i in range(1000000):
    total += m.get(i, -1)
print(len(m), total)
