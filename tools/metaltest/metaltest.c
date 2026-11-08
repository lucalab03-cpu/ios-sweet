/* metaltest: what Metal sees. Plain C, loads everything with dlopen so a missing library is logged, not fatal.
 * Log: /var/tmp/sweetmetal.log */
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static FILE *lg;
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(lg, fmt, ap);
    va_end(ap);
    fputc('\n', lg);
    fflush(lg);
}
static void lsdir(const char *path, const char *needle) {
    DIR *d = opendir(path);
    if (!d) { say("ls %s: cannot open", path); return; }
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcasestr(e->d_name, needle)) say("ls %s: %s", path, e->d_name);
    closedir(d);
}
static void try_lib(const char *path) {
    void *h = dlopen(path, RTLD_NOW);
    say("dlopen %s: %s", path, h ? "ok" : dlerror());
}

int main(void) {
    lg = fopen("/var/tmp/sweetmetal.log", "w");
    if (!lg) return 1;
    say("metaltest: start, pid %d", getpid());
    const char *dirs[] = { "/System/Library/Extensions", "/System/Library/PrivateFrameworks", "/System/Library/Frameworks" };
    const char *keys[] = { "AGX", "GPU", "Metal", "Accel", "GLES", "IOSurface" };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 6; k++) lsdir(dirs[i], keys[k]);
    say("--- libraries");
    try_lib("/System/Library/Frameworks/Foundation.framework/Foundation");
    try_lib("/System/Library/Frameworks/Metal.framework/Metal");
    void *m = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
    if (m) {
        void *(*create)(void) = dlsym(m, "MTLCreateSystemDefaultDevice");
        say("MTLCreateSystemDefaultDevice symbol: %p", (void *)create);
        if (create) {
            say("calling it");
            void *dev = create();
            say("default device: %p", dev);
        }
    }
    say("metaltest: done");
    return 0;
}
