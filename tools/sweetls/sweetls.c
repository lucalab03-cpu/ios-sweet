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

static void
work(void)
{
	time_t start = time(NULL);

	sleep(60);
	dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
	dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
	id workspace = send0((id)objc_getClass("LSApplicationWorkspace"), "defaultWorkspace");

	census("before", workspace);
	if (once("/var/sweet/ls-migrator.done")) {
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
			mark("/var/sweet/ls-migrator.done");
		}
	}
	if (once("/var/sweet/ls-posters.done")) {
		int (*lsregister)(id, unsigned char) = (int (*)(id, unsigned char))dlsym(RTLD_DEFAULT, "LSRegisterURL");

		for (size_t i = 0; lsregister != NULL && i < sizeof(posters) / sizeof(posters[0]); i++) {
			id url = ((id (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSURL"), sel_registerName("fileURLWithPath:"),
			    nsstring(posters[i]));
			time_t t = time(NULL);
			int status = lsregister(url, 1);

			say("sweetls: LSRegisterURL %s: %d, %ld s\n", posters[i], status, (long)(time(NULL) - t));
		}
		say("sweetls: LSRegisterURL %s\n", lsregister != NULL ? "done" : "not there");
		if (lsregister != NULL) {
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
