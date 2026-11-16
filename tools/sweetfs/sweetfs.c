/*
 * sweetfs: read-only look at where PosterBoard keeps its state (diagnosis, changes nothing).
 *
 * PosterBoard asserts in -[PBFPosterExtensionDataStore _stateLock_setupSwitcherConfigurationForFirstTime]
 * 30 to 70 s after every launch, on the prewarm path (PRSServer -> PBFPosterExtensionDataStoreXPCServiceGlue
 * -> prewarmWithCompletion: -> executeUpdate: -> _stateLock_updateExtensions:): a switcher configuration is
 * already in memory while the data store believes it is the first run. The store on disk holds nothing but
 * empty directories (59/Extensions, 59/GalleryCache) while com.apple.PosterBoard.plist already has
 * PosterBoard's own keys (locale en_US, SnapshotsBuildVersion 20H392), so either the configuration never
 * reaches the disk or it is written somewhere else. Output: /var/tmp/sweetfs.log (the outbox fetches
 * /var/tmp/sweet*).
 *
 * Third version: it sleeps 45 s first, so the first prewarm (and its store setup) has happened and
 * PosterBoard has had the time to write something or to crash; then it lists the store tree, tries the
 * write pattern a data store uses (create, write, fsync, close, rename, unlink) inside the store and prints
 * the errno of every step, lists again, and dumps the preferences and the newest crash reports.
 *
 * It also looks for entries whose name contains "oster" under the places an app or extension keeps data,
 * prints each one (type, size, mtime), and walks into the directories it found, a few levels deep.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG "/var/tmp/sweetfs.log"
#define MAXLINES 8000
#define DUMPMAX (64 * 1024)
#define STORE "/var/mobile/Library/Application Support/PRBPosterExtensionDataStore"
#define PREF "/var/mobile/Library/Preferences/com.apple.PosterBoard.plist"

static FILE *lf;
static int lines;

static void
say(const char *fmt, ...)
{
	va_list ap;

	if (lines >= MAXLINES) {
		return;
	}
	if (++lines == MAXLINES) {
		fprintf(lf, "SWEETFS: line limit reached\n");
		return;
	}
	va_start(ap, fmt);
	vfprintf(lf, fmt, ap);
	va_end(ap);
	fflush(lf);
}

static void
show(const char *path)
{
	struct stat st;
	char when[32] = "?";

	if (lstat(path, &st) != 0) {
		say("  ? %s (cannot stat)\n", path);
		return;
	}
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", gmtime(&st.st_mtime));
	say("  %c %9lld %s %o %d:%d %s\n", S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : '-',
	    (long long)st.st_size, when, st.st_mode & 07777, (int)st.st_uid, (int)st.st_gid, path);
}

/* everything below dir, depth levels down */
static void
walk(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (d == NULL) {
		say("  ! cannot open %s\n", dir);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[1024];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		show(path);
		if (depth > 1 && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			walk(path, depth - 1);
		}
	}
	closedir(d);
}

/* entries of dir whose name contains "oster": shown, and walked into */
static void
find_posters(const char *dir, int depth, int into)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (d == NULL) {
		say("! cannot open %s\n", dir);
		return;
	}
	say("== %s\n", dir);
	while ((e = readdir(d)) != NULL) {
		char path[1024];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (strcasestr(e->d_name, "oster") != NULL) {
			show(path);
			if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
				walk(path, into);
			}
		} else if (depth > 1 && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			find_posters(path, depth - 1, into);
		}
	}
	closedir(d);
}

/* a file's contents: as text when it is text, else as hex + ascii (property lists are often binary) */
static void
dump_file(const char *path)
{
	unsigned char buf[DUMPMAX];
	FILE *f = fopen(path, "rb");
	size_t n, i, bad = 0;

	say("#### %s\n", path);
	if (f == NULL) {
		say("! cannot open\n");
		return;
	}
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	for (i = 0; i < n; i++) {
		if (buf[i] == 0 || (buf[i] < 32 && buf[i] != '\n' && buf[i] != '\t' && buf[i] != '\r')) {
			bad++;
		}
	}
	if (bad * 20 < n + 1) {
		fwrite(buf, 1, n, lf);
		fputs("\n", lf);
	} else {
		for (i = 0; i < n; i += 16) {
			size_t j;

			fprintf(lf, "%06zx ", i);
			for (j = 0; j < 16; j++) {
				if (i + j < n) {
					fprintf(lf, " %02x", buf[i + j]);
				} else {
					fputs("   ", lf);
				}
			}
			fputs("  ", lf);
			for (j = 0; j < 16 && i + j < n; j++) {
				fputc(buf[i + j] >= 32 && buf[i + j] < 127 ? buf[i + j] : '.', lf);
			}
			fputc('\n', lf);
		}
	}
	fflush(lf);
	say("#### end %s (%zu bytes read)\n", path, n);
}

/* the last (by name = by time) count files of dir that start with prefix and end in suffix */
static void
dump_newest(const char *dir, const char *prefix, const char *suffix, int count)
{
	struct dirent **list = NULL;
	int n = scandir(dir, &list, NULL, alphasort), i, shown = 0;

	say("== newest %s%s* in %s\n", prefix, suffix, dir);
	for (i = n - 1; i >= 0 && shown < count; i--) {
		size_t len = strlen(list[i]->d_name), sl = strlen(suffix);

		if (strncmp(list[i]->d_name, prefix, strlen(prefix)) == 0 && len >= sl &&
		    strcmp(list[i]->d_name + len - sl, suffix) == 0) {
			char path[1024];

			snprintf(path, sizeof(path), "%s/%s", dir, list[i]->d_name);
			dump_file(path);
			shown++;
		}
	}
	if (n < 0) {
		say("! cannot read %s\n", dir);
	}
}

/* the store's own tree, with sizes and mtimes (the mtime of a directory moves when a file lands in it) */
static void
tree(const char *tag)
{
	say("== store tree (root, then 4 levels): %s\n", tag);
	show(STORE);
	walk(STORE, 4);
}

/* the write pattern a data store uses: temp file, write, fsync, close, atomic rename, unlink */
static void
probe(void)
{
	char tmp[512], fin[512], dir[512], buf[4096];
	int fd, i, err, nw = 0;
	ssize_t n;

	snprintf(tmp, sizeof(tmp), "%s/sweetfs-probe.tmp", STORE);
	snprintf(fin, sizeof(fin), "%s/sweetfs-probe.final", STORE);
	memset(buf, 'a' + (int)(getpid() % 26), sizeof(buf));

	say("== write probe in the store (what the data store itself would do)\n");
	errno = 0;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	say("  open   -> %d (errno %d)\n", fd, fd < 0 ? errno : 0);
	if (fd < 0) {
		say("  ! the store directory cannot be written: no file can be persisted there\n");
		return;
	}
	n = 0;
	for (i = 0; i < 16; i++) {
		n = write(fd, buf, sizeof(buf));
		if (n != (ssize_t)sizeof(buf)) {
			break;
		}
		nw += (int)n;
	}
	say("  write  %d bytes -> %d (errno %d)\n", nw, (int)n, n < 0 ? errno : 0);
	errno = 0;
	err = fsync(fd);
	say("  fsync  -> %d (errno %d)\n", err, err ? errno : 0);
	errno = 0;
	err = close(fd);
	say("  close  -> %d (errno %d)\n", err, err ? errno : 0);
	errno = 0;
	err = rename(tmp, fin);
	say("  rename -> %d (errno %d)\n", err, err ? errno : 0);
	show(fin);
	{
		unsigned char back[8];
		size_t got;
		int e;
		FILE *f = fopen(fin, "rb");

		e = f == NULL ? errno : 0;
		got = f == NULL ? 0 : fread(back, 1, sizeof(back), f);
		if (f != NULL) {
			fclose(f);
		}
		say("  (read back: %zu bytes, first 0x%02x, open errno %d)\n", got, got ? back[0] : 0, e);
	}
	errno = 0;
	err = unlink(fin);
	say("  unlink -> %d (errno %d)\n", err, err ? errno : 0);

	/* and a directory, as a store does for each extension */
	snprintf(dir, sizeof(dir), "%s/sweetfs-probe-dir", STORE);
	errno = 0;
	err = mkdir(dir, 0755);
	say("  mkdir  -> %d (errno %d)\n", err, err ? errno : 0);
	show(dir);
	if (err == 0) {
		errno = 0;
		err = rmdir(dir);
		say("  rmdir  -> %d (errno %d)\n", err, err ? errno : 0);
	}
	tree("right after the probe");
}

int
main(void)
{
	lf = fopen(LOG, "w");
	if (lf == NULL) {
		return 1;
	}
	say("SWEETFS: start, pid %d\n", (int)getpid());
	say("SWEETFS: waiting 45 s, so PosterBoard's first prewarm has run (it asserts 30 to 70 s after launch)\n");
	sleep(45);
	tree("at +45 s");
	probe();
	find_posters("/var/mobile/Library", 3, 6);
	find_posters("/var/mobile/Containers/Data/PluginKitPlugin", 2, 6);
	find_posters("/var/mobile/Containers/Data/Application", 2, 6);
	find_posters("/var/mobile/Containers/Shared/AppGroup", 2, 6);
	find_posters("/var/mobile/Containers/Data/System", 2, 6);
	find_posters("/var/mobile/Containers/Shared/SystemGroup", 2, 6);
	find_posters("/var/root/Library", 3, 4);
	find_posters("/var/db", 2, 4);
	dump_file(PREF);
	dump_newest("/var/mobile/Library/Logs/CrashReporter", "PosterBoard-", ".ips", 2);
	say("SWEETFS: done\n");
	fclose(lf);
	return 0;
}
