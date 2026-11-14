/* Linux's timerfd, which Darwin has not got. panfrost_kmod.c uses it only for performance counters: always fails. */
#ifndef SWEET_SYS_TIMERFD_H
#define SWEET_SYS_TIMERFD_H
#include <errno.h>
#include <time.h>

#define TFD_CLOEXEC  0x80000
#define TFD_NONBLOCK 0x800

/* Darwin's time.h may or may not have it: ours under another name either way. */
struct sweet_itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};
#define itimerspec sweet_itimerspec

static inline int timerfd_create(int clockid, int flags) { (void)clockid; (void)flags; errno = ENOSYS; return -1; }
static inline int timerfd_settime(int fd, int flags, const struct itimerspec *n, struct itimerspec *o)
{
	(void)fd; (void)flags; (void)n; (void)o;
	errno = ENOSYS;
	return -1;
}
#endif
