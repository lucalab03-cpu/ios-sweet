/* Stands in for /usr/libexec/backboardd: starts the real one (backboardd.orig), with our library inserted when it can be.
 * Never fails on its own: if anything is missing the real backboardd still starts. Kill switch: a file /var/sweet/noinject. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	FILE *f = fopen("/var/tmp/sweetwrap.log", "a");
	int inject = access("/usr/local/lib/sweetinject.dylib", R_OK) == 0 && access("/var/sweet/noinject", F_OK) != 0;
	if (f) { fprintf(f, "wrap: pid %d start, inject %d\n", (int)getpid(), inject); fclose(f); }
	if (inject) setenv("DYLD_INSERT_LIBRARIES", "/usr/local/lib/sweetinject.dylib", 1);
	execv("/usr/libexec/backboardd.orig", argv);
	f = fopen("/var/tmp/sweetwrap.log", "a");
	if (f) { fprintf(f, "wrap: execv failed\n"); fclose(f); }
	return 1;
}
