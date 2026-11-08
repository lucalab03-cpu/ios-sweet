/* metaltest: what Metal sees on this system. Log: /var/tmp/sweetmetal.log */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static FILE *lg;
static void say(NSString *s) { fprintf(lg, "%s\n", s.UTF8String); fflush(lg); }

static void lsdir(const char *path, const char *needle) {
    DIR *d = opendir(path);
    if (!d) { say([NSString stringWithFormat:@"ls %s: cannot open", path]); return; }
    struct dirent *e;
    while ((e = readdir(d)))
        if (!needle || strcasestr(e->d_name, needle))
            say([NSString stringWithFormat:@"ls %s: %s", path, e->d_name]);
    closedir(d);
}

int main(void) {
    lg = fopen("/var/tmp/sweetmetal.log", "w");
    if (!lg) return 1;
    say(@"metaltest: start");
    lsdir("/System/Library/Extensions", "AGX");
    lsdir("/System/Library/Extensions", "GPU");
    lsdir("/System/Library/Extensions", "Metal");
    lsdir("/System/Library/Extensions", "Accel");
    lsdir("/System/Library/Extensions", "GLES");
    lsdir("/System/Library/PrivateFrameworks", "AGX");
    lsdir("/System/Library/PrivateFrameworks", "GPU");
    lsdir("/System/Library/PrivateFrameworks", "Metal");
    lsdir("/System/Library/PrivateFrameworks", "GLES");
    lsdir("/System/Library/Frameworks", "Metal");
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        say([NSString stringWithFormat:@"default device: %@", dev]);
        NSArray *all = MTLCopyAllDevices();
        say([NSString stringWithFormat:@"all devices: %lu %@", (unsigned long)all.count, all]);
        if (dev) say([NSString stringWithFormat:@"name %@", dev.name]);
    }
    say(@"metaltest: done");
    return 0;
}
