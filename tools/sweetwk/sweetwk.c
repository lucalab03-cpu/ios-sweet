/*
 * sweetwk: which preference WallpaperKit reads to decide whether a wallpaper made for another screen may be used,
 * and set it (2026-11-26).
 *
 *   sweetwk [set]      logs to stdout and /var/tmp/sweetwk.log
 *
 * Why: iOS 16's wallpapers exist on this image for three screen classes only, all @3x (375w-812h, 393w-852h,
 * 430w-932h: sweetfind in round al), and this screen is 414 x 896 points @2x. WallpaperKit then finds "no compatible
 * wallpaper on disk" and the poster is its gradient over black (the lock screen's red-to-black, the home screen's
 * black). The MT6768 port of github.com/nicolascuaces/hakios-xnu-kernel (docs/x6826b-wallpaper.md, run 256) makes
 * -[NSUserDefaults wk_ignoreLogicalScreenClassForWallpaperBundle] answer YES inside SpringBoard, PosterBoard and
 * CollectionsPoster, by replacing the method in each. A method of NSUserDefaults most likely reads a preference:
 * this program loads WallpaperKit, watches the NSUserDefaults getters while it calls that method once, and logs the
 * keys and the defaults object asked. With "set", a key read during that call whose name speaks of the screen or of
 * ignoring is set to true for root and mobile, in the global domain and WallpaperKit's/PosterBoard's/SpringBoard's,
 * and a second run of this program (a new process: the method may keep its first answer) says what it answers now.
 * Nothing of WallpaperKit or of the wallpapers is changed. Run by sweetroot (root.cmd).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static FILE *log_file;

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

static id
msg_id(id target, const char *selector)
{
	return ((id (*)(id, SEL))objc_msgSend)(target, sel_registerName(selector));
}

static const char *
text_of(id object)
{
	const char *s;

	if (object == NULL) {
		return "(nil)";
	}
	s = ((const char *(*)(id, SEL))objc_msgSend)(msg_id(object, "description"), sel_registerName("UTF8String"));
	return s != NULL ? s : "(?)";
}

static bool
responds(id object, const char *selector)
{
	return object != NULL &&
	    ((BOOL (*)(id, SEL, SEL))objc_msgSend)(object, sel_registerName("respondsToSelector:"), sel_registerName(selector));
}

/* keys read while the method runs */
#define KEYS 32
static char keys[KEYS][160];
static int nkeys;
static bool watching;
static id standard;

static void
note(const char *getter, id self, id key)
{
	char name[160] = "(nil)";

	if (key != NULL) {
		snprintf(name, sizeof(name), "%s", text_of(key));
	}
	if (!watching) {
		return;
	}
	say("sweetwk:   -[NSUserDefaults %s \"%s\"] on %p%s", getter, name, (void *)self, self == standard ? " (standardUserDefaults)" : "");
	if (responds(self, "_identifier")) {
		say(", identifier %s", text_of(msg_id(self, "_identifier")));
	}
	if (responds(self, "suiteName")) {
		say(", suite %s", text_of(msg_id(self, "suiteName")));
	}
	say("\n");
	for (int i = 0; i < nkeys; i++) {
		if (strcmp(keys[i], name) == 0) {
			return;
		}
	}
	if (nkeys < KEYS) {
		snprintf(keys[nkeys++], sizeof(keys[0]), "%s", name);
	}
}

static IMP orig_bool, orig_object, orig_integer, orig_string, orig_double;

static BOOL
watch_bool(id self, SEL sel, id key)
{
	note("boolForKey:", self, key);
	return ((BOOL (*)(id, SEL, id))orig_bool)(self, sel, key);
}

static id
watch_object(id self, SEL sel, id key)
{
	note("objectForKey:", self, key);
	return ((id (*)(id, SEL, id))orig_object)(self, sel, key);
}

static long
watch_integer(id self, SEL sel, id key)
{
	note("integerForKey:", self, key);
	return ((long (*)(id, SEL, id))orig_integer)(self, sel, key);
}

static id
watch_string(id self, SEL sel, id key)
{
	note("stringForKey:", self, key);
	return ((id (*)(id, SEL, id))orig_string)(self, sel, key);
}

static double
watch_double(id self, SEL sel, id key)
{
	note("doubleForKey:", self, key);
	return ((double (*)(id, SEL, id))orig_double)(self, sel, key);
}

static IMP
replace(Class cls, const char *selector, IMP with)
{
	Method m = class_getInstanceMethod(cls, sel_registerName(selector));

	return m != NULL ? method_setImplementation(m, with) : NULL;
}

/* what the method answers in this process; -1 when it is not there */
static int
ask(void)
{
	Class cls = objc_getClass("NSUserDefaults");
	SEL wk = sel_registerName("wk_ignoreLogicalScreenClassForWallpaperBundle");

	standard = msg_id((id)cls, "standardUserDefaults");
	if (class_getInstanceMethod(cls, wk) != NULL) {
		return ((BOOL (*)(id, SEL))objc_msgSend)(standard, wk) ? 1 : 0;
	}
	if (class_getClassMethod(cls, wk) != NULL) {
		say("sweetwk: (a class method)\n");
		return ((BOOL (*)(id, SEL))objc_msgSend)((id)cls, wk) ? 1 : 0;
	}
	return -1;
}

static bool
wanted(const char *key)
{
	return strcasestr(key, "screen") != NULL || strcasestr(key, "ignore") != NULL;
}

static void
set_key(const char *key)
{
	CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFStringRef domains[] = { kCFPreferencesAnyApplication, CFSTR("com.apple.WallpaperKit"), CFSTR("com.apple.wallpaperkit"),
		CFSTR("com.apple.PosterBoard"), CFSTR("com.apple.springboard"), CFSTR("com.apple.WallpaperKit.CollectionsPoster") };
	CFStringRef users[] = { CFSTR("mobile"), kCFPreferencesCurrentUser };

	for (unsigned u = 0; u < 2; u++) {
		for (unsigned d = 0; d < sizeof(domains) / sizeof(domains[0]); d++) {
			CFPreferencesSetValue(name, kCFBooleanTrue, domains[d], users[u], kCFPreferencesAnyHost);
			CFPreferencesSynchronize(domains[d], users[u], kCFPreferencesAnyHost);
		}
	}
	say("sweetwk: \"%s\" set to true (mobile and root; global, WallpaperKit, PosterBoard, SpringBoard, CollectionsPoster)\n", key);
	CFRelease(name);
}

int
main(int argc, char **argv)
{
	bool set = argc > 1 && strcmp(argv[1], "set") == 0;
	bool check = argc > 1 && strcmp(argv[1], "check") == 0;
	Class cls;
	int answer;

	log_file = fopen("/var/tmp/sweetwk.log", "a");
	if (dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW) == NULL ||
	    dlopen("/System/Library/PrivateFrameworks/WallpaperKit.framework/WallpaperKit", RTLD_NOW) == NULL) {
		say("sweetwk: cannot load WallpaperKit: %s\n", dlerror());
		return 1;
	}
	cls = objc_getClass("NSUserDefaults");
	if (check) {
		answer = ask();
		say("sweetwk: check (pid %d, uid %d): wk_ignoreLogicalScreenClassForWallpaperBundle answers %d\n", getpid(), getuid(), answer);
		return 0;
	}
	orig_bool = replace(cls, "boolForKey:", (IMP)watch_bool);
	orig_object = replace(cls, "objectForKey:", (IMP)watch_object);
	orig_integer = replace(cls, "integerForKey:", (IMP)watch_integer);
	orig_string = replace(cls, "stringForKey:", (IMP)watch_string);
	orig_double = replace(cls, "doubleForKey:", (IMP)watch_double);
	say("sweetwk: start (pid %d, uid %d); NSUserDefaults getters watched while the method runs\n", getpid(), getuid());
	watching = true;
	answer = ask();
	watching = false;
	say("sweetwk: wk_ignoreLogicalScreenClassForWallpaperBundle answers %d (-1: no such method); %d key(s) read\n", answer,
	    nkeys);
	if (!set || answer != 0) {
		return 0;
	}
	int done = 0;

	for (int i = 0; i < nkeys; i++) {
		if (wanted(keys[i])) {
			set_key(keys[i]);
			done++;
		} else {
			say("sweetwk: \"%s\" left alone (not about the screen)\n", keys[i]);
		}
	}
	if (done == 0) {
		say("sweetwk: nothing set\n");
		return 0;
	}
	char *child[] = { argv[0], "check", NULL };
	pid_t pid;
	int status = 0;

	if (posix_spawn(&pid, argv[0], NULL, NULL, child, environ) == 0) {
		waitpid(pid, &status, 0);
	}
	return 0;
}
