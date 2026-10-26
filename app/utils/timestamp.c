/**
 * @file timestamp.c
 * @brief 时间戳工具实现
 *
 * 使用 CLOCK_MONOTONIC 生成耗时统计时间戳，避免系统日期校准导致时间倒退。
 * 接口名称区分毫秒、微秒和纳秒；休眠遇到信号中断时继续等待剩余时长。
 */
#include "timestamp.h"

#include <errno.h>

uint64_t timestamp_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

uint64_t timestamp_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

uint64_t timestamp_diff_ms(uint64_t start, uint64_t end)
{
    return (end >= start) ? (end - start) : 0;
}

void timestamp_sleep_ns(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* 使用剩余时间继续睡眠。 */
    }
}
