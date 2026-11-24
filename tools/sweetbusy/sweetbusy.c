/*
 * sweetbusy: which IOKit services are still "busy" on the phone (2026-11-24).
 *
 *   sweetbusy OUT [DELAY_S]     goes to the background at once, waits DELAY_S (default 120) seconds, then appends to
 *                               OUT every service whose busy state is not 0 (class, name, busy count, path), the
 *                               network-looking services, and whether IOKit is quiet; then again 60 s later.
 *
 * Why: Settings > General > About hangs in _SCNetworkInterfaceCopyAllWithPreferences -> SCDynamicStoreNotifyWait
 * (sample of 2026-11-24 17:35, named by sweetsym): SystemConfiguration waits for configd's InterfaceNamer, which
 * waits for IOKit to go quiet. A driver of ours that never leaves its busy state would keep it waiting for ever.
 * Run by sweetroot (`run /usr/local/bin/sweetbusy /var/tmp/sweetbusy.txt`); it forks, so sweetroot does not wait.
 */
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_iterator_t;
typedef io_object_t io_registry_entry_t;
typedef char io_name_t[128];
typedef char io_string_t[512];

extern kern_return_t IORegistryCreateIterator(mach_port_t, const char *, uint32_t, io_iterator_t *);
extern io_object_t IOIteratorNext(io_iterator_t);
extern kern_return_t IOObjectRelease(io_object_t);
extern kern_return_t IOServiceGetBusyState(io_object_t, uint32_t *);
extern kern_return_t IORegistryEntryGetName(io_registry_entry_t, io_name_t);
extern kern_return_t IOObjectGetClass(io_object_t, io_name_t);
extern kern_return_t IORegistryEntryGetPath(io_registry_entry_t, const char *, io_string_t);
extern kern_return_t IOKitWaitQuiet(mach_port_t, mach_timespec_t *);
extern io_registry_entry_t IORegistryGetRootEntry(mach_port_t);

static int
networkish(const char *c)
{
	static const char *const words[] = { "Network", "Ethernet", "80211", "Skywalk", "Interface", "WLAN", "Wifi",
		"WiFi", "Bluetooth", "Cellular", "Baseband", "USB", "Conn", "CONN", NULL };
	for (int i = 0; words[i]; i++) {
		if (strstr(c, words[i])) {
			return 1;
		}
	}
	return 0;
}

static void
report(FILE *out, int round)
{
	io_iterator_t it = 0;
	io_object_t o;
	unsigned total = 0, busy = 0;
	time_t now = time(NULL);

	fprintf(out, "=== sweetbusy round %d at %s", round, ctime(&now));
	io_registry_entry_t root = IORegistryGetRootEntry(MACH_PORT_NULL);
	uint32_t rb = 0;
	IOServiceGetBusyState(root, &rb);
	fprintf(out, "root busy state %u\n", rb);
	IOObjectRelease(root);
	mach_timespec_t t = { 1, 0 };
	kern_return_t q = IOKitWaitQuiet(MACH_PORT_NULL, &t);
	fprintf(out, "IOKitWaitQuiet(1 s) -> %#x (%s)\n", q, q == KERN_SUCCESS ? "quiet" : "NOT quiet");
	if (IORegistryCreateIterator(MACH_PORT_NULL, "IOService", 1 /* recursively */, &it) != KERN_SUCCESS) {
		fprintf(out, "no iterator\n");
		return;
	}
	while ((o = IOIteratorNext(it)) != 0) {
		io_name_t cls = "", name = "";
		io_string_t path = "";
		uint32_t b = 0;

		total++;
		IOObjectGetClass(o, cls);
		IORegistryEntryGetName(o, name);
		if (IOServiceGetBusyState(o, &b) != KERN_SUCCESS) {
			b = 0;
		}
		if (b != 0 || networkish(cls)) {
			IORegistryEntryGetPath(o, "IOService", path);
			fprintf(out, "%s %s \"%s\" busy %u  %s\n", b ? "BUSY" : "net ", cls, name, b, path);
		}
		busy += b != 0;
		IOObjectRelease(o);
	}
	IOObjectRelease(it);
	fprintf(out, "%u services, %u busy\n", total, busy);
	fflush(out);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: sweetbusy OUT [DELAY_S]\n");
		return 2;
	}
	int delay = argc > 2 ? atoi(argv[2]) : 120;
	pid_t p = fork();
	if (p < 0) {
		return 1;
	}
	if (p > 0) {
		return 0;               /* sweetroot goes on */
	}
	setsid();
	sleep((unsigned)delay);
	FILE *out = fopen(argv[1], "a");
	if (!out) {
		return 1;
	}
	report(out, 1);
	sleep(60);
	report(out, 2);
	fclose(out);
	return 0;
}
