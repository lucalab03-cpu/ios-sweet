/*
 * sweetstack: what the threads of named processes are waiting on, with names, from the phone itself (2026-11-27).
 *
 *   sweetstack DELAY NAME...   in the background (sweetroot does not wait): at DELAY, 2*DELAY and 3*DELAY seconds the
 *                              stacks of every thread of each process whose name (p_comm) is one of NAME, into
 *                              /var/tmp/sweetstack.log.
 *
 * Why: round at, LaunchServices' rebuild (-[LSApplicationWorkspace _LSPrivateRebuildApplicationDatabasesForSystemApps:
 * internal:user:]) never came back, lsd stayed at 1-3 % of a CPU and SpringBoard waited. The kernel's sampler gives
 * bare addresses and naming them took a second boot (sweetsym); here the names come at once: the dyld shared cache is
 * at the same slide in every process of a boot, so an address in lsd is the same function in this process. The image
 * holding an address is found with dyld_shared_cache_iterate_text, loaded here (dlopen) and named with dladdr.
 * Frames: the frame-pointer chain (arm64, no pointer authentication on this iOS build). task_for_pid needs root
 * and sweetstack.entitlements.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

struct sweet_dylib_text_info {
	uint64_t version;
	uint64_t loadAddressUnslid;
	uint64_t textSegmentSize;
	uuid_t dylibUuid;
	const char *path;
	uint64_t textSegmentOffset;
};
extern int dyld_shared_cache_iterate_text(const uuid_t cacheUuid,
    void (^callback)(const struct sweet_dylib_text_info *info));
extern bool _dyld_get_shared_cache_uuid(uuid_t uuid);
kern_return_t task_for_pid(mach_port_t target, int pid, mach_port_t *task);

#define MAX_IMAGES 4096
static struct {
	uint64_t start, end;
	const char *path;
	int tried;
} images[MAX_IMAGES];
static int nimages;
static uint64_t slide;
static FILE *out;

static void
say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fflush(out);
}

static void
load_cache_map(void)
{
	uuid_t uuid;
	Dl_info self;
	__block uint64_t malloc_unslid = 0;

	if (!_dyld_get_shared_cache_uuid(uuid)) {
		say("sweetstack: no shared cache uuid\n");
		return;
	}
	dyld_shared_cache_iterate_text(uuid, ^(const struct sweet_dylib_text_info *info) {
		if (nimages < MAX_IMAGES) {
			images[nimages].start = info->loadAddressUnslid;
			images[nimages].end = info->loadAddressUnslid + info->textSegmentSize;
			images[nimages].path = strdup(info->path);
			nimages++;
		}
		if (strcmp(info->path, "/usr/lib/system/libsystem_malloc.dylib") == 0) {
			malloc_unslid = info->loadAddressUnslid;
		}
	});
	if (dladdr((const void *)&malloc, &self) && malloc_unslid != 0) {
		slide = (uint64_t)(uintptr_t)self.dli_fbase - malloc_unslid;
	}
	say("sweetstack: %d images in the shared cache, slide 0x%llx\n", nimages, (unsigned long long)slide);
}

static void
name_of(uint64_t pc, char *text, size_t size)
{
	Dl_info info;

	for (int i = 0; i < nimages; i++) {
		if (pc - slide >= images[i].start && pc - slide < images[i].end) {
			if (!images[i].tried) {
				images[i].tried = 1;
				dlopen(images[i].path, RTLD_LAZY | RTLD_LOCAL);
			}
			const char *base = strrchr(images[i].path, '/');

			base = base != NULL ? base + 1 : images[i].path;
			if (dladdr((const void *)(uintptr_t)pc, &info) && info.dli_sname != NULL) {
				snprintf(text, size, "%s`%s+%llu", base, info.dli_sname,
				    (unsigned long long)(pc - (uint64_t)(uintptr_t)info.dli_saddr));
			} else {
				snprintf(text, size, "%s+0x%llx", base, (unsigned long long)(pc - slide - images[i].start));
			}
			return;
		}
	}
	snprintf(text, size, "0x%llx (not in the cache: the program itself)", (unsigned long long)pc);
}

static void
thread_stack(task_t task, thread_t thread, int index)
{
	arm_thread_state64_t st;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	thread_extended_info_data_t ext;
	mach_msg_type_number_t ecount = THREAD_EXTENDED_INFO_COUNT;
	char name[256];

	if (thread_info(thread, THREAD_EXTENDED_INFO, (thread_info_t)&ext, &ecount) != KERN_SUCCESS) {
		ext.pth_name[0] = 0;
		ext.pth_run_state = 0;
	}
	if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&st, &count) != KERN_SUCCESS) {
		say("  thread %d: no state\n", index);
		return;
	}
	uint64_t pc = __darwin_arm_thread_state64_get_pc(st) & 0xfffffffffULL;
	uint64_t lr = __darwin_arm_thread_state64_get_lr(st) & 0xfffffffffULL;
	uint64_t fp = __darwin_arm_thread_state64_get_fp(st);

	say("  thread %d \"%s\" state %d\n", index, ext.pth_name, ext.pth_run_state);
	name_of(pc, name, sizeof(name));
	say("    0 %s\n", name);
	name_of(lr, name, sizeof(name));
	say("    1 %s (lr)\n", name);
	for (int depth = 2; depth < 28 && fp != 0; depth++) {
		uint64_t frame[2];
		vm_size_t got = 0;

		if (vm_read_overwrite(task, (vm_address_t)fp, sizeof(frame), (vm_address_t)(uintptr_t)frame, &got) != KERN_SUCCESS ||
		    got != sizeof(frame)) {
			break;
		}
		uint64_t ret = frame[1] & 0xfffffffffULL;

		if (ret == 0) {
			break;
		}
		name_of(ret, name, sizeof(name));
		say("    %d %s\n", depth, name);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
}

static void
process_stacks(int pid, const char *comm)
{
	task_t task = MACH_PORT_NULL;
	kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
	thread_act_array_t threads;
	mach_msg_type_number_t n = 0;

	if (kr != KERN_SUCCESS) {
		say("%s[%d]: task_for_pid %d\n", comm, pid, kr);
		return;
	}
	if (task_threads(task, &threads, &n) != KERN_SUCCESS) {
		say("%s[%d]: no threads\n", comm, pid);
		mach_port_deallocate(mach_task_self(), task);
		return;
	}
	say("%s[%d]: %u threads\n", comm, pid, n);
	for (mach_msg_type_number_t i = 0; i < n; i++) {
		thread_stack(task, threads[i], (int)i);
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(*threads));
	mach_port_deallocate(mach_task_self(), task);
}

static void
round_of(int argc, char **argv)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	struct kinfo_proc *procs = NULL;
	size_t size = 0;

	if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || (procs = malloc(size + 32 * sizeof(*procs))) == NULL) {
		say("sweetstack: no process list\n");
		return;
	}
	size += 32 * sizeof(*procs);
	if (sysctl(mib, 4, procs, &size, NULL, 0) != 0) {
		free(procs);
		return;
	}
	for (size_t i = 0; i < size / sizeof(*procs); i++) {
		struct extern_proc *p = &procs[i].kp_proc;

		for (int a = 2; a < argc; a++) {
			if (strncmp(p->p_comm, argv[a], sizeof(p->p_comm)) == 0) {
				process_stacks(p->p_pid, p->p_comm);
			}
		}
	}
	free(procs);
}

int
main(int argc, char **argv)
{
	int delay = argc > 2 ? atoi(argv[1]) : 0;

	if (delay <= 0) {
		fprintf(stderr, "usage: sweetstack DELAY NAME...\n");
		return 2;
	}
	pid_t pid = fork();

	if (pid != 0) {
		printf("sweetstack: in the background (pid %d), log /var/tmp/sweetstack.log\n", pid);
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
	out = fopen("/var/tmp/sweetstack.log", "a");
	if (out == NULL) {
		return 1;
	}
	say("sweetstack: start, pid %d, every %d s\n", getpid(), delay);
	load_cache_map();
	for (int r = 1; r <= 3; r++) {
		sleep(delay);
		say("=== sweetstack round %d, %d s after start\n", r, r * delay);
		round_of(argc, argv);
	}
	say("sweetstack: done\n");
	return 0;
}
