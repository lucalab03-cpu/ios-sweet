/*
 * sweetroot: applies a short list of file operations to the iOS root from the running system.
 *
 * The root is one writable HFS+ volume, so a bring-up experiment does not need a 7 minute image flash and the
 * fastboot hang that follows it: /var/sweet/root.cmd comes in with the dropbox (seconds), this program runs at
 * boot from /usr/local/etc/sweet-boot.list, and the change is on the phone from then on.
 *
 * Format: one operation per line; '#' comments and empty lines are ignored. Nothing is executed unless the
 * first operation is "apply": without it every operation is only printed (dry run), the safe default for a
 * file that can delete paths on a working installation.
 *
 *   apply                     run the operations below it (must come first)
 *   cp [-n] SRC DST           copy a file through DST.sweettmp, fsync, rename; -n leaves an existing DST alone
 *   mv SRC DST
 *   rm [-r] PATH...           -r for directories
 *   mkdir PATH                with parents
 *   chmod OCTAL PATH
 *   write PATH [TEXT]         create or truncate a file; the rest of the line is its text
 *   touch PATH [MODE]         create empty if missing, then chmod
 *   ls PATH
 *   cat PATH                  first 4 KiB
 *   run PROG [ARG...]         run a program, wait up to 300 s, log its status
 *   sync
 *
 * Log: /var/tmp/sweetroot.log (the outbox fetches /var/tmp/sweet*).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define CMD "/var/sweet/root.cmd"
#define LOG "/var/tmp/sweetroot.log"
#define MAXLINES 4000
#define RUN_TIMEOUT 300

static FILE *lf;
static int lines;
static int apply;

static void
say(const char *fmt, ...)
{
	va_list ap;

	if (lines >= MAXLINES) {
		return;
	}
	if (++lines == MAXLINES) {
		fprintf(lf, "SWEETROOT: line limit reached\n");
		return;
	}
	va_start(ap, fmt);
	vfprintf(lf, fmt, ap);
	va_end(ap);
	fflush(lf);
}

/* a path this program may touch: absolute, no ".." */
static int
sane(const char *p)
{
	return p != NULL && p[0] == '/' && strstr(p, "..") == NULL;
}

static void
show(const char *path)
{
	struct stat st;
	char when[32] = "?";

	if (lstat(path, &st) != 0) {
		say("  ? %s (%s)\n", path, strerror(errno));
		return;
	}
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", gmtime(&st.st_mtime));
	say("  %c %9lld %s %o %d:%d %s\n", S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : '-',
	    (long long)st.st_size, when, st.st_mode & 07777, (int)st.st_uid, (int)st.st_gid, path);
}

static int
copy_file(const char *src, const char *dst, int no_overwrite)
{
	char tmp[1100], buf[65536];
	struct stat st;
	int in, out, failed = 0;
	ssize_t n;

	if (!sane(src) || !sane(dst)) {
		say("cp: refused %s -> %s\n", src ? src : "(null)", dst ? dst : "(null)");
		return -1;
	}
	if (no_overwrite && lstat(dst, &st) == 0) {
		say("cp: %s exists, left alone\n", dst);
		return 0;
	}
	if (stat(src, &st) != 0) {
		say("cp: %s: %s\n", src, strerror(errno));
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		say("cp: %s is a directory (not supported)\n", src);
		return -1;
	}
	in = open(src, O_RDONLY);
	if (in < 0) {
		say("cp: open %s: %s\n", src, strerror(errno));
		return -1;
	}
	snprintf(tmp, sizeof(tmp), "%s.sweettmp", dst);
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
	if (out < 0) {
		say("cp: create %s: %s\n", tmp, strerror(errno));
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			failed = 1;
			break;
		}
	}
	if (n < 0) {
		failed = 1;
	}
	close(in);
	if (fsync(out) != 0) {
		failed = 1;
	}
	if (close(out) != 0) {
		failed = 1;
	}
	if (failed) {
		say("cp: writing %s: %s\n", tmp, strerror(errno));
		unlink(tmp);
		return -1;
	}
	chmod(tmp, st.st_mode & 0777);
	if (rename(tmp, dst) != 0) {
		say("cp: rename %s -> %s: %s\n", tmp, dst, strerror(errno));
		unlink(tmp);
		return -1;
	}
	show(dst);
	return 0;
}

static int
rm_path(const char *path, int recursive)
{
	struct stat st;

	if (!sane(path)) {
		say("rm: refused %s\n", path ? path : "(null)");
		return -1;
	}
	if (lstat(path, &st) != 0) {
		say("rm: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *e;

		if (!recursive) {
			say("rm: %s is a directory (use -r)\n", path);
			return -1;
		}
		if (d == NULL) {
			say("rm: open %s: %s\n", path, strerror(errno));
			return -1;
		}
		while ((e = readdir(d)) != NULL) {
			char child[1100];

			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
				continue;
			}
			snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
			rm_path(child, 1);
		}
		closedir(d);
		if (rmdir(path) != 0) {
			say("rm: %s: %s\n", path, strerror(errno));
			return -1;
		}
		say("removed directory %s\n", path);
		return 0;
	}
	if (unlink(path) != 0) {
		say("rm: %s: %s\n", path, strerror(errno));
		return -1;
	}
	say("removed %s\n", path);
	return 0;
}

static int
mkdirs(const char *path)
{
	char part[1100];

	if (!sane(path)) {
		say("mkdir: refused %s\n", path ? path : "(null)");
		return -1;
	}
	snprintf(part, sizeof(part), "%s", path);
	for (char *s = part + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			if (mkdir(part, 0755) != 0 && errno != EEXIST) {
				say("mkdir: %s: %s\n", part, strerror(errno));
				*s = '/';
				return -1;
			}
			*s = '/';
		}
	}
	if (mkdir(part, 0755) != 0 && errno != EEXIST) {
		say("mkdir: %s: %s\n", part, strerror(errno));
		return -1;
	}
	show(path);
	return 0;
}

static int
write_file(const char *path, const char *text)
{
	int fd;

	if (!sane(path)) {
		say("write: refused %s\n", path ? path : "(null)");
		return -1;
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		say("write: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (text != NULL && text[0] != 0 && write(fd, text, strlen(text)) != (ssize_t)strlen(text)) {
		say("write: %s: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (fsync(fd) != 0) {
		say("write: fsync %s: %s\n", path, strerror(errno));
	}
	close(fd);
	show(path);
	return 0;
}

static int
list_dir(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (d == NULL) {
		say("ls: %s: %s\n", path ? path : "(null)", strerror(errno));
		return -1;
	}
	say("  %s:\n", path);
	while ((e = readdir(d)) != NULL) {
		char child[1100];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		show(child);
	}
	closedir(d);
	return 0;
}

static int
cat_file(const char *path)
{
	char buf[4096];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0) {
		say("cat: %s: %s\n", path ? path : "(null)", strerror(errno));
		return -1;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0) {
		say("cat: %s: %s\n", path, strerror(errno));
		return -1;
	}
	buf[n] = 0;
	for (ssize_t i = 0; i < n; i++) {
		if ((unsigned char)buf[i] < 32 && buf[i] != '\n' && buf[i] != '\t') {
			buf[i] = '.';
		}
	}
	say("  %s (%zd bytes):\n%s\n", path, n, buf);
	return 0;
}

/*
 * The child's stdout and stderr, drained without blocking and written to the log with a marker, so that the result of
 * a tool that only prints (install-wrap's "patch: N" and "verify: N") is in the same place as everything else.
 */
static int child_line_start = 1;

static void
drain(int fd)
{
	char buf[512];
	ssize_t n;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n && lines < MAXLINES; i++) {
			if (child_line_start) {
				fputs("  | ", lf);
				child_line_start = 0;
			}
			fputc(buf[i], lf);
			if (buf[i] == '\n') {
				child_line_start = 1;
				lines++;
			}
		}
		fflush(lf);
	}
}

static int
run_prog(char **argv)
{
	pid_t pid;
	int status = 0, p[2] = { -1, -1 };
	posix_spawn_file_actions_t fa;

	if (!sane(argv[0])) {
		say("run: refused %s\n", argv[0] ? argv[0] : "(null)");
		return -1;
	}
	if (pipe(p) != 0) {
		say("run: pipe: %s\n", strerror(errno));
		return -1;
	}
	/*
	 * sweet-boot starts this program without stdin/stdout/stderr, so the pipe could be descriptors 0-2, and the
	 * dup2/close actions below then undo each other (boot 2026-11-23: every "run" failed, "Undefined error: 0").
	 * The pipe's ends are moved above 2 first.
	 */
	for (int i = 0; i < 2; i++) {
		if (p[i] <= STDERR_FILENO) {
			int moved = fcntl(p[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
			if (moved >= 0) {
				close(p[i]);
				p[i] = moved;
			}
		}
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, p[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	posix_spawn_file_actions_addclose(&fa, p[1]);
	int spawn_error = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	if (spawn_error != 0) {
		/* posix_spawn returns its error, it does not set errno */
		say("run: cannot start %s: %s (%d)\n", argv[0], strerror(spawn_error), spawn_error);
		posix_spawn_file_actions_destroy(&fa);
		close(p[0]);
		close(p[1]);
		return -1;
	}
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
	for (int t = 0;; t++) {
		pid_t r;

		drain(p[0]);
		r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			drain(p[0]);          /* whatever is still in the pipe */
			close(p[0]);
			say("run: %s exited with status %d\n", argv[0], status);
			return 0;
		}
		if (t >= RUN_TIMEOUT) {
			say("run: %s did not finish in %d s, killed\n", argv[0], RUN_TIMEOUT);
			kill(pid, 9);
			waitpid(pid, &status, 0);
			drain(p[0]);
			close(p[0]);
			return -1;
		}
		sleep(1);
	}
}

/* the rest of the line from token number `from`, joined by single spaces */
static void
rest_of_line(char *line, int from, char *out, size_t size)
{
	int i = 0;

	out[0] = 0;
	while (line[i] && from > 0) {
		while (line[i] == ' ' || line[i] == '\t') {
			i++;
		}
		while (line[i] && line[i] != ' ' && line[i] != '\t') {
			i++;
		}
		from--;
	}
	while (line[i] == ' ' || line[i] == '\t') {
		i++;
	}
	snprintf(out, size, "%s", line + i);
}

int
main(int argc, char **argv)
{
	const char *cmdpath = argc > 1 ? argv[1] : CMD;
	FILE *f;
	char line[1024];
	int n = 0;

	lf = fopen(LOG, "w");
	if (lf == NULL) {
		return 1;
	}
	say("SWEETROOT: start, pid %d, commands from %s\n", (int)getpid(), cmdpath);
	f = fopen(cmdpath, "r");
	if (f == NULL) {
		say("SWEETROOT: no command file (%s)\n", strerror(errno));
		fclose(lf);
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		char *tok[20];
		int nt = 0;
		char *p = line;

		line[strcspn(line, "\r\n")] = 0;
		while (*p) {
			while (*p == ' ' || *p == '\t') {
				*p++ = 0;
			}
			if (*p == 0) {
				break;
			}
			if (nt < 20) {
				tok[nt++] = p;
			}
			while (*p && *p != ' ' && *p != '\t') {
				p++;
			}
		}
		if (nt == 0 || tok[0][0] == '#') {
			continue;
		}
		n++;
		if (!strcmp(tok[0], "apply")) {
			apply = 1;
			say("SWEETROOT: apply mode on (line %d)\n", n);
			continue;
		}
		if (!apply) {
			say("dry: %s\n", line);
			continue;
		}
		if (!strcmp(tok[0], "cp") && nt >= 3) {
			int no = 0, at = 1;

			if (!strcmp(tok[1], "-n")) {
				no = 1;
				at = 2;
			}
			if (nt < at + 2) {
				say("cp: needs SRC DST\n");
			} else {
				copy_file(tok[at], tok[at + 1], no);
			}
		} else if (!strcmp(tok[0], "mv") && nt >= 3) {
			if (!sane(tok[1]) || !sane(tok[2])) {
				say("mv: refused\n");
			} else if (rename(tok[1], tok[2]) != 0) {
				say("mv: %s -> %s: %s\n", tok[1], tok[2], strerror(errno));
			} else {
				say("moved %s -> %s\n", tok[1], tok[2]);
			}
		} else if (!strcmp(tok[0], "rm") && nt >= 2) {
			int recursive = 0, at = 1;

			if (!strcmp(tok[1], "-r")) {
				recursive = 1;
				at = 2;
			}
			for (; at < nt; at++) {
				rm_path(tok[at], recursive);
			}
		} else if (!strcmp(tok[0], "mkdir") && nt >= 2) {
			mkdirs(tok[1]);
		} else if (!strcmp(tok[0], "chmod") && nt >= 3) {
			if (!sane(tok[2])) {
				say("chmod: refused\n");
			} else if (chmod(tok[2], (mode_t)strtol(tok[1], NULL, 8)) != 0) {
				say("chmod: %s: %s\n", tok[2], strerror(errno));
			} else {
				show(tok[2]);
			}
		} else if (!strcmp(tok[0], "write") && nt >= 2) {
			char text[900];

			rest_of_line(line, 2, text, sizeof(text));
			write_file(tok[1], text);
		} else if (!strcmp(tok[0], "touch") && nt >= 2) {
			mode_t mode = nt >= 3 ? (mode_t)strtol(tok[2], NULL, 8) : 0644;

			if (write_file(tok[1], NULL) == 0) {
				chmod(tok[1], mode);
			}
		} else if (!strcmp(tok[0], "ls") && nt >= 2) {
			list_dir(tok[1]);
		} else if (!strcmp(tok[0], "cat") && nt >= 2) {
			cat_file(tok[1]);
		} else if (!strcmp(tok[0], "run") && nt >= 2) {
			run_prog(&tok[1]);
		} else if (!strcmp(tok[0], "sync")) {
			sync();
			say("synced\n");
		} else {
			say("unknown operation, skipped: %s\n", line);
		}
	}
	fclose(f);
	say("SWEETROOT: done, %d line(s)\n", n);
	fclose(lf);
	return 0;
}
