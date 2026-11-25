/* Stands in for /usr/libexec/backboardd. Starts backboardd.inj (the real program with our library added as a dependency)
 * and, if that crashes twice in a row, the untouched backboardd.orig. Kill switch: a file /var/sweet/noinject. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define INJ "/usr/libexec/backboardd.inj"
#define ORIG "/usr/libexec/backboardd.orig"
#define STATE "/var/tmp/sweetwrap.state"

/* Seconds of kern.boottime: different on every boot, so a crash count never carries over to the next boot. */
static long boot_id(void)
{
	struct timeval tv = { 0, 0 }; size_t len = sizeof(tv); int mib[2] = { CTL_KERN, KERN_BOOTTIME };
	return sysctl(mib, 2, &tv, &len, NULL, 0) == 0 ? (long)tv.tv_sec : 0;
}

int main(int argc, char **argv)
{
	long now = (long)time(NULL), last = 0, boot = boot_id(), lastboot = -1; int count = 0;
	FILE *s = fopen(STATE, "r");
	if (s) { if (fscanf(s, "%d %ld %ld", &count, &last, &lastboot) < 2) { count = 0; last = 0; } fclose(s); }
	/* The phone has no real-time clock (it starts at 1970-01-01 on every boot), so only a launch 0..29 s after the
	 * previous one in the same boot counts as a repeated crash. */
	count = (boot == lastboot && now >= last && now - last < 30) ? count + 1 : 1;
	s = fopen(STATE, "w"); if (s) { fprintf(s, "%d %ld %ld\n", count, now, boot); fclose(s); }
	int use_inj = count <= 2 && access(INJ, X_OK) == 0 && access("/var/sweet/noinject", F_OK) != 0;
	FILE *f = fopen("/var/tmp/sweetwrap.log", "a");
	if (f) { fprintf(f, "wrap: pid %d start, launch %d, using %s\n", (int)getpid(), count, use_inj ? "inj" : "orig"); fclose(f); }
	/*
	 * 2026-11-25: CoreAnimation on OpenGL ES (tools/sweetgl, from HakiOS): with /var/sweet/gl present, our EAGLContext
	 * (libsweet-gl.dylib, Mesa Panfrost behind it) goes into the window server too, in either program (the Metal
	 * driver gives backboardd no device then, so CoreAnimation takes its OpenGL ES renderer). Two crashes in a row
	 * (count above) end with the untouched program and no library at all.
	 */
	int gl = use_inj && access("/var/sweet/gl", F_OK) == 0 && access("/usr/local/lib/libsweet-gl.dylib", R_OK) == 0;
	if (use_inj && access("/usr/lib/sweetinject.dylib", R_OK) == 0)
		setenv("DYLD_INSERT_LIBRARIES", gl ? "/usr/lib/sweetinject.dylib:/usr/local/lib/libsweet-gl.dylib" : "/usr/lib/sweetinject.dylib", 1);
	if (gl)
		setenv("SWEET_GL", "1", 1);
	if (gl && (f = fopen("/var/tmp/sweetwrap.log", "a")) != NULL) { fprintf(f, "wrap: OpenGL ES for CoreAnimation (libsweet-gl)\n"); fclose(f); }
	execv(use_inj ? INJ : ORIG, argv);
	execv(ORIG, argv);
	return 1;
}
