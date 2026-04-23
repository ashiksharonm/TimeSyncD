#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define LOG_INFO(...) do { printf("[INFO] "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#define LOG_ERR(...)  do { fprintf(stderr, "[ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)

// Returns current monotonic time in nanoseconds
uint64_t get_time_ns(void);

#endif // UTILS_H
