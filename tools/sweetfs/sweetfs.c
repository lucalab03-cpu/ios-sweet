/*
 * sweetfs: read-only look at where PosterBoard keeps its state (diagnosis, changes nothing).
 *
 * PosterBoard asserts "!_stateLock_switcherConfiguration" in -[PBFPosterExtensionDataStore
 * _stateLock_setupSwitcherConfigurationForFirstTime] on every boot: a switcher configuration is already in memory
 * while the data store believes it is the first run. The configuration can only come from disk, so this lists
 * what is there. Output: /var/tmp/sweetfs.log (the outbox fetches /var/tmp/sweet*).
 *
 * It looks for entries whose name contains "oster" under the places an app or extension keeps data, prints each
 * one (type, size, mtime), and walks into the directories it found, a few levels deep.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG "/var/tmp/sweetfs.log"
#define MAXLINES 4000

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

int
main(void)
{
	lf = fopen(LOG, "w");
	if (lf == NULL) {
		return 1;
	}
	say("SWEETFS: start\n");
	find_posters("/var/mobile/Library", 3, 6);
	find_posters("/var/mobile/Containers/Data/PluginKitPlugin", 2, 6);
	find_posters("/var/mobile/Containers/Data/Application", 2, 6);
	find_posters("/var/mobile/Containers/Shared/AppGroup", 2, 6);
	find_posters("/var/mobile/Containers/Data/System", 2, 6);
	find_posters("/var/mobile/Containers/Shared/SystemGroup", 2, 6);
	find_posters("/var/root/Library", 3, 4);
	find_posters("/var/db", 2, 4);
	/* where the system posters (extensions) are */
	find_posters("/System/Library/PrivateFrameworks", 1, 1);
	find_posters("/System/Library/ExtensionKit/Extensions", 1, 3);
	find_posters("/System/Library/PrivateFrameworks/PosterBoard.framework", 1, 1);
	say("SWEETFS: done\n");
	fclose(lf);
	return 0;
}
