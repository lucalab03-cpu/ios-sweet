/*
 * sweetheic: the stock wallpapers' HEIC pictures made PNG on the phone itself (2026-11-27).
 *
 *   sweetheic convert DIR TEXT   every *.HEIC under DIR whose path contains TEXT: decoded with libheif (+ libde265,
 *                                software HEVC), its width padded to a multiple of 8 pixels by repeating the last column,
 *                                written as PNG (ImageIO) under the same name. The original is kept first under
 *                                /var/sweet/heic-orig/ + its path. A file that already starts with PNG's bytes is left
 *                                alone, so running it again changes nothing.
 *   sweetheic convert-bg DIR TEXT  the same in the background (sweetroot does not wait), then ImageIO is asked to draw
 *                                the first converted picture (the check that gave 0 lit pixels before), and PosterBoard
 *                                and CollectionsPoster are ended so that they draw again with the PNGs.
 *   sweetheic test FILE          what libheif makes of FILE: size, alpha, lit pixels.
 *   Log: /var/tmp/sweetheic.log (and stdout).
 *
 * Why: this phone has no Apple video decoder (AppleAVD), and ImageIO draws the stock wallpapers' HEIC pictures as all
 * black: sweetls round as, Clownfish Background.HEIC 1380x1812, 0 of 2500560 pixels lit. The Galaxy S7 port
 * (wardevsa/ios-a5xelte, tools/herolte-wallpaper-png.sh, MIT) found the same and converts them with macOS's sips when
 * it builds its root; ImageIO knows a PNG by its first bytes, and the .ca packages name their assets by file name, so
 * the name stays. Widths padded to x8 as the S7 learned: CoreAnimation reads rows rounded to 32 bytes, and a
 * 1105-pixel picture with tight rows took backboardd down (S7 tools/herolte-png-pad8.py). Our repository is public,
 * so Apple's pictures cannot be converted in CI: the phone converts its own files.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <libheif/heif.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/sysctl.h>

#define ORIG_DIR "/var/sweet/heic-orig"

static FILE *log_file;
static int made, kept, failed;
static char first_made[1024];

static void
say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	if (log_file != NULL) {
		va_start(ap, fmt);
		vfprintf(log_file, fmt, ap);
		va_end(ap);
		fflush(log_file);
	}
}

static long
ms_now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static int
mkdirs(const char *path)
{
	char tmp[1024];

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int
copy_file(const char *from, const char *to)
{
	char buf[65536];
	int in = open(from, O_RDONLY), out;
	ssize_t n;

	if (in < 0) {
		return -1;
	}
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			n = -1;
			break;
		}
	}
	fsync(out);
	close(in);
	close(out);
	return n < 0 ? -1 : 0;
}

static int
is_png(const char *path)
{
	unsigned char head[8];
	int fd = open(path, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, head, sizeof(head)) : -1;

	if (fd >= 0) {
		close(fd);
	}
	return n == 8 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0;
}

/* Decoded RGBA, width padded to a multiple of 8 (last column repeated). Caller frees. */
static unsigned char *
decode(const char *path, int *out_w, int *out_h, int *padded_w, int *alpha, CFDataRef *icc)
{
	struct heif_context *ctx = heif_context_alloc();
	struct heif_image_handle *handle = NULL;
	struct heif_image *image = NULL;
	struct heif_error err = heif_context_read_from_file(ctx, path, NULL);
	unsigned char *pixels = NULL;

	*icc = NULL;
	if (err.code != heif_error_Ok) {
		say("  %s: libheif cannot read it: %s\n", path, err.message);
		goto out;
	}
	err = heif_context_get_primary_image_handle(ctx, &handle);
	if (err.code != heif_error_Ok) {
		say("  %s: no primary image: %s\n", path, err.message);
		goto out;
	}
	*alpha = heif_image_handle_has_alpha_channel(handle);
	err = heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, NULL);
	if (err.code != heif_error_Ok) {
		say("  %s: not decoded: %s\n", path, err.message);
		goto out;
	}
	int w = heif_image_get_width(image, heif_channel_interleaved);
	int h = heif_image_get_height(image, heif_channel_interleaved);
	int stride = 0;
	const uint8_t *plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);

	if (plane == NULL || w <= 0 || h <= 0) {
		say("  %s: no pixels\n", path);
		goto out;
	}
	int pw = (w + 7) & ~7;

	pixels = malloc((size_t)pw * h * 4);
	if (pixels == NULL) {
		goto out;
	}
	for (int y = 0; y < h; y++) {
		unsigned char *row = pixels + (size_t)y * pw * 4;

		memcpy(row, plane + (size_t)y * stride, (size_t)w * 4);
		for (int x = w; x < pw; x++) {
			memcpy(row + x * 4, row + (w - 1) * 4, 4);
		}
	}
	size_t icc_size = heif_image_handle_get_raw_color_profile_size(handle);

	if (icc_size > 0 && heif_image_handle_get_color_profile_type(handle) == heif_color_profile_type_prof) {
		void *data = malloc(icc_size);

		if (data != NULL && heif_image_handle_get_raw_color_profile(handle, data).code == heif_error_Ok) {
			*icc = CFDataCreate(NULL, data, (CFIndex)icc_size);
		}
		free(data);
	}
	*out_w = w;
	*out_h = h;
	*padded_w = pw;
out:
	if (image != NULL) {
		heif_image_release(image);
	}
	if (handle != NULL) {
		heif_image_handle_release(handle);
	}
	heif_context_free(ctx);
	return pixels;
}

static int
write_png(const char *path, unsigned char *pixels, int w, int h, int alpha, CFDataRef icc)
{
	CGColorSpaceRef space = icc != NULL ? CGColorSpaceCreateWithICCData(icc) : NULL;

	if (space == NULL) {
		space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	}
	CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, (size_t)w * h * 4, NULL);
	CGImageRef image = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
	    (alpha ? kCGImageAlphaLast : kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrderDefault, provider, NULL, false,
	    kCGRenderingIntentDefault);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
	CGImageDestinationRef dest = url != NULL ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
	int ok = 0;

	if (image != NULL && dest != NULL) {
		CGImageDestinationAddImage(dest, image, NULL);
		ok = CGImageDestinationFinalize(dest);
	}
	if (dest != NULL) {
		CFRelease(dest);
	}
	if (url != NULL) {
		CFRelease(url);
	}
	if (image != NULL) {
		CGImageRelease(image);
	}
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(space);
	return ok;
}

static void
convert_one(const char *path)
{
	char orig[1200], tmp[1200];
	long t = ms_now();
	int w = 0, h = 0, pw = 0, alpha = 0;
	CFDataRef icc = NULL;

	if (is_png(path)) {
		kept++;
		return;
	}
	unsigned char *pixels = decode(path, &w, &h, &pw, &alpha, &icc);

	if (pixels == NULL) {
		failed++;
		return;
	}
	size_t lit = 0;

	for (size_t i = 0; i < (size_t)pw * h; i++) {
		lit += (pixels[i * 4] | pixels[i * 4 + 1] | pixels[i * 4 + 2]) != 0;
	}
	snprintf(orig, sizeof(orig), "%s%s", ORIG_DIR, path);
	char *slash = strrchr(orig, '/');

	*slash = 0;
	mkdirs(orig);
	*slash = '/';
	if (access(orig, F_OK) != 0 && copy_file(path, orig) != 0) {
		say("  %s: original not kept (%s): left alone\n", path, strerror(errno));
		free(pixels);
		failed++;
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.sweetpng", path);
	if (!write_png(tmp, pixels, pw, h, alpha, icc) || rename(tmp, path) != 0) {
		say("  %s: PNG not written\n", path);
		unlink(tmp);
		failed++;
	} else {
		struct stat st;

		stat(path, &st);
		say("  %s: %dx%d -> %dx%d PNG%s%s, %zu of %zu pixels lit, %lld bytes, %ld ms\n", path, w, h, pw, h,
		    alpha ? " with alpha" : "", icc != NULL ? ", ICC kept" : "", lit, (size_t)pw * h, (long long)st.st_size,
		    ms_now() - t);
		made++;
		if (first_made[0] == 0) {
			snprintf(first_made, sizeof(first_made), "%s", path);
		}
	}
	if (icc != NULL) {
		CFRelease(icc);
	}
	free(pixels);
}

static void
walk(const char *dir, const char *text)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (d == NULL) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[1024];

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (e->d_type == DT_DIR) {
			walk(path, text);
			continue;
		}
		size_t n = strlen(e->d_name);

		if (n > 5 && strcasecmp(e->d_name + n - 5, ".heic") == 0 && strstr(path, text) != NULL) {
			convert_one(path);
		}
	}
	closedir(d);
}

/* What ImageIO (as the wallpaper's poster uses it) makes of a file: its type and how many pixels are lit. */
static void
imageio_check(const char *path)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
	CGImageSourceRef source = url != NULL ? CGImageSourceCreateWithURL(url, NULL) : NULL;
	CFStringRef type = source != NULL ? CGImageSourceGetType(source) : NULL;
	char type_text[64] = "(none)";

	if (type != NULL) {
		CFStringGetCString(type, type_text, sizeof(type_text), kCFStringEncodingUTF8);
	}
	CGImageRef image = source != NULL ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
	size_t w = image != NULL ? CGImageGetWidth(image) : 0, h = image != NULL ? CGImageGetHeight(image) : 0;
	unsigned char *pixels = w && h ? calloc(w * h, 4) : NULL;
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = pixels != NULL ? CGBitmapContextCreate(pixels, w, h, 8, w * 4, space,
	    kCGImageAlphaPremultipliedLast) : NULL;
	size_t lit = 0;

	if (context != NULL) {
		CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
		for (size_t i = 0; i < w * h; i++) {
			lit += (pixels[i * 4] | pixels[i * 4 + 1] | pixels[i * 4 + 2]) != 0;
		}
	}
	say("sweetheic: ImageIO check %s: type %s, %zux%zu, %zu of %zu pixels lit\n", path, type_text, w, h, lit, w * h);
}

static void
end_all(const char *name)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	struct kinfo_proc *procs = NULL;
	size_t size = 0;

	if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || (procs = malloc(size + 16 * sizeof(*procs))) == NULL) {
		return;
	}
	size += 16 * sizeof(*procs);
	if (sysctl(mib, 4, procs, &size, NULL, 0) == 0) {
		for (size_t i = 0; i < size / sizeof(*procs); i++) {
			struct extern_proc *p = &procs[i].kp_proc;

			if (strncmp(p->p_comm, name, sizeof(p->p_comm)) == 0) {
				say("sweetheic: %s[%d] ended: %d\n", name, p->p_pid, kill(p->p_pid, SIGTERM));
			}
		}
	}
	free(procs);
}

int
main(int argc, char **argv)
{
	log_file = fopen("/var/tmp/sweetheic.log", "a");
	int background = argc == 4 && strcmp(argv[1], "convert-bg") == 0;

	if (background) {
		pid_t pid = fork();

		if (pid != 0) {
			printf("sweetheic: converting in the background (pid %d), log /var/tmp/sweetheic.log\n", pid);
			return pid < 0;
		}
		setsid();
		int null = open("/dev/null", O_RDWR);

		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
			if (null > 2) {
				close(null);
			}
		}
	}
	if (argc == 4 && (strcmp(argv[1], "convert") == 0 || background)) {
		long t = ms_now();

		say("sweetheic: libheif %s; converting the HEIC under %s with \"%s\" in the path\n", heif_get_version(),
		    argv[2], argv[3]);
		walk(argv[2], argv[3]);
		say("sweetheic: %d made PNG, %d already PNG, %d failed, %ld s\n", made, kept, failed, (ms_now() - t) / 1000);
		if (first_made[0] != 0) {
			imageio_check(first_made);
		}
		if (background && made > 0) {
			end_all("PosterBoard");
			end_all("CollectionsPoster");
		}
		return failed != 0;
	}
	if (argc == 3 && strcmp(argv[1], "test") == 0) {
		int w = 0, h = 0, pw = 0, alpha = 0;
		CFDataRef icc = NULL;
		long t = ms_now();
		unsigned char *pixels = decode(argv[2], &w, &h, &pw, &alpha, &icc);
		size_t lit = 0;

		for (size_t i = 0; pixels != NULL && i < (size_t)pw * h; i++) {
			lit += (pixels[i * 4] | pixels[i * 4 + 1] | pixels[i * 4 + 2]) != 0;
		}
		say("sweetheic test %s: %dx%d, alpha %d, ICC %s, %zu of %zu pixels lit, %ld ms\n", argv[2], w, h, alpha,
		    icc != NULL ? "yes" : "no", lit, (size_t)pw * h, ms_now() - t);
		return pixels == NULL;
	}
	fprintf(stderr, "usage: sweetheic convert[-bg] DIR TEXT | sweetheic test FILE\n");
	return 2;
}
