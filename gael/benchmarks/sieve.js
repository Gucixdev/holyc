function sieve(n) {
  const flags = new Array(n).fill(true);
  let count = 0;
  for (let i = 2; i < n; i++) {
    if (!flags[i]) continue;
    count++;
    for (let k = i * i; k < n; k += i) flags[k] = false;
  }
  return count;
}

console.log(sieve(5000000));
