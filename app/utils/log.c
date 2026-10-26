/**
 * @file log.c
 * @brief 日志系统实现
 *
 * 线程安全日志：按级别过滤，记录时间、模块和源码位置，输出到标准错误或指定文件。
 * 全局锁串行化日志状态和输出；初始化/关闭文件句柄时也遵守同一把锁。
 */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <strings.h>
#include <errno.h>

/* 级别字符串与颜色 */
static const char *s_level_str[] = {"D", "I", "W", "E", "F"};
static const char *s_level_color[] = {
    "\033[37m", "\033[32m", "\033[33m", "\033[31m", "\033[41;37m"
};
#define COLOR_RESET "\033[0m"

static struct {
    char          name[32]; /**< 当前日志标签的值副本，最多 31 字节加 NUL；输出每条日志时读取。 */
    log_level_t   level; /**< 最小输出等级，初始化可受 LOG_LEVEL 环境变量覆盖；受全局 s_log_lock 保护。 */
    FILE         *fp; /**< 输出流，stderr 为借用，fopen 的日志文件由本模块关闭；不是同时输出两个目标。 */
    int           use_color; /**< 输出为终端 stderr 时可启用 ANSI 颜色，日志文件输出关闭颜色。 */
    int           inited; /**< 日志配置可用标志；与其他日志字段一起由 s_log_lock 保护。 */
} s_log;

static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_level(log_level_t level)
{
    return level >= LOG_LEVEL_DEBUG && level <= LOG_LEVEL_FATAL;
}

/** @brief 解析环境变量指定的日志级别，非法值回退到调用方给定级别。 */
static log_level_t level_from_env(log_level_t fallback)
{
    const char *value = getenv("LOG_LEVEL");
    if (!value || !*value) return fallback;

    if (strlen(value) == 1 && value[0] >= '0' && value[0] <= '4') {
        return (log_level_t)(value[0] - '0');
    }
    if (strcasecmp(value, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(value, "info") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(value, "warn") == 0 || strcasecmp(value, "warning") == 0)
        return LOG_LEVEL_WARN;
    if (strcasecmp(value, "error") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(value, "fatal") == 0) return LOG_LEVEL_FATAL;
    return fallback;
}

int log_init(const char *name, log_level_t level, int to_file, const char *path)
{
    if (!valid_level(level)) return -1;

    pthread_mutex_lock(&s_log_lock);
    if (s_log.inited && s_log.fp && s_log.fp != stderr) {
        fclose(s_log.fp);
    }
    memset(&s_log, 0, sizeof(s_log));
    s_log.level = level_from_env(level);

    if (name) {
        strncpy(s_log.name, name, sizeof(s_log.name) - 1);
    }

    if (to_file) {
        char default_path[96];
        if (!path || !*path) {
            snprintf(default_path, sizeof(default_path), "/tmp/%s.log",
                     s_log.name[0] ? s_log.name : "ocr");
            path = default_path;
        }
        s_log.fp = fopen(path, "a");
        if (!s_log.fp) {
            int saved_errno = errno;
            pthread_mutex_unlock(&s_log_lock);
            errno = saved_errno;
            return -2;
        }
        s_log.use_color = 0;
    } else {
        s_log.fp = stderr;
        s_log.use_color = isatty(fileno(stderr)) ? 1 : 0;
    }

    s_log.inited = 1;
    pthread_mutex_unlock(&s_log_lock);
    return 0;
}

void log_deinit(void)
{
    pthread_mutex_lock(&s_log_lock);
    if (s_log.fp && s_log.fp != stderr) {
        fclose(s_log.fp);
    }
    s_log.fp = NULL;
    s_log.inited = 0;
    pthread_mutex_unlock(&s_log_lock);
}

void log_set_level(log_level_t level)
{
    if (!valid_level(level)) return;
    pthread_mutex_lock(&s_log_lock);
    s_log.level = level;
    pthread_mutex_unlock(&s_log_lock);
}

/** @brief 在全局锁内读取配置并写出一条完整日志，避免多线程日志相互交错。 */
int log_write(log_level_t level, const char *file, int line,
              const char *func, const char *fmt, ...)
{
    if (!valid_level(level) || !fmt) return -1;

    pthread_mutex_lock(&s_log_lock);
    if (!s_log.inited || level < s_log.level) {
        pthread_mutex_unlock(&s_log_lock);
        return 0;
    }

    /* 时间戳 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    if (s_log.use_color) {
        fprintf(s_log.fp, "%s", s_level_color[level]);
    }
    const char *base = file ? strrchr(file, '/') : NULL;
    base = base ? base + 1 : (file ? file : "?");
    fprintf(s_log.fp, "[%04d-%02d-%02d %02d:%02d:%02d.%03ld][%s][%s][%s:%d %s] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
            s_level_str[level], s_log.name[0] ? s_log.name : "ocr",
            base, line, func ? func : "?");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log.fp, fmt, ap);
    va_end(ap);

    if (s_log.use_color) {
        fprintf(s_log.fp, "%s", COLOR_RESET);
    }
    fprintf(s_log.fp, "\n");
    fflush(s_log.fp);

    pthread_mutex_unlock(&s_log_lock);

    return 0;
}
