#include <stdio.h>

int main(void)
{
	long sum = 0, i;
	for (i = 0; i < 20000000; i++)
		if (i % 3 == 0 || i % 5 == 0) sum += i;
	printf("%ld\n", sum);
	return 0;
}
