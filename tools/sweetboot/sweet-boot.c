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
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>

extern char **environ;

#define DROPBOX "/var/sweet/dropbox.bin"
#define APPLIED "/var/sweet/applied.gen"
#define LIST    "/usr/local/etc/sweet-boot.list"
#define MAXPAYLOAD (100ull << 20)
#define OUTBOX_OFFSET (128ull << 20)
#define OUTBOX_MAX (32ull << 20)
#define PROGRAM_TIMEOUT 420

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


/*
 * The outbox: the second half of the dropbox file. Every few seconds the logs in /var/tmp are copied there,
 * so that they can be read from Android (raw blocks) without a 50 minute image read. Header: "SWOUT001",
 * u64 length, then the text.
 */
static void
write_outbox(void)
{
	static char buf[OUTBOX_MAX];
	size_t n = 16;
	DIR *d = opendir("/var/tmp");
	struct dirent *e;
	int fd;

	if (!d) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[300];
		int in;
		size_t room;
		ssize_t got;

		if (strncmp(e->d_name, "malitest", 8) && strncmp(e->d_name, "gltest", 6) && strncmp(e->d_name, "sweet", 5)) {
			continue;
		}
		snprintf(path, sizeof(path), "/var/tmp/%s", e->d_name);
		in = open(path, O_RDONLY);
		if (in < 0) {
			continue;
		}
		room = sizeof(buf) - n;
		n += (size_t)snprintf(buf + n, room, "\n===== %s =====\n", e->d_name);
		room = sizeof(buf) - n;
		/* the end of a long file is what matters */
		off_t size = lseek(in, 0, SEEK_END);
		off_t start = size > (off_t)(room / 2) ? size - (off_t)(room / 2) : 0;
		got = pread(in, buf + n, room / 2, start);
		if (got > 0) {
			n += (size_t)got;
		}
		close(in);
	}
	closedir(d);
	memcpy(buf, "SWOUT001", 8);
	uint64_t len = n - 16;
	memcpy(buf + 8, &len, 8);
	fd = open(DROPBOX, O_WRONLY);
	if (fd >= 0) {
		size_t total = (n + 4095) & ~(size_t)4095;
		if (total > sizeof(buf)) {
			total = sizeof(buf);
		}
		memset(buf + n, 0, total - n);
		if (pwrite(fd, buf, total, OUTBOX_OFFSET) == (ssize_t)total) {
			fsync(fd);
		}
		close(fd);
	}
}

static void *
outbox_thread(void *arg)
{
	(void)arg;
	for (;;) {
		write_outbox();
		sleep(10);
	}
	return NULL;
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

		int background = 0;
		char *path = line;

		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '&') {           /* "&/path": start it and go on without waiting */
			background = 1;
			path = line + 1;
		}
		if (path[0] != '/') {
			continue;
		}
		argv[0] = path;
		say("SWEETBOOT: starting %s%s\n", path, background ? " (in the background)" : "");
		if (posix_spawn(&pid, path, NULL, NULL, argv, environ) != 0) {
			say("SWEETBOOT: cannot start %s\n", path);
			continue;
		}
		if (background) {
			continue;
		}
		for (int t = 0;; t++) {
			pid_t r = waitpid(pid, &status, WNOHANG);
			if (r == pid) {
				say("SWEETBOOT: %s finished, status %d\n", path, status);
				break;
			}
			if (t >= PROGRAM_TIMEOUT) {
				say("SWEETBOOT: %s did not finish in %d s, killed\n", path, PROGRAM_TIMEOUT);
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				break;
			}
			sleep(1);
		}
	}
	fclose(f);
}

int
main(void)
{
	pthread_t th;

	say("SWEETBOOT: start\n");
	apply_dropbox();
	pthread_create(&th, NULL, outbox_thread, NULL);
	run_list();
	say("SWEETBOOT: done\n");
	write_outbox();
	sleep(100000);          /* keeps the outbox thread going */
	return 0;
}
