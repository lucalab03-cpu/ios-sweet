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
#include <time.h>
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
	FILE *lf = fopen("/var/tmp/sweetboot.log", "a");     /* also in the outbox */
	if (lf) {
		fputs(line, lf);
		fclose(lf);
	}
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
	/*
	 * Written in pieces through a 256 KB buffer (2026-11-25): the whole 32 MB outbox in one static buffer grew with
	 * /var/tmp until jetsam killed sweet-boot for its per-process limit ("CRASH sweet-boot sig 9 ... reason ns 1
	 * code 0x7" in every boot of 2026-11-24 evening), and from then on the outbox was never refreshed. Each file
	 * gives at most its last 1 MB; the header goes last, once the length is known.
	 */
	static char buf[256 << 10];
	const uint64_t per_file = 1u << 20;
	uint64_t pos = 16;                      /* outbox offset of the next text byte, header excluded */
	size_t fill = 0;
	DIR *d = opendir("/var/tmp");
	struct dirent *e;
	int fd = open(DROPBOX, O_WRONLY);

	if (!d || fd < 0) {
		if (d) {
			closedir(d);
		}
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
#define OUT_FLUSH() do { \
		if (fill && pos + fill <= OUTBOX_MAX) { \
			(void)pwrite(fd, buf, fill, OUTBOX_OFFSET + pos); \
			pos += fill; \
		} \
		fill = 0; \
	} while (0)
	while ((e = readdir(d)) != NULL) {
		char path[300];
		int in;

		if (strncmp(e->d_name, "malitest", 8) && strncmp(e->d_name, "gltest", 6) && strncmp(e->d_name, "sweet", 5)) {
			continue;
		}
		snprintf(path, sizeof(path), "/var/tmp/%s", e->d_name);
		in = open(path, O_RDONLY);
		if (in < 0) {
			continue;
		}
		if (sizeof(buf) - fill < 400) {
			OUT_FLUSH();
		}
		fill += (size_t)snprintf(buf + fill, sizeof(buf) - fill, "\n===== %s =====\n", e->d_name);
		off_t size = lseek(in, 0, SEEK_END);
		off_t at = size > (off_t)per_file ? size - (off_t)per_file : 0;
		for (;;) {
			if (fill == sizeof(buf)) {
				OUT_FLUSH();
			}
			ssize_t got = pread(in, buf + fill, sizeof(buf) - fill, at);
			if (got <= 0) {
				break;
			}
			fill += (size_t)got;
			at += got;
		}
		close(in);
		if (pos + fill > OUTBOX_MAX - (64 << 10)) {
			break;                  /* full: what fits */
		}
	}
	closedir(d);
	/*
	 * 2026-11-26: files elsewhere, named one absolute path a line in /var/sweet/outbox.list, give their last 256 KB
	 * the same way (what launchd keeps writing to its log, seen as it is now and not as a copy made at boot).
	 */
	FILE *list = fopen("/var/sweet/outbox.list", "r");
	char want[300];

	while (list != NULL && fgets(want, sizeof(want), list) != NULL) {
		want[strcspn(want, "\r\n")] = 0;
		if (want[0] != '/') {
			continue;
		}
		int in = open(want, O_RDONLY);

		if (in < 0) {
			continue;
		}
		if (sizeof(buf) - fill < 400) {
			OUT_FLUSH();
		}
		off_t size = lseek(in, 0, SEEK_END);
		off_t at = size > (off_t)(256 << 10) ? size - (off_t)(256 << 10) : 0;

		fill += (size_t)snprintf(buf + fill, sizeof(buf) - fill, "\n===== %s (last %lld of %lld bytes) =====\n", want,
		    (long long)(size - at), (long long)size);
		for (;;) {
			if (fill == sizeof(buf)) {
				OUT_FLUSH();
			}
			ssize_t got = pread(in, buf + fill, sizeof(buf) - fill, at);
			if (got <= 0) {
				break;
			}
			fill += (size_t)got;
			at += got;
		}
		close(in);
		if (pos + fill > OUTBOX_MAX - (64 << 10)) {
			break;
		}
	}
	if (list != NULL) {
		fclose(list);
	}
	OUT_FLUSH();
#undef OUT_FLUSH
	/* zero the next block so a shorter outbox does not show an older tail, then the header */
	memset(buf, 0, 4096);
	if (pos + 4096 <= OUTBOX_MAX) {
		(void)pwrite(fd, buf, 4096, OUTBOX_OFFSET + pos);
	}
	char head[16];
	uint64_t len = pos - 16;
	memcpy(head, "SWOUT001", 8);
	memcpy(head + 8, &len, 8);
	(void)pwrite(fd, head, sizeof(head), OUTBOX_OFFSET);
	fsync(fd);
	close(fd);
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

/*
 * The payload is read in pieces of CHUNK bytes, never whole: a whole 60 MB payload in memory got sweet-boot killed
 * by jetsam (per-process limit, boot 2026-11-14 with PanVK's driver in the dropbox).
 */
#define CHUNK (1u << 20)
static uint8_t chunk[CHUNK];

/* What applying the dropbox is doing, for the watchdog in main() (boot 2026-11-14: 35 minutes at "checking"). */
static volatile const char *apply_phase = "starting";
static volatile uint64_t apply_bytes, apply_total;
static volatile int apply_finished;

static int
pread_all(int fd, void *buf, size_t len, uint64_t off)
{
	size_t done = 0;

	while (done < len) {
		time_t t0 = time(NULL);
		ssize_t n = pread(fd, (uint8_t *)buf + done, len - done, (off_t)(off + done));
		if (time(NULL) - t0 >= 5) {
			say("SWEETBOOT: slow read: %zu bytes at %llu took %ld s\n", len - done,
			    (unsigned long long)(off + done), (long)(time(NULL) - t0));
		}
		if (n <= 0) {
			say("SWEETBOOT: read at %llu: %s\n", (unsigned long long)(off + done), n == 0 ? "end of file" : strerror(errno));
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
}

/* Copy `size` bytes at `off` of the dropbox to a new file at `path` (through path.sweettmp). */
static int
install_file(int fd, uint64_t off, uint64_t size, const char *path, uint32_t mode)
{
	char tmp[1040];
	int out;

	snprintf(tmp, sizeof(tmp), "%s.sweettmp", path);
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
	if (out < 0) {
		return -1;
	}
	for (uint64_t done = 0; done < size;) {
		size_t n = size - done > CHUNK ? CHUNK : (size_t)(size - done);
		if (pread_all(fd, chunk, n, off + done) != 0 || write(out, chunk, n) != (ssize_t)n) {
			close(out);
			unlink(tmp);
			return -1;
		}
		done += n;
		apply_bytes = off - 64 + done;  /* progress inside a big file too, for the watchdog */
	}
	if (fsync(out) != 0) {
		close(out);
		unlink(tmp);
		return -1;
	}
	close(out);
	chmod(tmp, mode & 0777);
	return rename(tmp, path);
}

static void
apply_dropbox(void)
{
	uint8_t head[64], digest[32], entry[16];
	uint64_t generation, length, applied = 0, pos = 0;
	CC_SHA256_CTX sha;
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
	/* first the checksum of the whole payload, piece by piece; nothing is installed unless it matches */
	say("SWEETBOOT: dropbox generation %llu, %llu bytes: checking\n", (unsigned long long)generation,
	    (unsigned long long)length);
	CC_SHA256_Init(&sha);
	apply_phase = "checking";
	apply_total = length;
	for (uint64_t done = 0; done < length;) {
		size_t n = length - done > CHUNK ? CHUNK : (size_t)(length - done);
		if (pread_all(fd, chunk, n, 64 + done) != 0) {
			say("SWEETBOOT: cannot read the dropbox\n");
			close(fd);
			return;
		}
		CC_SHA256_Update(&sha, chunk, (CC_LONG)n);
		done += n;
		apply_bytes = done;
	}
	apply_phase = "installing";
	apply_bytes = 0;
	CC_SHA256_Final(digest, &sha);
	say("SWEETBOOT: checksum %s\n", memcmp(digest, head + 24, 32) == 0 ? "ok" : "wrong");
	if (memcmp(digest, head + 24, 32) != 0) {
		say("SWEETBOOT: dropbox checksum does not match, not applied\n");
		close(fd);
		return;
	}
	while (pos + 16 <= length) {
		uint32_t plen, mode;
		uint64_t size;
		char path[1024];

		if (pread_all(fd, entry, 16, 64 + pos) != 0) {
			say("SWEETBOOT: cannot read entry at %llu\n", (unsigned long long)pos);
			bad++;
			break;
		}
		plen = rd32(entry);
		mode = rd32(entry + 4);
		size = rd64(entry + 8);
		pos += 16;
		if (plen == 0 || plen >= sizeof(path) || pos + plen + size > length ||
		    pread_all(fd, path, plen, 64 + pos) != 0) {
			say("SWEETBOOT: damaged entry at %llu\n", (unsigned long long)pos);
			bad++;
			break;
		}
		path[plen] = 0;
		pos += plen;
		if (path[0] != '/' || strstr(path, "..") != NULL) {
			say("SWEETBOOT: refused path %s\n", path);
			bad++;
			pos += size;
			continue;
		}
		make_parents(path);
		apply_bytes = pos;
		if (install_file(fd, 64 + pos, size, path, mode) != 0) {
			say("SWEETBOOT: cannot install %s: %s\n", path, strerror(errno));
			bad++;
		} else {
			files++;
			say("SWEETBOOT: installed %s (%llu bytes)\n", path, (unsigned long long)size);
		}
		pos += size;
	}
	close(fd);
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

static void *
apply_thread(void *arg)
{
	(void)arg;
	apply_dropbox();
	apply_finished = 1;
	return NULL;
}

/*
 * The programs of the list wait for the dropbox as long as it makes progress: they start anyway after APPLY_STALL
 * seconds without a byte more, or after APPLY_DEADLINE seconds in all. (180 s flat was too short for a 40 MB drop at
 * 6-7 s per MB: the tests ran with the old files.)
 */
#define APPLY_STALL 60
#define APPLY_DEADLINE 1200

int
main(void)
{
	pthread_t th, ap;
	uint64_t last = (uint64_t)-1;

	say("SWEETBOOT: start\n");
	/*
	 * Reading the dropbox can be very slow or stall: boot 2026-11-14 spent about 3.5 minutes on an 18 MB drop, and
	 * the next boot stayed at "checking" for 35 minutes, so no program of the list ever started (no sweetaccel-d:
	 * backboardd crash loop; no tests; the outbox never refreshed). The dropbox is applied in its own thread; every
	 * 10 s this one logs where it is, and after APPLY_DEADLINE seconds the list starts anyway with the files that
	 * are there (a drop that finishes later takes effect for the programs started after it). The outbox thread
	 * waits for the dropbox too, but not beyond the deadline: the logs matter more than a clean first read.
	 */
	pthread_create(&ap, NULL, apply_thread, NULL);
	int stalled = 0;
	const char *seen_phase = NULL;
	for (int t = 0; !apply_finished && t < APPLY_DEADLINE && stalled < APPLY_STALL; t++) {
		sleep(1);
		uint64_t now = apply_bytes;
		const char *phase = (const char *)apply_phase;
		if (now != last || phase != seen_phase) {
			stalled = 0;
			last = now;
			seen_phase = phase;
		} else {
			stalled++;
		}
		if (t % 10 == 9 && !apply_finished) {
			say("SWEETBOOT: still %s after %d s: %llu of %llu bytes%s\n", phase, t + 1, (unsigned long long)now,
			    (unsigned long long)apply_total, stalled >= 10 ? " (no progress)" : "");
		}
	}
	if (!apply_finished) {
		say("SWEETBOOT: the dropbox is not applied (%s): starting the programs anyway\n",
		    stalled >= APPLY_STALL ? "no progress for a minute" : "20 minutes passed");
	}
	pthread_create(&th, NULL, outbox_thread, NULL);
	run_list();
	say("SWEETBOOT: done\n");
	write_outbox();
	sleep(100000);          /* keeps the outbox thread going */
	return 0;
}
