/* Puts the wrapper in place of backboardd (keeping the real one as backboardd.orig) and restarts backboardd once.
 * Safe to run on every boot: it does nothing when everything is already as it should be. */
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BB "/usr/libexec/backboardd"
#define ORIG "/usr/libexec/backboardd.orig"
#define WRAP "/usr/local/libexec/backboardd-wrap"

static void say(const char *fmt, ...)
{
	char l[400]; va_list ap;
	FILE *f = fopen("/var/tmp/sweetwrap.log", "a");
	va_start(ap, fmt); vsnprintf(l, sizeof(l), fmt, ap); va_end(ap);
	fputs(l, stdout); fflush(stdout);
	if (f) { fputs(l, f); fclose(f); }
}

static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

static int same(const char *a, const char *b)
{
	FILE *x = fopen(a, "rb"), *y = fopen(b, "rb"); int r = 0;
	if (x && y && fsize(a) == fsize(b)) {
		char p[4096], q[4096]; size_t n; r = 1;
		while (r && (n = fread(p, 1, sizeof(p), x)) > 0) if (fread(q, 1, n, y) != n || memcmp(p, q, n)) r = 0;
	}
	if (x) fclose(x); if (y) fclose(y);
	return r;
}

static int copy(const char *src, const char *dst, mode_t mode)
{
	char tmp[300]; char buf[8192]; ssize_t n; int in = open(src, O_RDONLY), out;
	snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
	if (in < 0) return -1;
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) { close(in); return -1; }
	while ((n = read(in, buf, sizeof(buf))) > 0) if (write(out, buf, n) != n) { n = -1; break; }
	close(in);
	if (n < 0 || fsync(out) != 0) { close(out); unlink(tmp); return -1; }
	close(out); chmod(tmp, mode);
	if (!same(src, tmp)) { unlink(tmp); return -1; }
	return rename(tmp, dst);
}

int main(void)
{
	int changed = 0;
	say("install-wrap: start\n");
	if (access(WRAP, X_OK) != 0) { say("install-wrap: no wrapper at %s\n", WRAP); return 1; }
	if (access(ORIG, F_OK) != 0) {
		say("install-wrap: copying %s to %s\n", BB, ORIG);
		if (copy(BB, ORIG, 0755) != 0) { say("install-wrap: copy of the original failed (%s)\n", strerror(errno)); return 2; }
		changed = 1;
	}
	if (!same(WRAP, BB)) {
		say("install-wrap: installing the wrapper\n");
		if (copy(WRAP, BB, 0755) != 0) { say("install-wrap: could not install (%s)\n", strerror(errno)); return 3; }
		changed = 1;
	}
	if (changed) {
		int pids[2048], n = proc_listallpids(pids, sizeof(pids));
		for (int i = 0; i < n; i++) {
			char name[64] = "";
			proc_name(pids[i], name, sizeof(name));
			if (!strcmp(name, "backboardd") || !strcmp(name, "backboardd.orig")) { say("install-wrap: restarting pid %d\n", pids[i]); kill(pids[i], SIGKILL); }
		}
	} else say("install-wrap: already installed\n");
	return 0;
}
