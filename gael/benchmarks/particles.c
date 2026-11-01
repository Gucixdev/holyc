#include <stdio.h>

typedef struct { double x, y, vx, vy; } Particle;

static long seed = 1;
static long rnd(void) { seed = seed * 16807 % 2147483647; return seed; }

static Particle ps[10000];

int main(void)
{
	double sum = 0;
	int i, frame;
	for (i = 0; i < 10000; i++) {
		ps[i].x = (double)(rnd() % 1000);
		ps[i].y = (double)(rnd() % 1000);
		ps[i].vx = (double)(rnd() % 21 - 10);
		ps[i].vy = (double)(rnd() % 21 - 10);
	}
	for (frame = 0; frame < 600; frame++) {
		for (i = 0; i < 10000; i++) {
			Particle *p = &ps[i];
			p->x += p->vx * 0.5;
			p->y += p->vy * 0.5;
			if (p->x < 0.0 || p->x > 1000.0) p->vx = 0.0 - p->vx;
			if (p->y < 0.0 || p->y > 1000.0) p->vy = 0.0 - p->vy;
		}
	}
	for (i = 0; i < 10000; i++) sum += ps[i].x + ps[i].y;
	printf("%ld\n", (long)sum);
	return 0;
}
