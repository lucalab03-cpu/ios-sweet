/*
 * bbshim: sits at /usr/libexec/backboardd. launchd starts it (user mobile) and it exec()s the real
 * backboardd (/usr/libexec/backboardd.real) in the same process, so launchd still sees its own job and the
 * Mach services keep working.
 *
 * It tries to load /usr/local/lib/libbbprobe.dylib into backboardd with DYLD_INSERT_LIBRARIES.
 * Safety: if the library has never confirmed it loaded (file "ok"), at most 2 tries are made; after that
 * backboardd starts plain. So if iOS kills backboardd because of the library, the screen comes back on the
 * third start (KeepAlive restarts it by itself).
 *
 * Files in /var/tmp/bbprobe/: shim.log (one line per start), tries, ok (written by the library),
 * off (if it exists: never inject).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REAL  "/usr/libexec/backboardd.real"
#define LIB   "/usr/local/lib/libbbprobe.dylib"
#define DIR   "/var/tmp/bbprobe"
#define MAXTRIES 2

static void
logline(const char *fmt, const char *a, long b)
{
	FILE *f = fopen(DIR "/shim.log", "a");

	if (!f) {
		return;
	}
	fprintf(f, "%ld pid=%d uid=%d ", (long)time(NULL), (int)getpid(), (int)getuid());
	fprintf(f, fmt, a, b);
	fputc('\n', f);
	fclose(f);
}

static long
read_num(const char *path)
{
	FILE *f = fopen(path, "r");
	long n = 0;

	if (f) {
		if (fscanf(f, "%ld", &n) != 1) {
			n = 0;
		}
		fclose(f);
	}
	return n;
}

static void
write_num(const char *path, long n)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fprintf(f, "%ld\n", n);
		fclose(f);
	}
}

int
main(int argc, char **argv)
{
	int inject = 0;
	long tries;

	(void)argc;
	mkdir(DIR, 0777);
	chmod(DIR, 0777);
	tries = read_num(DIR "/tries");

	if (access(DIR "/off", F_OK) == 0) {
		logline("%s tries=%ld", "off-file: plain start", tries);
	} else if (access(LIB, R_OK) != 0) {
		logline("%s tries=%ld", "library missing: plain start", tries);
	} else if (access(DIR "/ok", F_OK) == 0) {
		inject = 1;
		logline("%s tries=%ld", "library confirmed before: inject", tries);
	} else if (tries < MAXTRIES) {
		inject = 1;
		write_num(DIR "/tries", tries + 1);
		logline("%s tries=%ld", "inject attempt", tries + 1);
	} else {
		logline("%s tries=%ld", "FALLBACK: library never confirmed, plain start", tries);
	}

	if (inject) {
		setenv("DYLD_INSERT_LIBRARIES", LIB, 1);
	} else {
		unsetenv("DYLD_INSERT_LIBRARIES");
	}
	argv[0] = (char *)REAL;
	execv(REAL, argv);

	/* exec failed: try once more without the library, then give up (launchd will restart us) */
	logline("execv failed: %s (%ld)", strerror(errno), (long)errno);
	unsetenv("DYLD_INSERT_LIBRARIES");
	execv(REAL, argv);
	logline("execv plain failed too: %s (%ld)", strerror(errno), (long)errno);
	return 1;
}
