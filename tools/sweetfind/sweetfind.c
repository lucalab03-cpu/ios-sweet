/*
 * sweetfind: paths whose last component contains a string (case-insensitive), on the phone (2026-11-24).
 *
 *   sweetfind OUT STRING DIR...      walks each DIR (8 levels, symlinks not followed) and appends every matching path to
 *                                    OUT, then a count. Only directory entries are read, no file contents.
 *
 * Why: PosterBoard has no poster extensions to offer (PosterBoard.app has no PlugIns, PaperBoard.app has only
 * LegacyPoster.appex, /System/Library/ExtensionKit is missing), and its first-time setup then trips over its own
 * assertion. Where are CollectionsPoster.appex and its kin on this image, if anywhere? Run by sweetroot.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static FILE *out;
static const char *needle;
static unsigned seen, found;

static int
contains(const char *hay)
{
	size_t n = strlen(needle);
	for (; *hay; hay++) {
		if (strncasecmp(hay, needle, n) == 0) {
			return 1;
		}
	}
	return 0;
}

static void
walk(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[1024];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		seen++;
		if (contains(e->d_name)) {
			fprintf(out, "%s\n", path);
			found++;
		}
		if (depth > 0 && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			walk(path, depth - 1);
		}
	}
	closedir(d);
}

int
main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: sweetfind OUT STRING DIR...\n");
		return 2;
	}
	out = fopen(argv[1], "a");
	if (!out) {
		return 1;
	}
	needle = argv[2];
	for (int i = 3; i < argc; i++) {
		walk(argv[i], 8);
	}
	fprintf(out, "sweetfind %s: %u entries looked at, %u match\n", needle, seen, found);
	fclose(out);
	return 0;
}
