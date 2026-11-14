/* Linux's eventfd, which Darwin has not got. panfrost_kmod.c uses it only for performance counters: always fails. */
#ifndef SWEET_SYS_EVENTFD_H
#define SWEET_SYS_EVENTFD_H
#include <errno.h>
#include <stdint.h>

typedef uint64_t eventfd_t;
#define EFD_CLOEXEC  0x80000
#define EFD_NONBLOCK 0x800

static inline int eventfd(unsigned int initval, int flags) { (void)initval; (void)flags; errno = ENOSYS; return -1; }
static inline int eventfd_read(int fd, eventfd_t *value) { (void)fd; (void)value; errno = ENOSYS; return -1; }
static inline int eventfd_write(int fd, eventfd_t value) { (void)fd; (void)value; errno = ENOSYS; return -1; }
#endif
