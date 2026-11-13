/*
 * SweetMTLDriver.bundle: the Metal driver that iOS 16's Metal loads for our GPU.
 *
 * How Metal finds it (_MTLRegisterDevices, see ios-viva docs/PIANO-GPU.md "Fatti verificati"): every IOAcceleratorES
 * service in the kernel that has MetalPluginName loads /System/Library/Extensions/<MetalPluginName>.bundle, takes
 * the class named by MetalPluginClassName, checks that it is a subclass of _MTLDevice, then calls
 * [[cls alloc] initWithAcceleratorPort:service]. Our kernel (patch 0126) publishes SweetAccelerator with
 * MetalPluginName = SweetMTLDriver and MetalPluginClassName = SweetMTLDevice.
 *
 * Step 1 (this file): prove that the chain works. The device draws nothing yet. It is handed only to the programs
 * listed in /var/sweet/metal.allow (one name per line; without the file: metaltest). Every other process, backboardd
 * and SpringBoard first of all, gets no device and keeps drawing with the CPU as before.
 * Log: /var/tmp/sweetmtl.log
 */
#import <Foundation/Foundation.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Metal's private base class of every device; resolved at load time from Metal.framework (link flag -U). */
@interface _MTLDevice : NSObject
@end

@interface SweetMTLDevice : _MTLDevice
- (instancetype)initWithAcceleratorPort:(unsigned int)port;
@end

static void say(const char *fmt, ...)
{
	char line[600];
	va_list ap;
	FILE *f = fopen("/var/tmp/sweetmtl.log", "a");
	if (!f) return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(f, "%s[%d]: %s\n", getprogname(), (int)getpid(), line);
	fclose(f);
}

/* Is this process allowed to get the device? */
static int allowed(void)
{
	const char *me = getprogname();
	FILE *f = fopen("/var/sweet/metal.allow", "r");
	char line[256];
	int yes = 0;

	if (!f) return strcmp(me, "metaltest") == 0;
	while (!yes && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n \t")] = 0;
		if (line[0] && strcmp(line, me) == 0) yes = 1;
	}
	fclose(f);
	return yes;
}

__attribute__((constructor)) static void loaded(void)
{
	say("bundle loaded");
}

@implementation SweetMTLDevice

- (instancetype)initWithAcceleratorPort:(unsigned int)port
{
	if (!allowed()) {
		say("initWithAcceleratorPort: service port 0x%x, not on the allow list: no device", port);
		return nil;
	}
	self = [super init];
	say("initWithAcceleratorPort: service port 0x%x, device %p", port, (__bridge void *)self);
	return self;
}

- (NSString *)name
{
	return @"Sweet Mali-G52 (stub)";
}

@end
