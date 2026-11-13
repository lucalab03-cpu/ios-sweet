/* Puts the wrapper in place of backboardd (keeping the real one as backboardd.orig) and restarts backboardd once.
 * Safe to run on every boot: it does nothing when everything is already as it should be. */
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#define CC_SHA1(d, l, o) SHA1(d, l, o)
#define CC_SHA256(d, l, o) SHA256(d, l, o)
#define CC_SHA384(d, l, o) SHA384(d, l, o)
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* libproc.h is not in the iOS SDK; the functions are in libSystem */
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, unsigned buffersize);

#define INJ "/usr/libexec/backboardd.inj"
#define DYLIB "/usr/lib/sweetinject.dylib"
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



static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/*
 * The kernel checks a binary's code signature page by page. Adding a load command changed the first page, so its hash in
 * every CodeDirectory of the signature has to be recomputed (the rest of the signature, entitlements included, stays).
 * fix != 0: rewrite the hashes; returns 0 when all of them match afterwards (or already did), negative on a problem.
 */
static int cs_page0(uint8_t *b, int fix)
{
	uint32_t ncmds, magic; memcpy(&magic, b, 4); memcpy(&ncmds, b + 16, 4);
	uint8_t *p = b + 32, *sig = NULL; uint32_t sigsize = 0;
	for (uint32_t i = 0; i < ncmds; i++) {
		uint32_t cmd, sz; memcpy(&cmd, p, 4); memcpy(&sz, p + 4, 4);
		if (cmd == 0x1d) { uint32_t o, n; memcpy(&o, p + 8, 4); memcpy(&n, p + 12, 4); sig = b + o; sigsize = n; }
		p += sz;
	}
	if (!sig) return 0;                                    /* unsigned: nothing to keep valid */
	if (be32(sig) != 0xfade0cc0u) return -10;
	uint32_t count = be32(sig + 8), bad = 0;
	for (uint32_t k = 0; k < count && 12 + 8 * k + 8 <= sigsize; k++) {
		uint32_t off = be32(sig + 12 + 8 * k + 4);
		uint8_t *cd = sig + off;
		if (off + 44 > sigsize || be32(cd) != 0xfade0c02u) continue;
		uint32_t hoff = be32(cd + 16), limit = be32(cd + 32);
		unsigned hsize = cd[36], htype = cd[37], psz = cd[39];
		uint32_t len = psz ? (1u << psz) : limit;
		uint8_t h[64];
		if (len > limit) len = limit;
		if (htype == 1) CC_SHA1(b, len, h);
		else if (htype == 2 || htype == 3) CC_SHA256(b, len, h);
		else if (htype == 4) CC_SHA384(b, len, h);
		else return -11;
		if (hsize > sizeof(h) || off + hoff + hsize > sigsize) return -12;
		if (memcmp(cd + hoff, h, hsize)) { if (fix) memcpy(cd + hoff, h, hsize); else bad++; }
	}
	return bad ? -13 : 0;
}

/* Adds an LC_LOAD_DYLIB for `dylib` to a 64-bit Mach-O slice, in the free room after the load commands. 0 ok, 1 already
 * there, negative on failure. */
static int patch_slice(uint8_t *b, const char *dylib)
{
	uint32_t magic, ncmds, sizeofcmds, first = 0xffffffffu;
	memcpy(&magic, b, 4); memcpy(&ncmds, b + 16, 4); memcpy(&sizeofcmds, b + 20, 4);
	if (magic != 0xfeedfacfu) return -1;
	uint8_t *p = b + 32;
	for (uint32_t i = 0; i < ncmds; i++) {
		uint32_t cmd, sz; memcpy(&cmd, p, 4); memcpy(&sz, p + 4, 4);
		if (cmd == 0x19) {                                  /* LC_SEGMENT_64: find the first section's file offset */
			uint32_t ns; memcpy(&ns, p + 64, 4);
			for (uint32_t k = 0; k < ns; k++) {
				uint8_t *sec = p + 72 + 80 * k; uint32_t off; uint64_t size;
				memcpy(&off, sec + 48, 4); memcpy(&size, sec + 40, 8);
				if (size && off && off < first) first = off;
			}
		} else if (cmd == 0xc || cmd == 0xd || cmd == 0x18 || cmd == 0x1f) {      /* a dylib command: already ours? */
			uint32_t no; memcpy(&no, p + 8, 4);
			if (!strcmp((char *)p + no, dylib)) return 1;
		}
		p += sz;
	}
	uint32_t need = (24 + (uint32_t)strlen(dylib) + 1 + 7) & ~7u;
	if (first == 0xffffffffu || first < 32 + sizeofcmds || first - (32 + sizeofcmds) < need) return -2;
	uint8_t *w = b + 32 + sizeofcmds;
	memset(w, 0, need);
	uint32_t v[6] = { 0xc, need, 24, 2, 0x10000, 0x10000 };
	memcpy(w, v, 24); memcpy(w + 24, dylib, strlen(dylib));
	ncmds++; sizeofcmds += need;
	memcpy(b + 16, &ncmds, 4); memcpy(b + 20, &sizeofcmds, 4);
	return cs_page0(b, 1);
}

/* Patch a thin or fat Mach-O file in place (every arm64 slice). */
static int patch_file(const char *path, const char *dylib)
{
	long n = fsize(path); int r = -1, any = 0;
	if (n < 64) return -1;
	uint8_t *b = malloc(n); FILE *f = fopen(path, "rb");
	if (!f || fread(b, 1, n, f) != (size_t)n) { if (f) fclose(f); free(b); return -1; }
	fclose(f);
	uint32_t magic; memcpy(&magic, b, 4);
	if (magic == 0xfeedfacfu) { r = patch_slice(b, dylib); any = 1; }
	else if (ntohl(magic) == 0xcafebabeu) {
		uint32_t na; memcpy(&na, b + 4, 4); na = ntohl(na); r = 0;
		for (uint32_t i = 0; i < na; i++) {
			uint32_t cputype, off; memcpy(&cputype, b + 8 + 20 * i, 4); memcpy(&off, b + 8 + 20 * i + 8, 4);
			if (ntohl(cputype) == 0x0100000cu) { int x = patch_slice(b + ntohl(off), dylib); any = 1; if (x < 0) r = x; else if (x == 0 && r == 1) r = 0; }
		}
	}
	if (!any) r = -3;
	if (r == 0) {
		f = fopen(path, "wb");
		if (!f || fwrite(b, 1, n, f) != (size_t)n || fclose(f) != 0) r = -4;
	}
	free(b);
	return r;
}

/* 0 when the first page of every arm64 slice matches its signature. */
static int verify_file(const char *path)
{
	long n = fsize(path); int r = -1;
	if (n < 64) return -1;
	uint8_t *b = malloc(n); FILE *f = fopen(path, "rb");
	if (!f || fread(b, 1, n, f) != (size_t)n) { if (f) fclose(f); free(b); return -1; }
	fclose(f);
	uint32_t magic; memcpy(&magic, b, 4);
	if (magic == 0xfeedfacfu) r = cs_page0(b, 0);
	else if (ntohl(magic) == 0xcafebabeu) {
		uint32_t na; memcpy(&na, b + 4, 4); na = ntohl(na); r = 0;
		for (uint32_t i = 0; i < na; i++) {
			uint32_t cputype, off; memcpy(&cputype, b + 8 + 20 * i, 4); memcpy(&off, b + 8 + 20 * i + 8, 4);
			if (ntohl(cputype) == 0x0100000cu) { int x = cs_page0(b + ntohl(off), 0); if (x) r = x; }
		}
	}
	free(b);
	return r;
}

int main(int argc, char **argv)
{
	int changed = 0;
	if (argc == 3 && !strcmp(argv[1], "--verify")) { int r = verify_file(argv[2]); printf("verify: %d\n", r); return r != 0; }
	if (argc == 4 && !strcmp(argv[1], "--patch")) { int r = patch_file(argv[2], argv[3]); printf("patch: %d\n", r); return r < 0; }
	say("install-wrap: start\n");
	if (access(WRAP, X_OK) != 0) { say("install-wrap: no wrapper at %s\n", WRAP); return 1; }
	if (access(ORIG, F_OK) != 0) {
		say("install-wrap: copying %s to %s\n", BB, ORIG);
		if (copy(BB, ORIG, 0755) != 0) { say("install-wrap: copy of the original failed (%s)\n", strerror(errno)); return 2; }
		changed = 1;
	}
	if (access(INJ, F_OK) == 0 && verify_file(INJ) != 0) { say("install-wrap: %s does not match its signature, rebuilding\n", INJ); unlink(INJ); }
	if (access(INJ, F_OK) != 0 && access(DYLIB, R_OK) == 0) {
		say("install-wrap: making %s\n", INJ);
		if (copy(ORIG, INJ, 0755) == 0) {
			int r = patch_file(INJ, DYLIB);
			say("install-wrap: patch result %d\n", r);
			if (r < 0) unlink(INJ); else changed = 1;
		} else say("install-wrap: copy for the patch failed\n");
	}
	{ int fd = open("/var/tmp/sweetwrap.state", O_WRONLY | O_CREAT, 0666); if (fd >= 0) { fchmod(fd, 0666); close(fd); } }
	if (!same(WRAP, BB)) {
		say("install-wrap: installing the wrapper\n");
		if (copy(WRAP, BB, 0755) != 0) { say("install-wrap: could not install (%s)\n", strerror(errno)); return 3; }
		changed = 1;
	}
	if (changed) {
		int pids[2048], n = proc_listallpids(pids, sizeof(pids));   /* a count of pids */
		for (int i = 0; i < n; i++) {
			char name[64] = "";
			proc_name(pids[i], name, sizeof(name));
			if (!strcmp(name, "backboardd") || !strcmp(name, "backboardd.orig")) { say("install-wrap: restarting pid %d\n", pids[i]); kill(pids[i], SIGKILL); }
		}
	} else say("install-wrap: already installed\n");
	return 0;
}
