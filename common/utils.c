#include "../include/utils.h"
#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

uint64_t raise_nofile_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return 0;
    rlim_t want = rl.rlim_max;
#ifdef __APPLE__
    /* The hard limit can read as "infinity" but the kernel caps it per process. */
    int maxfiles = 0;
    size_t len = sizeof(maxfiles);
    if (sysctlbyname("kern.maxfilesperproc", &maxfiles, &len, NULL, 0) == 0 && maxfiles > 0 &&
        (want == RLIM_INFINITY || want > (rlim_t)maxfiles))
        want = (rlim_t)maxfiles;
#endif
    if (want == RLIM_INFINITY)
        want = 1u << 20;
    if (want > rl.rlim_cur) {
        rl.rlim_cur = want;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0 && getrlimit(RLIMIT_NOFILE, &rl) != 0)
            return 0;
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    return (uint64_t)rl.rlim_cur;
}
