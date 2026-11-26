/*
 * sweetjob: list or remove launchd jobs in the root's /System/Library/xpc/launchd.plist (2026-11-26).
 *
 *   sweetjob list                 every job: label and program
 *   sweetjob remove PATTERN...    remove the jobs whose Label is PATTERN or whose program path contains it
 *   sweetjob env LABEL KEY=VALUE  EnvironmentVariables KEY=VALUE for the job (VALUE empty: KEY taken out)
 *   sweetjob log LABEL FILE       the job's os_log also goes to FILE: EnvironmentVariables OS_ACTIVITY_DT_MODE=YES
 *                                 (libtrace then writes every message to stderr too) and StandardErrorPath FILE
 *                                 (2026-11-26: PosterBoard runs but launches no poster at all; what it says is the
 *                                 only way to see why, and its own signed binary has to stay for its entitlements)
 *
 * launchd reads that file at boot (boot-arg launchd_unsecure_cache=1), so a removal takes effect at the next boot.
 * The first change keeps the original as launchd.plist.sweetbak (never overwritten); the new file is written next to
 * it, fsync'd and renamed over it, in the same binary format. Run by sweetroot (`run /usr/local/bin/sweetjob ...`).
 * Why: VisualVoicemail's vmd used ~0.9 of a CPU all the time with no telephony (round ah, 2026-11-26).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLIST "/System/Library/xpc/launchd.plist"
#define BACKUP PLIST ".sweetbak"
#define TMP PLIST ".sweettmp"

static void
text(CFTypeRef value, char *out, size_t size)
{
	out[0] = 0;
	if (value != NULL && CFGetTypeID(value) == CFStringGetTypeID()) {
		CFStringGetCString(value, out, (CFIndex)size, kCFStringEncodingUTF8);
	}
}

/* the job's program: Program, else ProgramArguments[0] */
static void
program_of(CFDictionaryRef job, char *out, size_t size)
{
	CFTypeRef args;

	text(CFDictionaryGetValue(job, CFSTR("Program")), out, size);
	if (out[0] == 0 && (args = CFDictionaryGetValue(job, CFSTR("ProgramArguments"))) != NULL &&
	    CFGetTypeID(args) == CFArrayGetTypeID() && CFArrayGetCount(args) > 0) {
		text(CFArrayGetValueAtIndex(args, 0), out, size);
	}
}

static CFDataRef
read_file(const char *path)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	CFDataRef data = NULL;

	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (64 << 20)) {
		if (fd >= 0) {
			close(fd);
		}
		return NULL;
	}
	UInt8 *bytes = malloc((size_t)st.st_size);
	if (bytes != NULL && read(fd, bytes, (size_t)st.st_size) == st.st_size) {
		data = CFDataCreate(NULL, bytes, (CFIndex)st.st_size);
	}
	free(bytes);
	close(fd);
	return data;
}

static int
write_file(const char *path, const UInt8 *bytes, size_t length, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	size_t done = 0;

	if (fd < 0) {
		return -1;
	}
	while (done < length) {
		ssize_t n = write(fd, bytes + done, length - done);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		done += (size_t)n;
	}
	if (fsync(fd) != 0) {
		close(fd);
		return -1;
	}
	return close(fd);
}

int
main(int argc, char **argv)
{
	bool list = argc == 2 && strcmp(argv[1], "list") == 0;
	bool remove = argc >= 3 && strcmp(argv[1], "remove") == 0;
	bool logging = argc == 4 && strcmp(argv[1], "log") == 0;
	bool envset = argc == 4 && strcmp(argv[1], "env") == 0 && strchr(argv[3], '=') != NULL;
	CFPropertyListFormat format = kCFPropertyListBinaryFormat_v1_0;
	CFErrorRef error = NULL;
	struct stat st;

	if (!list && !remove && !logging && !envset) {
		fprintf(stderr, "usage: sweetjob list | sweetjob remove PATTERN... | sweetjob log LABEL FILE\n");
		return 2;
	}
	CFDataRef data = read_file(PLIST);
	if (data == NULL || stat(PLIST, &st) != 0) {
		fprintf(stderr, "sweetjob: cannot read %s\n", PLIST);
		return 1;
	}
	CFPropertyListRef root = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainersAndLeaves,
	    &format, &error);
	CFRelease(data);
	if (root == NULL || CFGetTypeID(root) != CFDictionaryGetTypeID()) {
		fprintf(stderr, "sweetjob: %s is not a property list dictionary\n", PLIST);
		return 1;
	}
	CFMutableDictionaryRef daemons = (CFMutableDictionaryRef)CFDictionaryGetValue(root, CFSTR("LaunchDaemons"));
	if (daemons == NULL || CFGetTypeID(daemons) != CFDictionaryGetTypeID()) {
		fprintf(stderr, "sweetjob: no LaunchDaemons dictionary\n");
		return 1;
	}
	CFIndex count = CFDictionaryGetCount(daemons);
	const void **keys = calloc((size_t)count, sizeof(void *)), **values = calloc((size_t)count, sizeof(void *));
	if (keys == NULL || values == NULL) {
		return 1;
	}
	CFDictionaryGetKeysAndValues(daemons, keys, values);
	int removed = 0;
	for (CFIndex i = 0; i < count; i++) {
		char label[256], program[512];

		if (CFGetTypeID(values[i]) != CFDictionaryGetTypeID()) {
			continue;
		}
		text(CFDictionaryGetValue(values[i], CFSTR("Label")), label, sizeof(label));
		program_of(values[i], program, sizeof(program));
		if (list) {
			printf("%s\t%s\n", label, program);
			continue;
		}
		if (envset && strcmp(label, argv[2]) == 0) {
			CFMutableDictionaryRef job = (CFMutableDictionaryRef)values[i];
			CFTypeRef env = CFDictionaryGetValue(job, CFSTR("EnvironmentVariables"));
			char keytext[256];
			const char *eq = strchr(argv[3], '=');

			snprintf(keytext, sizeof(keytext), "%.*s", (int)(eq - argv[3]), argv[3]);
			CFStringRef key = CFStringCreateWithCString(NULL, keytext, kCFStringEncodingUTF8);
			CFStringRef value = CFStringCreateWithCString(NULL, eq + 1, kCFStringEncodingUTF8);
			CFTypeRef had = env != NULL && CFGetTypeID(env) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(env, key) : NULL;

			if ((eq[1] == 0 && had == NULL) || (had != NULL && CFEqual(had, value))) {
				printf("sweetjob: %s already has %s\n", label, argv[3]);
				return 0;
			}
			if (env == NULL || CFGetTypeID(env) != CFDictionaryGetTypeID()) {
				CFMutableDictionaryRef fresh = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
				    &kCFTypeDictionaryValueCallBacks);
				CFDictionarySetValue(job, CFSTR("EnvironmentVariables"), fresh);
				CFRelease(fresh);
				env = CFDictionaryGetValue(job, CFSTR("EnvironmentVariables"));
			}
			if (eq[1] == 0) {
				CFDictionaryRemoveValue((CFMutableDictionaryRef)env, key);
			} else {
				CFDictionarySetValue((CFMutableDictionaryRef)env, key, value);
			}
			printf("sweetjob: %s (%s): environment %s\n", label, program, argv[3]);
			logging = true;         /* (written the same way) */
			removed++;
			break;
		}
		if (logging) {
			CFMutableDictionaryRef job = (CFMutableDictionaryRef)values[i];
			CFStringRef file = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
			CFTypeRef env = CFDictionaryGetValue(job, CFSTR("EnvironmentVariables"));
			CFTypeRef had = CFDictionaryGetValue(job, CFSTR("StandardErrorPath"));
			CFTypeRef mode = env != NULL && CFGetTypeID(env) == CFDictionaryGetTypeID() ?
			    CFDictionaryGetValue(env, CFSTR("OS_ACTIVITY_DT_MODE")) : NULL;

			if (strcmp(label, argv[2]) != 0) {
				CFRelease(file);
				continue;
			}
			if (had != NULL && CFEqual(had, file) && mode != NULL && CFEqual(mode, CFSTR("YES"))) {
				printf("sweetjob: %s already logs to %s\n", label, argv[3]);
				CFRelease(file);
				return 0;
			}
			if (env == NULL || CFGetTypeID(env) != CFDictionaryGetTypeID()) {
				CFMutableDictionaryRef fresh = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
				    &kCFTypeDictionaryValueCallBacks);
				CFDictionarySetValue(job, CFSTR("EnvironmentVariables"), fresh);
				CFRelease(fresh);
				env = CFDictionaryGetValue(job, CFSTR("EnvironmentVariables"));
			}
			CFDictionarySetValue((CFMutableDictionaryRef)env, CFSTR("OS_ACTIVITY_DT_MODE"), CFSTR("YES"));
			CFDictionarySetValue(job, CFSTR("StandardErrorPath"), file);
			CFRelease(file);
			printf("sweetjob: %s (%s): OS_ACTIVITY_DT_MODE=YES, stderr to %s\n", label, program, argv[3]);
			removed++;
			break;
		}
		for (int a = 2; a < argc; a++) {
			if (strcmp(label, argv[a]) == 0 || (program[0] != 0 && strstr(program, argv[a]) != NULL)) {
				printf("sweetjob: removing %s (%s)\n", label, program);
				CFDictionaryRemoveValue(daemons, keys[i]);
				removed++;
				break;
			}
		}
	}
	if (list) {
		printf("sweetjob: %ld jobs\n", (long)count);
		return 0;
	}
	if (removed == 0) {
		printf("sweetjob: no job matched; %s left alone\n", PLIST);
		return 0;
	}
	CFDataRef out = CFPropertyListCreateData(NULL, root, format, 0, &error);
	if (out == NULL) {
		fprintf(stderr, "sweetjob: cannot serialise the property list\n");
		return 1;
	}
	if (access(BACKUP, F_OK) != 0) {
		CFDataRef original = read_file(PLIST);
		if (original == NULL || write_file(BACKUP, CFDataGetBytePtr(original), (size_t)CFDataGetLength(original),
		    st.st_mode & 07777) != 0) {
			fprintf(stderr, "sweetjob: cannot keep the original as %s; nothing changed\n", BACKUP);
			return 1;
		}
		CFRelease(original);
		printf("sweetjob: original kept as %s\n", BACKUP);
	}
	if (write_file(TMP, CFDataGetBytePtr(out), (size_t)CFDataGetLength(out), st.st_mode & 07777) != 0 ||
	    chown(TMP, st.st_uid, st.st_gid) != 0 || rename(TMP, PLIST) != 0) {
		fprintf(stderr, "sweetjob: cannot write %s; nothing changed\n", PLIST);
		unlink(TMP);
		return 1;
	}
	sync();
	if (logging) {
		printf("sweetjob: %s written; in effect at the next boot\n", PLIST);
		return 0;
	}
	printf("sweetjob: %d job(s) removed from %s (%ld -> %ld); in effect at the next boot\n", removed, PLIST,
	    (long)count, (long)(count - removed));
	return 0;
}
