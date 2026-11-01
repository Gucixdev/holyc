/* C has no hash map: an open-addressing table with linear probing, the kind a C program writes */
#include <stdio.h>
#include <stdlib.h>

#define CAP (1 << 21)
static long keys[CAP], vals[CAP];
static char used[CAP];

static unsigned long hash(long k) { return ((unsigned long)k * 11400714819323198485ul) >> 43; }

static void insert(long k, long v)
{
	unsigned long i = hash(k);
	while (used[i] && keys[i] != k) i = (i + 1) & (CAP - 1);
	used[i] = 1;
	keys[i] = k;
	vals[i] = v;
}

static int lookup(long k, long *v)
{
	unsigned long i = hash(k);
	while (used[i]) {
		if (keys[i] == k) { *v = vals[i]; return 1; }
		i = (i + 1) & (CAP - 1);
	}
	return 0;
}

int main(void)
{
	long i, v, n = 0, sum = 0;
	for (i = 0; i < 1000000; i++) insert(i * 7919 % 1000003, i);
	for (i = 0; i < CAP; i++) n += used[i];
	for (i = 0; i < 1000000; i++) sum += lookup(i, &v) ? v : -1;
	printf("%ld %ld\n", n, sum);
	return 0;
}
