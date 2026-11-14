/*
 * sweetsvc: keep launchd from starting daemons that only crash or exit in a loop on this phone.
 *
 * Boot 2026-11-14: bluetoothd and fairplayd.H2 were each started 335 times (no Bluetooth hardware they can use yet),
 * with 900 xpcproxy execs: CPU and battery for nothing. launchd honours /private/var/db/com.apple.xpc.launchd/
 * disabled.plist (a dictionary label -> true, what "launchctl disable system/LABEL" writes) from the next boot.
 *
 * /var/sweet/svc-disable lists program names (basenames, one per line; "#" comments). The labels are looked up in
 * launchd's own job list (/System/Library/xpc/launchd.plist, "LaunchDaemons", and the plists in
 * /System/Library/LaunchDaemons): every job whose Program or ProgramArguments[0] has that basename. Those labels are
 * added to disabled.plist; whatever it held stays. /var/sweet/svc-enable lists names to take out again.
 * Log: /var/tmp/sweetsvc.log.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DISABLED "/private/var/db/com.apple.xpc.launchd/disabled.plist"
#define CACHE "/System/Library/xpc/launchd.plist"

static FILE *out;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fputc('\n', out);
	fflush(out);
}

static CFPropertyListRef load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > (32 << 20)) {
		fclose(f);
		return NULL;
	}
	UInt8 *buf = malloc((size_t)n);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	CFDataRef data = CFDataCreate(NULL, buf, (CFIndex)got);
	free(buf);
	CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainersAndLeaves, NULL, NULL);
	CFRelease(data);
	return plist;
}

static int save(const char *path, CFPropertyListRef plist)
{
	CFDataRef data = CFPropertyListCreateData(NULL, plist, kCFPropertyListXMLFormat_v1_0, 0, NULL);
	if (!data)
		return -1;
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s.sweettmp", path);
	FILE *f = fopen(tmp, "wb");
	int ok = f && fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), f) == (size_t)CFDataGetLength(data);
	if (f) {
		fflush(f);
		fsync(fileno(f));
		fclose(f);
	}
	CFRelease(data);
	return ok && rename(tmp, path) == 0 ? 0 : -1;
}

/* names[] from a file of program names */
static int read_names(const char *path, char names[][64], int max)
{
	FILE *f = fopen(path, "r");
	int n = 0;
	char line[256];
	if (!f)
		return 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n \t#")] = 0;
		if (line[0])
			snprintf(names[n++], 64, "%s", line);
	}
	fclose(f);
	return n;
}

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

/* The program of a job dictionary, as a C string in buf (empty if none). */
static void job_program(CFDictionaryRef job, char *buf, size_t size)
{
	buf[0] = 0;
	CFTypeRef p = CFDictionaryGetValue(job, CFSTR("Program"));
	if (!p || CFGetTypeID(p) != CFStringGetTypeID()) {
		CFTypeRef args = CFDictionaryGetValue(job, CFSTR("ProgramArguments"));
		if (args && CFGetTypeID(args) == CFArrayGetTypeID() && CFArrayGetCount(args) > 0)
			p = CFArrayGetValueAtIndex(args, 0);
	}
	if (p && CFGetTypeID(p) == CFStringGetTypeID())
		CFStringGetCString(p, buf, (CFIndex)size, kCFStringEncodingUTF8);
}

static int labels_found;

/* For a job whose program's basename is one of names[], set disabled[label] = value. */
static void consider(CFDictionaryRef job, char names[][64], int n, CFMutableDictionaryRef disabled, CFBooleanRef value,
		     const char *where)
{
	char prog[512], label[256];
	if (!job || CFGetTypeID(job) != CFDictionaryGetTypeID())
		return;
	job_program(job, prog, sizeof(prog));
	CFTypeRef l = CFDictionaryGetValue(job, CFSTR("Label"));
	if (!prog[0] || !l || CFGetTypeID(l) != CFStringGetTypeID())
		return;
	CFStringGetCString(l, label, sizeof(label), kCFStringEncodingUTF8);
	for (int i = 0; i < n; i++) {
		if (!strcmp(base(prog), names[i])) {
			if (value == kCFBooleanTrue)
				CFDictionarySetValue(disabled, l, kCFBooleanTrue);
			else
				CFDictionaryRemoveValue(disabled, l);
			say("sweetsvc: %s %s (program %s, from %s)", value == kCFBooleanTrue ? "disable" : "enable", label, prog, where);
			labels_found++;
		}
	}
}

static void walk(char names[][64], int n, CFMutableDictionaryRef disabled, CFBooleanRef value)
{
	CFPropertyListRef cache = load(CACHE);
	if (cache && CFGetTypeID(cache) == CFDictionaryGetTypeID()) {
		CFTypeRef daemons = CFDictionaryGetValue(cache, CFSTR("LaunchDaemons"));
		if (daemons && CFGetTypeID(daemons) == CFDictionaryGetTypeID()) {
			CFIndex count = CFDictionaryGetCount(daemons);
			const void **values = malloc(sizeof(void *) * (size_t)count);
			CFDictionaryGetKeysAndValues(daemons, NULL, values);
			for (CFIndex i = 0; i < count; i++)
				consider(values[i], names, n, disabled, value, CACHE);
			free(values);
		}
	} else {
		say("sweetsvc: no job list at %s", CACHE);
	}
	if (cache)
		CFRelease(cache);
	DIR *d = opendir("/System/Library/LaunchDaemons");
	struct dirent *e;
	while (d && (e = readdir(d)) != NULL) {
		if (!strstr(e->d_name, ".plist"))
			continue;
		char path[512];
		snprintf(path, sizeof(path), "/System/Library/LaunchDaemons/%s", e->d_name);
		CFPropertyListRef job = load(path);
		if (job) {
			consider(job, names, n, disabled, value, path);
			CFRelease(job);
		}
	}
	if (d)
		closedir(d);
}

int main(void)
{
	char off[32][64], on[32][64];
	out = fopen("/var/tmp/sweetsvc.log", "a");
	if (!out)
		out = stdout;
	int noff = read_names("/var/sweet/svc-disable", off, 32);
	int non = read_names("/var/sweet/svc-enable", on, 32);
	say("sweetsvc: %d name(s) to disable, %d to enable", noff, non);
	if (noff + non == 0)
		return 0;
	CFPropertyListRef was = load(DISABLED);
	CFMutableDictionaryRef disabled = was && CFGetTypeID(was) == CFDictionaryGetTypeID() ?
	    (CFMutableDictionaryRef)was : CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
								      &kCFTypeDictionaryValueCallBacks);
	say("sweetsvc: %s held %ld entr%s", DISABLED, (long)CFDictionaryGetCount(disabled),
	    CFDictionaryGetCount(disabled) == 1 ? "y" : "ies");
	walk(off, noff, disabled, kCFBooleanTrue);
	walk(on, non, disabled, kCFBooleanFalse);
	if (!labels_found) {
		say("sweetsvc: RESULT no job matched: nothing changed");
		return 0;
	}
	mkdir("/private/var/db/com.apple.xpc.launchd", 0755);
	int r = save(DISABLED, disabled);
	say("sweetsvc: RESULT %s: %ld entr%s, from the next boot", r == 0 ? "written" : "NOT written",
	    (long)CFDictionaryGetCount(disabled), CFDictionaryGetCount(disabled) == 1 ? "y" : "ies");
	return r != 0;
}
