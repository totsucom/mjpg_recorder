#include "mytime.h"

void add_clock(struct timespec *clock, time_t sec, long nsec) {
    if (!clock) return;
    clock->tv_sec += sec;
    clock->tv_nsec += nsec;
    while (clock->tv_nsec >= 1000000000) {
        clock->tv_nsec -= 1000000000;
        ++clock->tv_sec;
    }
}

void get_future_clock(struct timespec *clock, time_t sec, long nsec) {
    if (!clock) return;
    clock_gettime(CLOCK_MONOTONIC_RAW, clock);
    add_clock(clock, sec, nsec);
}

bool clock_passed(const struct timespec *clock) {
    struct timespec c;
    clock_gettime(CLOCK_MONOTONIC_RAW, &c);
    return ((c.tv_sec > clock->tv_sec) ||
        (c.tv_sec == clock->tv_sec && c.tv_nsec >= clock->tv_nsec));
}

double elapsed_clock(const struct timespec *start) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    double elapsed = (end.tv_sec - start->tv_sec);
    elapsed += (double)(end.tv_nsec - start->tv_nsec) / 1000000000.0f;
    return elapsed;
}

double elapsed_time(time_t start, time_t end, char unit) {
    double sa = end - start;
    switch (unit) {
        case 's': break;
        case 'm': sa /= 60.0; break;
        case 'h': sa /= 3600.0; break;
        case 'd': sa /= 86400.0; break;
    }
    return sa;
}
