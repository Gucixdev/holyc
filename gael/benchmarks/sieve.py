def sieve(n):
    flags = [True] * n
    count = 0
    for i in range(2, n):
        if flags[i]:
            count += 1
            k = i * i
            while k < n:
                flags[k] = False
                k += i
    return count

print(sieve(5000000))
