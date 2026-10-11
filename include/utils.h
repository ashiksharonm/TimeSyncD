#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdio.h>

#define LOG_INFO(...) do { printf("[INFO] "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define LOG_ERR(...)  do { fprintf(stderr, "[ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

/* Monotonic time in nanoseconds. */
uint64_t get_time_ns(void);

int set_nonblocking(int fd);

/* Raise RLIMIT_NOFILE as far as the OS allows; returns the resulting soft limit (0 on error). */
uint64_t raise_nofile_limit(void);

#endif
