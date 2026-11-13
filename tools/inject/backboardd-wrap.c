/* Stands in for /usr/libexec/backboardd. Starts backboardd.inj (the real program with our library added as a dependency)
 * and, if that crashes twice in a row, the untouched backboardd.orig. Kill switch: a file /var/sweet/noinject. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define INJ "/usr/libexec/backboardd.inj"
#define ORIG "/usr/libexec/backboardd.orig"
#define STATE "/var/tmp/sweetwrap.state"

int main(int argc, char **argv)
{
	long now = (long)time(NULL), last = 0; int count = 0;
	FILE *s = fopen(STATE, "r");
	if (s) { if (fscanf(s, "%d %ld", &count, &last) != 2) { count = 0; last = 0; } fclose(s); }
	count = (now - last < 30) ? count + 1 : 1;
	s = fopen(STATE, "w"); if (s) { fprintf(s, "%d %ld\n", count, now); fclose(s); }
	int use_inj = count <= 2 && access(INJ, X_OK) == 0 && access("/var/sweet/noinject", F_OK) != 0;
	FILE *f = fopen("/var/tmp/sweetwrap.log", "a");
	if (f) { fprintf(f, "wrap: pid %d start, launch %d, using %s\n", (int)getpid(), count, use_inj ? "inj" : "orig"); fclose(f); }
	if (use_inj && access("/usr/lib/sweetinject.dylib", R_OK) == 0) setenv("DYLD_INSERT_LIBRARIES", "/usr/lib/sweetinject.dylib", 1);
	execv(use_inj ? INJ : ORIG, argv);
	execv(ORIG, argv);
	return 1;
}
