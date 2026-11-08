/* A short CPU test: integer work and memory copying, a few seconds. Output: stdout and /var/tmp/sweetbench.log */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double
now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int
main(void)
{
	FILE *log = fopen("/var/tmp/sweetbench.log", "a");
	char line[200];
	size_t size = 32u << 20;
	char *a = malloc(size), *b = malloc(size);

	for (int run = 0; run < 3; run++) {
		volatile uint64_t x = 1;
		double t0 = now(), t;
		uint64_t n = 0;

		do {
			for (int i = 0; i < 1000000; i++) {
				x = x * 6364136223846793005ull + 1442695040888963407ull;
			}
			n += 1000000;
			t = now();
		} while (t - t0 < 1.5);
		snprintf(line, sizeof(line), "cpubench: integer loop %.1f million/s\n", n / (t - t0) / 1e6);
		fputs(line, stdout);
		if (log) {
			fputs(line, log);
		}
	}
	memset(a, 1, size);
	double t0 = now();
	for (int i = 0; i < 10; i++) {
		memcpy(b, a, size);
	}
	double t = now() - t0;
	snprintf(line, sizeof(line), "cpubench: memcpy %.0f MB/s (%c)\n", 10.0 * size / t / 1e6, b[size / 2] + 47);
	fputs(line, stdout);
	if (log) {
		fputs(line, log);
		fclose(log);
	}
	return 0;
}
