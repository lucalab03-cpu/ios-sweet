/*
 * libbbprobe.dylib: test library loaded into backboardd by bbshim.
 * It only proves that it is running inside backboardd: it writes /var/tmp/bbprobe/ok and a line in
 * /var/tmp/bbprobe/lib.log, then removes DYLD_INSERT_LIBRARIES so programs started by backboardd do not
 * inherit it. It changes nothing in backboardd.
 */
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

extern const char *getprogname(void);

__attribute__((constructor)) static void
bbprobe_init(void)
{
	char exe[1024];
	uint32_t len = sizeof(exe);
	FILE *f;

	unsetenv("DYLD_INSERT_LIBRARIES");
	if (_NSGetExecutablePath(exe, &len) != 0) {
		exe[0] = 0;
	}
	f = fopen("/var/tmp/bbprobe/lib.log", "a");
	if (f) {
		fprintf(f, "%ld LOADED pid=%d uid=%d prog=%s exe=%s images=%u\n", (long)time(NULL), (int)getpid(),
		    (int)getuid(), getprogname(), exe, _dyld_image_count());
		fclose(f);
	}
	f = fopen("/var/tmp/bbprobe/ok", "w");
	if (f) {
		fputs("ok\n", f);
		fclose(f);
	}
}
