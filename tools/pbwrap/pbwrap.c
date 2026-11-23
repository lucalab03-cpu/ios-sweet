/*
 * pbwrap: stands in for a program and starts the same program with a dylib added (NAME.inj), or the untouched
 * NAME.orig when the injected one crashes twice in a row. Built for /Applications/PosterBoard.app/PosterBoard: the
 * assert text and the file operations of PosterBoard are invisible from outside, and this is the one way in
 * without a launchd plist change (the root image would have to be reflashed for that).
 *
 * Installed by /var/sweet/root.cmd (sweetroot) at boot; kill switch: a file /var/sweet/noposter (its own: the
 * /var/sweet/noinject an old drop left on the phone is backboardd-wrap's, and kept every launch on .orig, boot
 * 2026-11-24).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#else
/* so the file can be syntax checked away from a Mac; never used on the phone */
static int
_NSGetExecutablePath(char *buf, uint32_t *size)
{
	ssize_t n;
	int fd = open("/proc/self/exe", 0);
	(void)size;
	if (fd < 0) {
		return -1;
	}
	n = read(fd, buf, 1023);
	close(fd);
	if (n <= 0) {
		return -1;
	}
	buf[n] = 0;
	return 0;
}
#endif

#define DYLIB "/usr/lib/sweetposter.dylib"
#define LOG "/var/tmp/sweetposter.log"
#define STATE "/var/tmp/pbwrap.state"

static void
say(const char *fmt, ...)
{
	char line[400];
	__builtin_va_list ap;
	FILE *f = fopen(LOG, "a");

	if (f == NULL) {
		return;
	}
	__builtin_va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	__builtin_va_end(ap);
	fputs(line, f);
	fflush(f);
	fclose(f);
}

/* Seconds of kern.boottime: different on every boot, so a crash count never carries over (no real-time clock). */
static long
boot_id(void)
{
#ifdef __APPLE__
	struct timeval tv = { 0, 0 };
	size_t len = sizeof(tv);
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };

	return sysctl(mib, 2, &tv, &len, NULL, 0) == 0 ? (long)tv.tv_sec : 0;
#else
	return 0;   /* only used for the syntax check away from a Mac */
#endif
}

int
main(int argc, char **argv)
{
	char self[1024], inj[1100], orig[1100];
	uint32_t size = sizeof(self);
	long now = (long)time(NULL), last = 0, boot = boot_id(), lastboot = -1;
	int count = 0, use_inj;
	FILE *s;

	(void)argc;
	if (_NSGetExecutablePath(self, &size) != 0) {
		return 1;
	}
	snprintf(inj, sizeof(inj), "%s.inj", self);
	snprintf(orig, sizeof(orig), "%s.orig", self);
	s = fopen(STATE, "r");
	if (s != NULL) {
		if (fscanf(s, "%d %ld %ld", &count, &last, &lastboot) < 2) {
			count = 0;
			last = 0;
		}
		fclose(s);
	}
	/* Only a launch 0..29 s after the previous one in the same boot counts as a repeated crash. */
	count = (boot == lastboot && now >= last && now - last < 30) ? count + 1 : 1;
	s = fopen(STATE, "w");
	if (s != NULL) {
		fprintf(s, "%d %ld %ld\n", count, now, boot);
		fclose(s);
	}
	use_inj = count <= 2 && access(inj, X_OK) == 0 && access(orig, X_OK) == 0 &&
	    access("/var/sweet/noposter", F_OK) != 0 && access(DYLIB, R_OK) == 0;
	say("wrap: pid %d start, launch %d, using %s\n", (int)getpid(), count, use_inj ? "inj" : "orig");
	if (use_inj) {
		setenv("DYLD_INSERT_LIBRARIES", DYLIB, 1);
	}
	execv(use_inj ? inj : orig, argv);
	say("wrap: execv %s failed: %s\n", use_inj ? inj : orig, strerror(errno));
	return 1;
}
