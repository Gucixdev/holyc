#include <stdio.h>
#include <stdlib.h>

static long sieve(long n)
{
	char *flags = malloc((size_t)n);
	long count = 0, i, k;
	for (i = 0; i < n; i++) flags[i] = 1;
	for (i = 2; i < n; i++) {
		if (!flags[i]) continue;
		count++;
		for (k = i * i; k < n; k += i) flags[k] = 0;
	}
	free(flags);
	return count;
}

int main(void)
{
	printf("%ld\n", sieve(5000000));
	return 0;
}
