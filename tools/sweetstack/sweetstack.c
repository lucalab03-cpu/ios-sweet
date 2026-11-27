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
#include <mach-o/dyld_images.h>
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

/* (prof mode) the target's images outside the shared cache, by load address, from its dyld_all_image_infos */
#define MAX_TARGET 256
static struct { uint64_t start; char name[64]; } target_images[MAX_TARGET];
static int ntarget;

static void
name_of(uint64_t pc, char *text, size_t size);

static int
by_start(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void
load_target_images(task_t task)
{
	struct task_dyld_info dinfo;
	mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
	struct dyld_all_image_infos all;
	vm_size_t got = 0;

	if (task_info(task, TASK_DYLD_INFO, (task_info_t)&dinfo, &count) != KERN_SUCCESS ||
	    vm_read_overwrite(task, (vm_address_t)dinfo.all_image_info_addr, sizeof(all), (vm_address_t)&all, &got) != KERN_SUCCESS) {
		say("sweetstack: no image list of the target\n");
		return;
	}
	for (uint32_t i = 0; i < all.infoArrayCount && ntarget < MAX_TARGET; i++) {
		struct dyld_image_info info;
		char path[512];

		if (vm_read_overwrite(task, (vm_address_t)(all.infoArray + i), sizeof(info), (vm_address_t)&info, &got) != KERN_SUCCESS) {
			break;
		}
		uint64_t at = (uint64_t)(uintptr_t)info.imageLoadAddress;

		bool cached = false;

		for (int k = 0; k < nimages && !cached; k++) {
			cached = at - slide >= images[k].start && at - slide < images[k].end;
		}
		if (cached) {
			continue;               /* (in the shared cache: named by dladdr) */
		}
		memset(path, 0, sizeof(path));
		if (vm_read_overwrite(task, (vm_address_t)info.imageFilePath, sizeof(path) - 1, (vm_address_t)path, &got) != KERN_SUCCESS) {
			continue;
		}
		const char *base = strrchr(path, '/');

		target_images[ntarget].start = at;
		snprintf(target_images[ntarget].name, sizeof(target_images[ntarget].name), "%s", base != NULL ? base + 1 : path);
		ntarget++;
	}
	qsort(target_images, (size_t)ntarget, sizeof(target_images[0]), by_start);
	for (int i = 0; i < ntarget; i++) {
		say("sweetstack: image %s at 0x%llx\n", target_images[i].name, (unsigned long long)target_images[i].start);
	}
}

/*
 * sweetstack prof DELAY SAMPLES INTERVAL_MS PROCESS THREAD (2026-11-27, fix16): from DELAY seconds on, SAMPLES times
 * every INTERVAL_MS, the thread named THREAD of PROCESS is stopped for a moment and its stack read; at the end, the
 * functions it was in (the leaf) and the stacks (eight frames) most often seen, with counts, into the log. The kernel's
 * sampler (sweetprof) gives bare addresses of a shared cache whose names are only on the phone.
 */
#define PROF_KEYS 8192
static struct { char *key; unsigned count; } leaves[PROF_KEYS], stacks[PROF_KEYS], callers[PROF_KEYS];
static unsigned nleaves, nstacks, ncallers;

static void
tally(char *key, typeof(leaves) table, unsigned *n)
{
	for (unsigned i = 0; i < *n; i++) {
		if (strcmp(table[i].key, key) == 0) {
			table[i].count++;
			return;
		}
	}
	if (*n < PROF_KEYS) {
		table[*n].key = strdup(key);
		table[*n].count = 1;
		(*n)++;
	}
}

static int
by_count(const void *a, const void *b)
{
	unsigned x = ((const typeof(leaves[0]) *)a)->count, y = ((const typeof(leaves[0]) *)b)->count;

	return x > y ? -1 : x < y;
}

static void
prof_sample(task_t task, thread_t thread)
{
	arm_thread_state64_t st;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	uint64_t pcs[10];
	int depth = 0;
	char name[200], stack[2048];

	if (thread_suspend(thread) != KERN_SUCCESS) {
		return;
	}
	if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&st, &count) == KERN_SUCCESS) {
		uint64_t fp = __darwin_arm_thread_state64_get_fp(st);

		pcs[depth++] = __darwin_arm_thread_state64_get_pc(st) & 0xfffffffffULL;
		pcs[depth++] = __darwin_arm_thread_state64_get_lr(st) & 0xfffffffffULL;
		while (depth < 10 && fp != 0) {
			uint64_t frame[2];
			vm_size_t got = 0;

			if (vm_read_overwrite(task, (vm_address_t)fp, sizeof(frame), (vm_address_t)(uintptr_t)frame, &got) != KERN_SUCCESS ||
			    got != sizeof(frame) || (frame[1] & 0xfffffffffULL) == 0) {
				break;
			}
			pcs[depth++] = frame[1] & 0xfffffffffULL;
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
	}
	thread_resume(thread);
	if (depth == 0) {
		return;
	}
	stack[0] = 0;
	for (int i = 0; i < depth && i < 8; i++) {
		name_of(pcs[i], name, sizeof(name));
		char *plus = strrchr(name, '+');      /* (functions, not places in them: the offset dropped) */

		if (plus != NULL && strchr(name, '`') != NULL) {
			*plus = 0;
		}
		if (i == 0) {
			tally(name, leaves, &nleaves);
		}
		if (i == 2) {
			tally(name, callers, &ncallers);
		}
		strlcat(stack, i == 0 ? "" : " < ", sizeof(stack));
		strlcat(stack, name, sizeof(stack));
	}
	tally(stack, stacks, &nstacks);
}

static int
prof(int delay, int samples, int interval_ms, const char *process, const char *thread_name)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	struct kinfo_proc *procs = NULL;
	size_t size = 0;
	int pid = -1, taken = 0;
	task_t task = MACH_PORT_NULL;
	thread_t target = MACH_PORT_NULL;

	sleep((unsigned)delay);
	if (sysctl(mib, 4, NULL, &size, NULL, 0) == 0 && (procs = malloc(size + 32 * sizeof(*procs))) != NULL) {
		size += 32 * sizeof(*procs);
		if (sysctl(mib, 4, procs, &size, NULL, 0) == 0) {
			for (size_t i = 0; i < size / sizeof(*procs); i++) {
				if (strncmp(procs[i].kp_proc.p_comm, process, sizeof(procs[i].kp_proc.p_comm)) == 0) {
					pid = procs[i].kp_proc.p_pid;
				}
			}
		}
		free(procs);
	}
	if (pid < 0 || task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
		say("sweetstack prof: %s not found or not reachable\n", process);
		return 1;
	}
	load_target_images(task);
	for (int s = 0; s < samples; s++) {
		if (target == MACH_PORT_NULL) {
			thread_act_array_t threads;
			mach_msg_type_number_t n = 0;

			if (task_threads(task, &threads, &n) == KERN_SUCCESS) {
				for (mach_msg_type_number_t i = 0; i < n; i++) {
					thread_extended_info_data_t ext;
					mach_msg_type_number_t ecount = THREAD_EXTENDED_INFO_COUNT;

					if (target == MACH_PORT_NULL &&
					    thread_info(threads[i], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &ecount) == KERN_SUCCESS &&
					    strcmp(ext.pth_name, thread_name) == 0) {
						target = threads[i];
					} else {
						mach_port_deallocate(mach_task_self(), threads[i]);
					}
				}
				vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(*threads));
			}
			if (target == MACH_PORT_NULL) {
				say("sweetstack prof: no thread \"%s\" in %s[%d]\n", thread_name, process, pid);
				return 1;
			}
		}
		thread_basic_info_data_t basic;
		mach_msg_type_number_t bcount = THREAD_BASIC_INFO_COUNT;

		if (thread_info(target, THREAD_BASIC_INFO, (thread_info_t)&basic, &bcount) == KERN_SUCCESS &&
		    basic.run_state == TH_STATE_RUNNING) {
			prof_sample(task, target);      /* (only when it runs: what it waits on is not its cost) */
			taken++;
		}
		usleep((useconds_t)interval_ms * 1000);
	}
	qsort(leaves, nleaves, sizeof(leaves[0]), by_count);
	qsort(callers, ncallers, sizeof(callers[0]), by_count);
	qsort(stacks, nstacks, sizeof(stacks[0]), by_count);
	say("=== sweetstack prof %s[%d] \"%s\": %d samples of it running, of %d tries every %d ms\n", process, pid,
	    thread_name, taken, samples, interval_ms);
	for (unsigned i = 0; i < nleaves && i < 40; i++) {
		say("LEAF %u %s\n", leaves[i].count, leaves[i].key);
	}
	for (unsigned i = 0; i < ncallers && i < 40; i++) {
		say("FRAME2 %u %s\n", callers[i].count, callers[i].key);
	}
	for (unsigned i = 0; i < nstacks && i < 40; i++) {
		say("STACK %u %s\n", stacks[i].count, stacks[i].key);
	}
	say("sweetstack prof: done\n");
	return 0;
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
	for (int i = 0; i < ntarget; i++) {
		if (pc >= target_images[i].start && pc < target_images[i].start + 0x4000000 &&
		    (i + 1 == ntarget || pc < target_images[i + 1].start)) {
			snprintf(text, size, "%s+0x%llx", target_images[i].name, (unsigned long long)(pc - target_images[i].start));
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
	bool profiling = argc == 7 && strcmp(argv[1], "prof") == 0;
	int delay = profiling ? atoi(argv[2]) : argc > 2 ? atoi(argv[1]) : 0;

	if (delay <= 0) {
		fprintf(stderr, "usage: sweetstack DELAY NAME... | sweetstack prof DELAY SAMPLES INTERVAL_MS PROCESS THREAD\n");
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
	if (profiling) {
		return prof(delay, atoi(argv[3]), atoi(argv[4]), argv[5], argv[6]);
	}
	for (int r = 1; r <= 3; r++) {
		sleep(delay);
		say("=== sweetstack round %d, %d s after start\n", r, r * delay);
		round_of(argc, argv);
	}
	say("sweetstack: done\n");
	return 0;
}
