#ifndef OCR_UTILS_LOG_H
#define OCR_UTILS_LOG_H
/**
 * @file log.h
 * @brief 日志系统：支持级别/时间戳/文件行号/颜色输出
 */

#include <stdint.h>

/** 日志级别枚举 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
} log_level_t;

/**
 * @brief 初始化日志系统
 * @param[in] name   模块名（打印前缀）
 * @param[in] level  最小输出级别
 * @param[in] to_file 是否写入文件（0=stderr，1=文件）
 * @param[in] path   日志文件路径（to_file=1 时有效，可传 NULL）
 * @return 0=成功，负数=错误
 */
int log_init(const char *name, log_level_t level, int to_file, const char *path);

/**
 * @brief 反初始化日志系统
 */
void log_deinit(void);

/**
 * @brief 设置日志级别
 */
void log_set_level(log_level_t level);

/** 内部宏：统一日志输出入口 */
#define LOG_RAW(lvl, fmt, ...) \
    log_write((lvl), __FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__)

/** 各级别日志宏 */
#define LOG_D(fmt, ...) LOG_RAW(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) LOG_RAW(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) LOG_RAW(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) LOG_RAW(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_F(fmt, ...) LOG_RAW(LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__)

/**
 * @brief 日志写入核心函数（通常通过宏调用）
 * @param[in] level  日志级别
 * @param[in] file   源文件名
 * @param[in] line   行号
 * @param[in] func   函数名
 * @param[in] fmt    printf 风格格式串
 * @return 0=成功，负数=错误
 */
int log_write(log_level_t level, const char *file, int line,
              const char *func, const char *fmt, ...);

#endif /* OCR_UTILS_LOG_H */
