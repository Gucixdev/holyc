const m = new Map();
for (let i = 0; i < 1000000; i++) m.set(i * 7919 % 1000003, i);
let sum = 0;
for (let i = 0; i < 1000000; i++) {
  const v = m.get(i);
  sum += v === undefined ? -1 : v;
}
console.log(m.size + " " + sum);
