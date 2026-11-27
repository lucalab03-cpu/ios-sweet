/* Stands in for /usr/libexec/backboardd. Starts backboardd.inj (the real program with our library added as a dependency)
 * and, if that crashes twice in a row, the untouched backboardd.orig. Kill switch: a file /var/sweet/noinject. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

/*
 * 2026-11-27: one boot a drop. sweet-boot applies the dropbox while backboardd starts, so the first backboardd of a
 * boot had the old libsweet-gl and the old glargs, and a drop took two boots. When the dropbox holds a generation
 * newer than the one applied (header: "SWDROP01", u64 generation; /var/sweet/applied.gen), this waits for
 * sweet-boot to say it is done with it (/var/tmp/sweetboot.ready naming this boot), at most WAIT_MAX seconds.
 */
#define WAIT_MAX 600

static unsigned long long applied_gen(void)
{
	unsigned long long a = 0; FILE *f = fopen("/var/sweet/applied.gen", "r");
	if (f) { if (fscanf(f, "%llu", &a) != 1) a = 0; fclose(f); }
	return a;
}

static void wait_for_dropbox(long boot)
{
	unsigned char head[16]; unsigned long long gen = 0; FILE *d = fopen("/var/sweet/dropbox.bin", "rb"), *f;
	if (!d) return;
	if (fread(head, 1, sizeof(head), d) != sizeof(head) || memcmp(head, "SWDROP01", 8) != 0) { fclose(d); return; }
	fclose(d);
	for (int i = 0; i < 8; i++) gen |= (unsigned long long)head[8 + i] << (8 * i);
	if (gen <= applied_gen()) return;
	if ((f = fopen("/var/tmp/sweetwrap.log", "a")) != NULL) { fprintf(f, "wrap: dropbox generation %llu not applied yet: waiting for sweet-boot\n", gen); fclose(f); }
	int t = 0;
	for (; t < WAIT_MAX; t++) {
		long ready = 0; FILE *r = fopen("/var/tmp/sweetboot.ready", "r");
		if (r) { if (fscanf(r, "%ld", &ready) != 1) ready = 0; fclose(r); }
		if ((boot != 0 && ready == boot) || gen <= applied_gen()) break;
		sleep(1);
	}
	if ((f = fopen("/var/tmp/sweetwrap.log", "a")) != NULL) { fprintf(f, "wrap: waited %d s for the dropbox (applied %llu)\n", t, applied_gen()); fclose(f); }
}

int main(int argc, char **argv)
{
	long boot = boot_id(), last = 0, lastboot = -1; int count = 0;
	wait_for_dropbox(boot);
	long now = (long)time(NULL);
	FILE *s = fopen(STATE, "r");
	if (s) { if (fscanf(s, "%d %ld %ld", &count, &last, &lastboot) < 2) { count = 0; last = 0; } fclose(s); }
	/* The phone has no real-time clock (it starts at 1970-01-01 on every boot), so only a launch 0..29 s after the
	 * previous one in the same boot counts as a repeated crash. */
	count = (boot == lastboot && now >= last && now - last < 30) ? count + 1 : 1;
	s = fopen(STATE, "w"); if (s) { fprintf(s, "%d %ld %ld\n", count, now, boot); fclose(s); }
	/* round y: backboardd.inj lost its entitlements (round x: "backboardd.inj is not entitled" for the HID services,
	 * abort, twice); kernel patch 0153 answers dyld's AMFI policy request, so the untouched program takes
	 * DYLD_INSERT_LIBRARIES and the GL path goes there again (round w: dyld had ignored it) */
	int gl_wanted = access("/var/sweet/gl", F_OK) == 0 && access("/usr/local/lib/libsweet-gl.dylib", R_OK) == 0;
	int use_inj = count <= 2 && access(INJ, X_OK) == 0 && access("/var/sweet/noinject", F_OK) != 0;
	FILE *f = fopen("/var/tmp/sweetwrap.log", "a");
	if (f) { fprintf(f, "wrap: pid %d start, launch %d, using %s\n", (int)getpid(), count, use_inj ? "inj" : "orig"); fclose(f); }
	/*
	 * 2026-11-25: CoreAnimation on OpenGL ES (tools/sweetgl, from HakiOS): with /var/sweet/gl present, our EAGLContext
	 * (libsweet-gl.dylib, Mesa Panfrost behind it) goes into the window server too, in either program (the Metal
	 * driver gives backboardd no device then, so CoreAnimation takes its OpenGL ES renderer). Two crashes in a row
	 * (count above) end with the untouched program and no library at all.
	 */
	int gl = count <= 2 && gl_wanted;
	if (use_inj && access("/usr/lib/sweetinject.dylib", R_OK) == 0)
		setenv("DYLD_INSERT_LIBRARIES", gl ? "/usr/lib/sweetinject.dylib:/usr/local/lib/libsweet-gl.dylib" : "/usr/lib/sweetinject.dylib", 1);
	else if (gl)
		setenv("DYLD_INSERT_LIBRARIES", "/usr/local/lib/libsweet-gl.dylib", 1);
	if (gl)
		setenv("SWEET_GL", "1", 1);
	if (gl && (f = fopen("/var/tmp/sweetwrap.log", "a")) != NULL) { fprintf(f, "wrap: OpenGL ES for CoreAnimation (libsweet-gl)\n"); fclose(f); }
	execv(use_inj ? INJ : ORIG, argv);
	execv(ORIG, argv);
	return 1;
}
