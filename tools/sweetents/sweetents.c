/*
 * sweetents: a binary's entitlements, and which binaries hold a given one, on the phone (2026-11-24).
 *
 *   sweetents -o OUT FILE...                 the XML entitlements of each (arm64 or fat), into OUT
 *   sweetents -o OUT -s STRING DIR...         every regular file under DIR (4 levels, files up to 96 MB) that
 *                                            contains STRING, into OUT (appended)
 *
 * Why: PosterBoard dies after RunningBoard refuses its prewarm assertion, "originator doesn't have entitlement
 * com.apple.runningboard.posterkit.host" (boot 2026-11-24 17:35). Which program should have it, and does the one that
 * runs have it? Run by sweetroot (`run /usr/local/bin/sweetents ...`).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MH_MAGIC_64     0xfeedfacfu
#define FAT_MAGIC_BE    0xcafebabeu
#define CPU_ARM64       0x0100000cu
#define LC_CODE_SIG     0x1du
#define CSMAGIC_EMBEDDED 0xfade0cc0u
#define CSMAGIC_ENTS    0xfade7171u
#define CSMAGIC_DERENTS 0xfade7172u

static FILE *out;

static uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t
le32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

/* One thin arm64 image at `base` (size `len`): its XML entitlements printed. */
static void
thin(const char *path, const uint8_t *base, size_t len)
{
	if (len < 32 || le32(base) != MH_MAGIC_64) {
		fprintf(out, "%s: not a 64-bit Mach-O\n", path);
		return;
	}
	uint32_t ncmds = le32(base + 16), off = 32;
	for (uint32_t i = 0; i < ncmds && off + 8 <= len; i++) {
		uint32_t cmd = le32(base + off), size = le32(base + off + 4);
		if (cmd == LC_CODE_SIG && off + 16 <= len) {
			uint32_t sigoff = le32(base + off + 8), siglen = le32(base + off + 12);
			if ((size_t)sigoff + siglen > len || siglen < 12 || be32(base + sigoff) != CSMAGIC_EMBEDDED) {
				fprintf(out, "%s: no readable code signature\n", path);
				return;
			}
			const uint8_t *sb = base + sigoff;
			uint32_t count = be32(sb + 8), found = 0;
			for (uint32_t j = 0; j < count && 12 + 8 * j + 8 <= siglen; j++) {
				uint32_t boff = be32(sb + 12 + 8 * j + 4);
				if (boff + 8 > siglen) {
					continue;
				}
				uint32_t magic = be32(sb + boff), blen = be32(sb + boff + 4);
				if (magic == CSMAGIC_ENTS && boff + blen <= siglen && blen > 8) {
					fprintf(out, "%s: XML entitlements, %u bytes\n", path, blen - 8);
					fwrite(sb + boff + 8, 1, blen - 8, out);
					fprintf(out, "\n");
					found = 1;
				} else if (magic == CSMAGIC_DERENTS) {
					fprintf(out, "%s: DER entitlements, %u bytes\n", path, blen - 8);
				}
			}
			if (!found) {
				fprintf(out, "%s: no XML entitlements\n", path);
			}
			return;
		}
		off += size;
	}
	fprintf(out, "%s: no LC_CODE_SIGNATURE\n", path);
}

static void
entitlements(const char *path)
{
	int fd = open(path, O_RDONLY);
	struct stat st;

	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 32) {
		fprintf(out, "%s: %s\n", path, fd < 0 ? strerror(errno) : "too small");
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	const uint8_t *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		fprintf(out, "%s: mmap: %s\n", path, strerror(errno));
		return;
	}
	if (be32(p) == FAT_MAGIC_BE) {
		uint32_t n = be32(p + 4);
		for (uint32_t i = 0; i < n && 8 + 20 * i + 20 <= (uint32_t)st.st_size; i++) {
			const uint8_t *a = p + 8 + 20 * i;
			uint32_t off = be32(a + 8), size = be32(a + 12);
			if (be32(a) == CPU_ARM64 && (off_t)off + size <= st.st_size) {
				thin(path, p + off, size);
			}
		}
	} else {
		thin(path, p, (size_t)st.st_size);
	}
	munmap((void *)p, (size_t)st.st_size);
}

static unsigned scanned, matched;

static void
scan(const char *dir, const char *needle, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	size_t nlen = strlen(needle);

	if (d == NULL) {
		fprintf(out, "scan %s: %s\n", dir, strerror(errno));
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[1024];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (lstat(path, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth > 0) {
				scan(path, needle, depth - 1);
			}
			continue;
		}
		if (!S_ISREG(st.st_mode) || st.st_size < 4096 || st.st_size > 96 * 1024 * 1024) {
			continue;
		}
		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			continue;
		}
		const uint8_t *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (p == MAP_FAILED) {
			continue;
		}
		if (le32(p) == MH_MAGIC_64 || be32(p) == FAT_MAGIC_BE) {
			scanned++;
			if (memmem(p, (size_t)st.st_size, needle, nlen) != NULL) {
				fprintf(out, "has %s: %s\n", needle, path);
				matched++;
			}
		}
		munmap((void *)p, (size_t)st.st_size);
	}
	closedir(d);
}

int
main(int argc, char **argv)
{
	const char *outpath = NULL, *needle = NULL;
	int i = 1;

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			outpath = argv[++i];
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			needle = argv[++i];
		} else {
			fprintf(stderr, "usage: sweetents -o OUT [-s STRING] PATH...\n");
			return 2;
		}
	}
	out = outpath ? fopen(outpath, "a") : stdout;
	if (out == NULL) {
		perror("sweetents: output");
		return 1;
	}
	for (; i < argc; i++) {
		if (needle) {
			scan(argv[i], needle, 4);
		} else {
			entitlements(argv[i]);
		}
	}
	if (needle) {
		fprintf(out, "scan for %s: %u Mach-O file(s) looked at, %u hold it\n", needle, scanned, matched);
	}
	if (out != stdout) {
		fclose(out);
	}
	return 0;
}
