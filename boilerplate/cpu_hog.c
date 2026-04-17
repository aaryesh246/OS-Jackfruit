/*
 * cpu_hog.c - CPU-bound workload for scheduler experiments.
 *
 * Usage:
 *   /cpu_hog [seconds]
 *
 * The program burns CPU and prints progress once per second so students
 * can compare completion times and responsiveness under different
 * priorities or CPU-affinity settings.
 *
 * If you copy this binary into an Alpine rootfs, make sure it is built in a
 * format that can run there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Added for Task 5 scheduling experiments */
#include <unistd.h>
#include <sched.h>

static unsigned int parse_seconds(const char *arg, unsigned int fallback)
{
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);
    if (!arg || *arg == '\0' || (end && *end != '\0') || value == 0)
        return fallback;
    return (unsigned int)value;
}

int main(int argc, char *argv[])
{
    const unsigned int duration = (argc > 1) ? parse_seconds(argv[1], 10) : 10;

    /* ── Task 5 addition: print identity info for experiment logs ──────── */
    printf("cpu_hog start pid=%d nice=%d duration=%u\n",
           getpid(), nice(0), duration);
    fflush(stdout);

    /* ── Task 5 addition: high-resolution start time for measurements ──── */
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* ── Original boilerplate — not modified ───────────────────────────── */
    const time_t start = time(NULL);
    time_t last_report = start;
    volatile unsigned long long accumulator = 0;
    while ((unsigned int)(time(NULL) - start) < duration) {
        accumulator = accumulator * 1664525ULL + 1013904223ULL;
        if (time(NULL) != last_report) {
            last_report = time(NULL);
            printf("cpu_hog alive elapsed=%ld accumulator=%llu\n",
                   (long)(last_report - start),
                   accumulator);
            fflush(stdout);
        }
    }
    printf("cpu_hog done duration=%u accumulator=%llu\n", duration, accumulator);
    /* ── End of original boilerplate ───────────────────────────────────── */

    /* ── Task 5 addition: report wall-clock elapsed time precisely ──────── */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (double)(ts_end.tv_sec  - ts_start.tv_sec)
                   + (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    printf("cpu_hog wall_time=%.6f s  pid=%d\n", elapsed, getpid());
    fflush(stdout);

    return 0;
}
