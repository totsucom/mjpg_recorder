#ifndef __MYTIME_H
#define __MYTIME_H
#include <time.h>
#include <stdbool.h>

/*
 * 時刻とは無関係な経過時間処理クラス
 */

#define get_current_clock(pClock) clock_gettime(CLOCK_MONOTONIC_RAW, pClock)

void add_clock(struct timespec *clock, time_t sec, long nsec);
void get_future_clock(struct timespec *clock, time_t sec, long nsec);
bool clock_passed(const struct timespec *clock);
double elapsed_clock(const struct timespec *start);
double elapsed_time(time_t start, time_t end, char unit);

#endif