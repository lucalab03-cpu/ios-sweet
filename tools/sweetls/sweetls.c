/*
 * sweetls: tell LaunchServices of the system's own bundles and poster extensions, once, and say what it knows
 * (2026-11-26).
 *
 *   sweetls            in the background (sweetroot does not wait): 60 s after start, as root the LaunchServices data
 *                      migrator (once), LSRegisterURL of each poster extension (once), then as mobile
 *                      -[LSApplicationWorkspace _LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:]
 *                      (once), and before and after, the records LaunchServices has of the poster extensions and what
 *                      NSExtension finds for com.apple.posterkit.provider. Log: /var/tmp/sweetls.log. Markers:
 *                      /var/sweet/ls-migrator.done, ls-posters.done, ls-rebuild.done (written only when it went
 *                      through, so an interrupted rebuild starts again at the next boot).
 *
 * Why: PosterBoard asks no poster extension for the wallpaper. On the Pixel 3 port (pixel3-ios docs/LOG.md "migr5",
 * same iOS 20H392) the LaunchServices database had been built on the first boot while the table of extension points
 * could not be read (no AppleMobileFileIntegrity service: our kernel patch 0163), so it held no records of the
 * system's own extensions, PosterBoard found no provider and the wallpaper stayed black. The Galaxy S7 port
 * (wardevsa/ios-a5xelte, herolte, gpu/eagl/test/hero-run.c, MIT) rebuilds it from the phone with these calls; this
 * program does the same. The first rebuild keeps lsd busy for a few minutes (SpringBoard waits for it on the S7).
 *
 * Round as (2026-11-26): round ar's rebuild answered NO at once and the migrator in 0 s: sweetls was signed with no
 * entitlements, and lsd refuses both without com.apple.lsapplicationworkspace.rebuildappdatabases and
 * com.apple.private.coreservices.canmaplsdatabase (the S7's hero-run.entitlements has them; S7: "database rebuilt: 1",
 * 1 -> 515 plug-ins, widgets and wallpapers back). Now signed with sweetls.entitlements; the migrator runs again
 * (new marker ls-migrator2.done); the posters are registered through LSRegisterURL or, when it is not exported,
 * -[LSApplicationWorkspace registerPlugin:]; the entitlements lsd will see are checked first (csops).
 * Round at: registerPlugin: is gone again: it started installd and never came back (round as: Settings then hung on
 * LaunchServices and was killed, 0x8badf00d; the rebuild was never reached). The S7 only ever used LSRegisterURL.
 * A watchdog ends sweetls after 20 minutes, with a line in the log, so nothing waits on it for good.
 * Also: whether ImageIO draws the stock wallpaper's HEIC (the S7 found 0 lit pixels: no HEVC decoder without
 * Apple's hardware), in a child, so the answer is there whatever happens.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define SWEET_CS_OPS_ENTITLEMENTS_BLOB 7

static FILE *log_file;

static void
say(const char *fmt, ...)
{
	va_list ap;

	if (log_file == NULL) {
		return;
	}
	va_start(ap, fmt);
	vfprintf(log_file, fmt, ap);
	va_end(ap);
	fflush(log_file);
}

static id
send0(id target, const char *selector)
{
	return target != NULL ? ((id (*)(id, SEL))objc_msgSend)(target, sel_registerName(selector)) : NULL;
}

static id
nsstring(const char *text)
{
	return (id)CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
}

static const char *
text_of(id object)
{
	const char *s;

	if (object == NULL) {
		return "(nil)";
	}
	s = ((const char *(*)(id, SEL))objc_msgSend)(send0(object, "description"), sel_registerName("UTF8String"));
	return s != NULL ? s : "(?)";
}

static int
once(const char *marker)
{
	return access(marker, F_OK) != 0;
}

static void
mark(const char *marker)
{
	int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

static const char *const posters[] = {
	"/System/Library/PrivateFrameworks/WallpaperKit.framework/PlugIns/CollectionsPoster.appex",
	"/System/Library/PrivateFrameworks/GradientPoster.framework/PlugIns/GradientPosterExtension.appex",
	"/System/Library/PrivateFrameworks/PhotosUIPrivate.framework/PlugIns/PhotosPosterProvider.appex",
	"/System/Library/PrivateFrameworks/EmojiPoster.framework/PlugIns/EmojiPosterExtension.appex",
	"/System/Library/PrivateFrameworks/UnityPoster.framework/PlugIns/UnityPosterExtension.appex",
	"/System/Library/PrivateFrameworks/PridePoster.framework/PlugIns/PridePosterExtension.appex",
	"/Applications/PaperBoard.app/PlugIns/LegacyPoster.appex",
};
static const char *const poster_ids[] = {
	"com.apple.WallpaperKit.CollectionsPoster", "com.apple.GradientPoster.GradientPosterExtension",
	"com.apple.PhotosUIPrivate.PhotosPosterProvider", "com.apple.PaperBoard.LegacyPoster",
};

static void
census(const char *when, id workspace)
{
	Class records = objc_getClass("LSApplicationExtensionRecord");
	id plugins = send0(workspace, "installedPlugins");
	size_t count = plugins != NULL ? ((size_t (*)(id, SEL))objc_msgSend)(plugins, sel_registerName("count")) : 0;

	say("sweetls: %s (uid %d): %zu plug-ins known\n", when, getuid(), count);
	for (size_t i = 0; i < count && i < 3; i++) {
		id plugin = ((id (*)(id, SEL, size_t))objc_msgSend)(plugins, sel_registerName("objectAtIndex:"), i);

		say("sweetls:   e.g. %s\n", text_of(send0(plugin, "pluginIdentifier")));
	}
	for (size_t i = 0; records != NULL && i < sizeof(poster_ids) / sizeof(poster_ids[0]); i++) {
		id error = NULL;
		id record = ((id (*)(id, SEL, id, id *))objc_msgSend)(send0((id)records, "alloc"),
		    sel_registerName("initWithBundleIdentifier:error:"), nsstring(poster_ids[i]), &error);

		say("sweetls:   record %s: %s%s%s\n", poster_ids[i], record != NULL ? "yes" : "NO",
		    error != NULL ? "; " : "", error != NULL ? text_of(error) : "");
	}
	id attributes = ((id (*)(id, SEL, id, id))objc_msgSend)((id)objc_getClass("NSDictionary"),
	    sel_registerName("dictionaryWithObject:forKey:"), nsstring("com.apple.posterkit.provider"),
	    nsstring("NSExtensionPointName"));
	id error = NULL;
	id found = objc_getClass("NSExtension") != NULL ? ((id (*)(id, SEL, id, id *))objc_msgSend)(
	    (id)objc_getClass("NSExtension"), sel_registerName("extensionsWithMatchingAttributes:error:"), attributes,
	    &error) : NULL;
	size_t n = found != NULL ? ((size_t (*)(id, SEL))objc_msgSend)(found, sel_registerName("count")) : 0;

	say("sweetls:   NSExtension for com.apple.posterkit.provider: %zu%s%s\n", n, error != NULL ? "; " : "",
	    error != NULL ? text_of(error) : "");
	for (size_t i = 0; i < n && i < 16; i++) {
		id extension = ((id (*)(id, SEL, size_t))objc_msgSend)(found, sel_registerName("objectAtIndex:"), i);

		say("sweetls:     %s\n", text_of(send0(extension, "identifier")));
	}
}

/* After a rebuild PosterBoard still holds what it found at its start: it is ended, and launchd starts it again. */
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
				say("sweetls: %s[%d] ended: %d\n", name, p->p_pid, kill(p->p_pid, SIGTERM));
			}
		}
	}
	free(procs);
}

/* The entitlements the kernel gives this process (what lsd sees through its audit token). */
static void
own_entitlements(void)
{
	static unsigned char blob[8192];
	int r = csops(getpid(), SWEET_CS_OPS_ENTITLEMENTS_BLOB, blob, sizeof(blob));
	unsigned len = r == 0 ? ((unsigned)blob[4] << 24 | blob[5] << 16 | blob[6] << 8 | blob[7]) : 0;

	if (len > sizeof(blob)) {
		len = sizeof(blob);
	}
	say("sweetls: own entitlements: csops %d, %u bytes, rebuildappdatabases %s, canmaplsdatabase %s\n", r, len,
	    len > 8 && memmem(blob, len, "rebuildappdatabases", 19) != NULL ? "yes" : "NO",
	    len > 8 && memmem(blob, len, "canmaplsdatabase", 16) != NULL ? "yes" : "NO");
}

#define HEIC_TEST "/Library/Wallpaper/Collections/Clownfish~iphone.wallpaperCollection/Wallpapers/" \
	"7420.Clownfish-375w-812h@3x~iphone.wallpaper/7420.Clownfish_Background-375w-812h@3x~iphone.ca/assets/Background.HEIC"

/* ImageIO opens the picture and draws it into memory: how many of its pixels are not black. */
static void
heic_test(const char *path)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
	CGImageSourceRef source = url != NULL ? CGImageSourceCreateWithURL(url, NULL) : NULL;
	CFStringRef type = source != NULL ? CGImageSourceGetType(source) : NULL;
	char type_text[64] = "(none)";
	time_t t = time(NULL);

	if (type != NULL) {
		CFStringGetCString(type, type_text, sizeof(type_text), kCFStringEncodingUTF8);
	}
	CGImageRef image = source != NULL ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
	size_t w = image != NULL ? CGImageGetWidth(image) : 0, h = image != NULL ? CGImageGetHeight(image) : 0;

	say("sweetls: HEIC test: type %s, image %zux%zu\n", type_text, w, h);
	if (image == NULL || w == 0 || h == 0) {
		return;
	}
	unsigned char *pixels = calloc(w * h, 4);
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = pixels != NULL ? CGBitmapContextCreate(pixels, w, h, 8, w * 4, space,
	    kCGImageAlphaPremultipliedLast) : NULL;

	if (context == NULL) {
		say("sweetls: HEIC test: no bitmap\n");
		return;
	}
	CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
	size_t lit = 0;

	for (size_t i = 0; i < w * h; i++) {
		lit += (pixels[i * 4] | pixels[i * 4 + 1] | pixels[i * 4 + 2]) != 0;
	}
	say("sweetls: HEIC test: %zu of %zu pixels lit, %ld s (0 = no HEVC decoder: the pictures must become PNG)\n",
	    lit, w * h, (long)(time(NULL) - t));
}

static void
heic_test_in_child(const char *path)
{
	pid_t child = fork();
	int status = 0;

	if (child == 0) {
		alarm(90);
		heic_test(path);
		_exit(0);
	}
	if (child > 0 && waitpid(child, &status, 0) == child && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
		say("sweetls: HEIC test: the child ended badly (status 0x%x)\n", status);
	}
}

static void
watchdog(int sig)
{
	static const char line[] = "sweetls: still not done after 20 minutes: ended\n";

	(void)sig;
	if (log_file != NULL) {
		write(fileno(log_file), line, sizeof(line) - 1);
	}
	_exit(3);
}

static void
work(void)
{
	time_t start = time(NULL);

	own_entitlements();
	heic_test_in_child(HEIC_TEST);
	signal(SIGALRM, watchdog);
	alarm(1200);
	sleep(60);
	dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
	dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
	id workspace = send0((id)objc_getClass("LSApplicationWorkspace"), "defaultWorkspace");

	census("before", workspace);
	if (once("/var/sweet/ls-migrator2.done")) {
		id bundle = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSBundle"), sel_registerName("bundleWithPath:"),
		    nsstring("/System/Library/DataClassMigrators/00LaunchServicesMigrator.migrator"));
		BOOL loaded = bundle != NULL && ((BOOL (*)(id, SEL))objc_msgSend)(bundle, sel_registerName("load"));
		id cls = loaded ? send0(bundle, "principalClass") : NULL;

		say("sweetls: LaunchServices migrator: bundle %s, loaded %d, class %s\n", bundle ? "found" : "MISSING", loaded,
		    text_of(cls));
		if (cls != NULL) {
			time_t t = time(NULL);
			BOOL done = ((BOOL (*)(id, SEL))objc_msgSend)(send0(send0(cls, "alloc"), "init"),
			    sel_registerName("performMigration"));

			say("sweetls: LaunchServices migrator: performMigration %d, %ld s\n", done, (long)(time(NULL) - t));
			mark("/var/sweet/ls-migrator2.done");
		}
	}
	if (once("/var/sweet/ls-posters.done")) {
		void *cs = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
		int (*lsregister)(id, unsigned char) = (int (*)(id, unsigned char))dlsym(RTLD_DEFAULT, "LSRegisterURL");
		int ran = 0;

		if (lsregister == NULL && cs != NULL) {
			lsregister = (int (*)(id, unsigned char))dlsym(cs, "LSRegisterURL");
		}
		say("sweetls: LSRegisterURL %s\n", lsregister != NULL ? "found" : "not there: left out (the rebuild finds them)");
		for (size_t i = 0; lsregister != NULL && i < sizeof(posters) / sizeof(posters[0]); i++) {
			id url = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSURL"), sel_registerName("fileURLWithPath:"),
			    nsstring(posters[i]));
			time_t t = time(NULL);
			int status = lsregister(url, 1);

			say("sweetls: LSRegisterURL %s: %d, %ld s\n", posters[i], status, (long)(time(NULL) - t));
			ran = 1;
		}
		if (ran) {
			mark("/var/sweet/ls-posters.done");
		}
	}
	if (setgid(501) != 0 || setuid(501) != 0) {
		say("sweetls: cannot become mobile; the rebuild is left out\n");
		return;
	}
	workspace = send0((id)objc_getClass("LSApplicationWorkspace"), "defaultWorkspace");
	if (once("/var/sweet/ls-rebuild.done") && workspace != NULL) {
		time_t t = time(NULL);

		say("sweetls: rebuilding LaunchServices' database for the system's apps (lsd will be busy)\n");
		BOOL rebuilt = ((BOOL (*)(id, SEL, BOOL, BOOL, BOOL))objc_msgSend)(workspace,
		    sel_registerName("_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:"), YES, YES, YES);

		say("sweetls: database rebuilt: %d, %ld s\n", rebuilt, (long)(time(NULL) - t));
		if (rebuilt) {
			/* (mobile cannot write /var/sweet: the marker is left to the parent's next run, see main) */
			int fd = open("/var/tmp/sweetls-rebuilt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

			if (fd >= 0) {
				close(fd);
			}
			sleep(20);
			end_all("PosterBoard");
		}
	}
	census("after", workspace);
	say("sweetls: done after %ld s\n", (long)(time(NULL) - start));
}

int
main(void)
{
	pid_t pid;

	/* a rebuild that went through in an earlier boot becomes the marker here (root) */
	if (access("/var/tmp/sweetls-rebuilt", F_OK) == 0) {
		mark("/var/sweet/ls-rebuild.done");
	}
	pid = fork();
	if (pid != 0) {
		printf("sweetls: working in the background (pid %d), log /var/tmp/sweetls.log\n", pid);
		return pid < 0;
	}
	setsid();
	/* let go of sweetroot's pipe, or it would wait for this child to end */
	int null = open("/dev/null", O_RDWR);

	if (null >= 0) {
		dup2(null, 0);
		dup2(null, 1);
		dup2(null, 2);
		if (null > 2) {
			close(null);
		}
	}
	log_file = fopen("/var/tmp/sweetls.log", "a");
	say("sweetls: start, pid %d\n", getpid());
	work();
	return 0;
}
