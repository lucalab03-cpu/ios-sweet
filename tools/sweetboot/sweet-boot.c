/*
 * sweet-boot: the first program of every boot (a launchd job).
 *
 * 1. Applies the "dropbox": /var/sweet/dropbox.bin is a file that was made big and empty when the root image
 *    was written; new files are put into its blocks from the outside (Android side, raw writes, seconds
 *    instead of a 50 minute image write). If it holds a package newer than the one applied last time, its
 *    files are installed.
 * 2. Starts the programs listed in /usr/local/etc/sweet-boot.list, one after the other.
 *
 * Package format (all little endian): 64 byte header { "SWDROP01", u64 generation, u64 payload length,
 * u8 sha256[32], 8 bytes zero }, then the payload: entries { u32 path length, u32 mode, u64 size, path, data }.
 */
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define DROPBOX "/var/sweet/dropbox.bin"
#define APPLIED "/var/sweet/applied.gen"
#define LIST    "/usr/local/etc/sweet-boot.list"
#define MAXPAYLOAD (240ull << 20)

static void
say(const char *fmt, ...)
{
	char line[400];
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	__builtin_va_end(ap);
	fputs(line, stdout);
	fflush(stdout);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int
make_parents(char *path)
{
	for (char *s = path + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			if (mkdir(path, 0755) != 0 && errno != EEXIST) {
				*s = '/';
				return -1;
			}
			*s = '/';
		}
	}
	return 0;
}

static int
read_all(int fd, uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = read(fd, buf + done, len - done);
		if (n <= 0) {
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
}

static void
apply_dropbox(void)
{
	uint8_t head[64], digest[32];
	uint64_t generation, length, applied = 0, pos = 0;
	uint8_t *payload;
	int fd, files = 0, bad = 0;
	FILE *f;

	mkdir("/var/sweet", 0755);
	fd = open(DROPBOX, O_RDONLY);
	if (fd < 0) {
		say("SWEETBOOT: no dropbox (%s)\n", strerror(errno));
		return;
	}
	if (read_all(fd, head, sizeof(head)) != 0 || memcmp(head, "SWDROP01", 8) != 0) {
		say("SWEETBOOT: dropbox is empty\n");
		close(fd);
		return;
	}
	generation = rd64(head + 8);
	length = rd64(head + 16);
	f = fopen(APPLIED, "r");
	if (f) {
		unsigned long long a = 0;
		if (fscanf(f, "%llu", &a) == 1) {
			applied = a;
		}
		fclose(f);
	}
	if (generation <= applied) {
		say("SWEETBOOT: dropbox generation %llu already applied\n", (unsigned long long)generation);
		close(fd);
		return;
	}
	if (length == 0 || length > MAXPAYLOAD) {
		say("SWEETBOOT: dropbox length %llu is not valid\n", (unsigned long long)length);
		close(fd);
		return;
	}
	payload = malloc(length);
	if (!payload || read_all(fd, payload, length) != 0) {
		say("SWEETBOOT: cannot read the dropbox\n");
		close(fd);
		return;
	}
	close(fd);
	CC_SHA256(payload, (CC_LONG)length, digest);
	if (memcmp(digest, head + 24, 32) != 0) {
		say("SWEETBOOT: dropbox checksum does not match, not applied\n");
		free(payload);
		return;
	}
	while (pos + 16 <= length) {
		uint32_t plen = rd32(payload + pos), mode = rd32(payload + pos + 4);
		uint64_t size = rd64(payload + pos + 8);
		char path[1024], tmp[1040];
		int out;

		pos += 16;
		if (plen == 0 || plen >= sizeof(path) || pos + plen + size > length) {
			say("SWEETBOOT: damaged entry at %llu\n", (unsigned long long)pos);
			bad++;
			break;
		}
		memcpy(path, payload + pos, plen);
		path[plen] = 0;
		pos += plen;
		if (path[0] != '/' || strstr(path, "..") != NULL) {
			say("SWEETBOOT: refused path %s\n", path);
			bad++;
			pos += size;
			continue;
		}
		snprintf(tmp, sizeof(tmp), "%s.sweettmp", path);
		make_parents(path);
		out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
		if (out < 0 || write(out, payload + pos, size) != (ssize_t)size || fsync(out) != 0) {
			say("SWEETBOOT: cannot write %s: %s\n", path, strerror(errno));
			bad++;
			if (out >= 0) {
				close(out);
			}
			pos += size;
			continue;
		}
		close(out);
		chmod(tmp, mode & 0777);
		if (rename(tmp, path) != 0) {
			say("SWEETBOOT: cannot rename to %s: %s\n", path, strerror(errno));
			bad++;
		} else {
			files++;
		}
		pos += size;
	}
	free(payload);
	say("SWEETBOOT: dropbox generation %llu: %d file(s) installed, %d problem(s)\n",
	    (unsigned long long)generation, files, bad);
	if (bad == 0) {
		f = fopen(APPLIED, "w");
		if (f) {
			fprintf(f, "%llu\n", (unsigned long long)generation);
			fclose(f);
		}
		sync();
	}
}

static void
run_list(void)
{
	FILE *f = fopen(LIST, "r");
	char line[512];

	if (!f) {
		say("SWEETBOOT: no list of programs to start\n");
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char *argv[2] = { line, NULL };
		pid_t pid;
		int status = 0;

		line[strcspn(line, "\r\n")] = 0;
		if (line[0] != '/') {
			continue;
		}
		say("SWEETBOOT: starting %s\n", line);
		if (posix_spawn(&pid, line, NULL, NULL, argv, environ) != 0) {
			say("SWEETBOOT: cannot start %s\n", line);
			continue;
		}
		waitpid(pid, &status, 0);
		say("SWEETBOOT: %s finished, status %d\n", line, status);
	}
	fclose(f);
}

int
main(void)
{
	say("SWEETBOOT: start\n");
	apply_dropbox();
	run_list();
	say("SWEETBOOT: done\n");
	return 0;
}
