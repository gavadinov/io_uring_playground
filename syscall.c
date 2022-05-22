#include <unistd.h>
#include <sys/syscall.h>
#if defined __has_include && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#else
#include "include/io_uring.h"
#endif

/*
 * Because io_uring is still not in libc we provide our own sysstem calls
 */

int __sys_io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return syscall(__NR_io_uring_setup, entries, p);
}

int __sys_io_uring_enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags)
{
    return syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}
