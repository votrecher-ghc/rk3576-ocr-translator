#ifndef OCR_UTILS_TIMESTAMP_H
#define OCR_UTILS_TIMESTAMP_H
/**
 * @file timestamp.h
 * @brief 时间戳工具：单调时钟/墙钟/毫秒/微秒
 *
 * 使用 CLOCK_MONOTONIC 生成耗时统计时间戳，避免系统日期校准导致时间倒退。
 * 接口名称区分毫秒、微秒和纳秒；休眠遇到信号中断时继续等待剩余时长。
 */

#include <stdint.h>
#include <time.h>

/**
 * @brief 获取单调时钟毫秒时间戳（不受系统时间调整影响）
 * @return 毫秒时间戳
 */
uint64_t timestamp_ms(void);

/**
 * @brief 获取单调时钟微秒时间戳
 * @return 微秒时间戳
 */
uint64_t timestamp_us(void);

/**
 * @brief 计算两个毫秒时间戳之差
 * @return diff = end - start（毫秒）
 */
uint64_t timestamp_diff_ms(uint64_t start, uint64_t end);

/**
 * @brief 纳秒级睡眠
 */
void timestamp_sleep_ns(uint64_t ns);

#endif /* OCR_UTILS_TIMESTAMP_H */
