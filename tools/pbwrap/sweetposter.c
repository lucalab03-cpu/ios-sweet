/*
 * sweetposter: inserted into PosterBoard. Prints the text BaseBoard's assert records just before the trap, and the
 * file operations PosterBoard makes on the poster store with their errno. Nothing is changed, only looked at.
 *
 * Why inside the process: PosterBoard dies with SIGTRAP in
 * -[PBFPosterExtensionDataStore _stateLock_setupSwitcherConfigurationForFirstTime] 30-70 s after every launch, and
 * the assertion text never reaches the crash report (the registers show _bs_set_crash_log_message and
 * -[__NSCFString UTF8String], so the message is built and handed over right before the breakpoint). The store on
 * disk holds only empty directories, so the second question is which writes the store attempts and with what
 * errno. Both are visible only from inside.
 *
 * Loaded by /usr/local/libexec/pbwrap (DYLD_INSERT_LIBRARIES), which stands in for the PosterBoard binary.
 * Log: /var/tmp/sweetposter.log
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG "/var/tmp/sweetposter.log"
#define MAXLINES 20000

/* dyld's interposition table (the iOS SDK does not ship <mach-o/dyld-interposing.h>) */
#define INTERPOSE(replacement, replacee)                                                                    \
	__attribute__((used)) static struct {                                                              \
		const void *replacement;                                                                   \
		const void *replacee;                                                                      \
	} _interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {                         \
		(const void *)(unsigned long)&replacement, (const void *)(unsigned long)&replacee          \
	}

/* Neither name is declared by the iOS SDK; interposition needs the symbol to point at. */
extern void _bs_set_crash_log_message(const char *);
extern int renamex_np(const char *, const char *, unsigned int);

static int lines;

static void
say(const char *fmt, ...)
{
	char line[900];
	va_list ap;
	FILE *f;

	if (lines >= MAXLINES) {
		return;
	}
	if (++lines == MAXLINES) {
		f = fopen(LOG, "a");
		if (f) {
			fputs("sweetposter: line limit reached\n", f);
			fclose(f);
		}
		return;
	}
	f = fopen(LOG, "a");
	if (f == NULL) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, f);
	fflush(f);
	fclose(f);
}

/* a C string that may be anything: printable ASCII only, bounded */
static void
text(char *out, size_t size, const char *s)
{
	size_t n = 0;

	out[0] = 0;
	if (s == NULL) {
		snprintf(out, size, "(null)");
		return;
	}
	while (n + 1 < size && n < 500 && s[n] != 0) {
		char c = s[n];

		out[n] = (c >= 32 && c < 127) ? c : '.';
		n++;
	}
	out[n] = 0;
}

/* paths that matter: the poster machinery and the preferences it writes */
static int
worth_it(const char *p)
{
	return p != NULL && (strstr(p, "Poster") != NULL || strstr(p, "poster") != NULL || strstr(p, "PRB") != NULL ||
	    strstr(p, "Preferences") != NULL);
}

/*
 * The assert itself. BaseBoard's _bs_set_crash_log_message(const char *) receives the finished message; the
 * replacement logs it and hands the same pointer to the real function, so the crash behaves exactly as before.
 */
static void
sweet_bs_set_crash_log_message(const char *msg)
{
	static void (*real)(const char *);
	char t[600];

	if (real == NULL) {
		real = (void (*)(const char *))dlsym(RTLD_NEXT, "_bs_set_crash_log_message");
	}
	text(t, sizeof(t), msg);
	say("ASSERT: %s\n", t);
	if (real != NULL) {
		real(msg);
	} else {
		say("ASSERT: (no real _bs_set_crash_log_message)\n");
	}
}
INTERPOSE(sweet_bs_set_crash_log_message, _bs_set_crash_log_message);

static int
sweet_rename(const char *from, const char *to)
{
	static int (*real)(const char *, const char *);
	int r;

	if (real == NULL) {
		real = (int (*)(const char *, const char *))dlsym(RTLD_NEXT, "rename");
	}
	errno = 0;
	r = real(from, to);
	if (r != 0 || worth_it(from) || worth_it(to)) {
		say("rename %s -> %s: %d (errno %d)\n", from, to, r, r ? errno : 0);
	}
	return r;
}
INTERPOSE(sweet_rename, rename);

static int
sweet_renamex_np(const char *from, const char *to, unsigned int flags)
{
	static int (*real)(const char *, const char *, unsigned int);
	int r;

	if (real == NULL) {
		real = (int (*)(const char *, const char *, unsigned int))dlsym(RTLD_NEXT, "renamex_np");
	}
	errno = 0;
	r = real(from, to, flags);
	if (r != 0 || worth_it(from) || worth_it(to)) {
		say("renamex_np %s -> %s flags 0x%x: %d (errno %d)\n", from, to, flags, r, r ? errno : 0);
	}
	return r;
}
INTERPOSE(sweet_renamex_np, renamex_np);

static int
sweet_fsync(int fd)
{
	static int (*real)(int);
	int r;

	if (real == NULL) {
		real = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
	}
	errno = 0;
	r = real(fd);
	if (r != 0) {
		say("fsync fd %d: %d (errno %d)\n", fd, r, errno);
	}
	return r;
}
INTERPOSE(sweet_fsync, fsync);

static int
sweet_mkdir(const char *path, mode_t mode)
{
	static int (*real)(const char *, mode_t);
	int r;

	if (real == NULL) {
		real = (int (*)(const char *, mode_t))dlsym(RTLD_NEXT, "mkdir");
	}
	errno = 0;
	r = real(path, mode);
	if (r != 0 || worth_it(path)) {
		say("mkdir %s %o: %d (errno %d)\n", path, (unsigned)mode, r, r ? errno : 0);
	}
	return r;
}
INTERPOSE(sweet_mkdir, mkdir);

static int
sweet_unlink(const char *path)
{
	static int (*real)(const char *);
	int r;

	if (real == NULL) {
		real = (int (*)(const char *))dlsym(RTLD_NEXT, "unlink");
	}
	errno = 0;
	r = real(path);
	if (r != 0 || worth_it(path)) {
		say("unlink %s: %d (errno %d)\n", path, r, r ? errno : 0);
	}
	return r;
}
INTERPOSE(sweet_unlink, unlink);

static int
sweet_rmdir(const char *path)
{
	static int (*real)(const char *);
	int r;

	if (real == NULL) {
		real = (int (*)(const char *))dlsym(RTLD_NEXT, "rmdir");
	}
	errno = 0;
	r = real(path);
	if (r != 0 || worth_it(path)) {
		say("rmdir %s: %d (errno %d)\n", path, r, r ? errno : 0);
	}
	return r;
}
INTERPOSE(sweet_rmdir, rmdir);

static FILE *
sweet_fopen(const char *path, const char *mode)
{
	static FILE *(*real)(const char *, const char *);
	FILE *r;

	if (real == NULL) {
		real = (FILE * (*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");
	}
	errno = 0;
	r = real(path, mode);
	if (r == NULL || worth_it(path)) {
		say("fopen %s %s: %p (errno %d)\n", path, mode, (void *)r, r == NULL ? errno : 0);
	}
	return r;
}
INTERPOSE(sweet_fopen, fopen);

__attribute__((constructor)) static void
init(void)
{
	char path[1024] = "?";
	uint32_t size = sizeof(path);

	extern int _NSGetExecutablePath(char *, uint32_t *);
	_NSGetExecutablePath(path, &size);
	say("poster: loaded in pid %d uid %d exe %s\n", (int)getpid(), (int)getuid(), path);
}
