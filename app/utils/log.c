/**
 * @file log.c
 * @brief 日志系统实现
 */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

/* 级别字符串与颜色 */
static const char *s_level_str[] = {"D", "I", "W", "E", "F"};
static const char *s_level_color[] = {
    "\033[37m", "\033[32m", "\033[33m", "\033[31m", "\033[41;37m"
};
#define COLOR_RESET "\033[0m"

static struct {
    char          name[32];   /* 模块名 */
    log_level_t   level;      /* 最小输出级别 */
    FILE         *fp;         /* 输出文件 */
    int           use_color;  /* 是否启用颜色（终端） */
    pthread_mutex_t lock;     /* 多线程写互斥 */
    int           inited;     /* 是否已初始化 */
} s_log;

int log_init(const char *name, log_level_t level, int to_file, const char *path)
{
    memset(&s_log, 0, sizeof(s_log));
    s_log.level = level;
    s_log.use_color = 0;

    if (name) {
        strncpy(s_log.name, name, sizeof(s_log.name) - 1);
    }

    if (to_file && path) {
        s_log.fp = fopen(path, "a");
        if (!s_log.fp) {
            s_log.fp = stderr;
        }
        s_log.use_color = 0;
    } else {
        s_log.fp = stderr;
        s_log.use_color = isatty(fileno(stderr)) ? 1 : 0;
    }

    pthread_mutex_init(&s_log.lock, NULL);
    s_log.inited = 1;
    return 0;
}

void log_deinit(void)
{
    if (!s_log.inited) return;
    pthread_mutex_lock(&s_log.lock);
    if (s_log.fp && s_log.fp != stderr) {
        fclose(s_log.fp);
    }
    s_log.fp = NULL;
    pthread_mutex_unlock(&s_log.lock);
    pthread_mutex_destroy(&s_log.lock);
    s_log.inited = 0;
}

void log_set_level(log_level_t level)
{
    s_log.level = level;
}

int log_write(log_level_t level, const char *file, int line,
              const char *func, const char *fmt, ...)
{
    if (!s_log.inited || level < s_log.level) {
        return 0;
    }

    /* 时间戳 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    pthread_mutex_lock(&s_log.lock);

    if (s_log.use_color) {
        fprintf(s_log.fp, "%s", s_level_color[level]);
    }
    fprintf(s_log.fp, "[%04d-%02d-%02d %02d:%02d:%02d.%03ld][%s][%s:%d %s] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
            s_level_str[level], file, line, func);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log.fp, fmt, ap);
    va_end(ap);

    if (s_log.use_color) {
        fprintf(s_log.fp, "%s", COLOR_RESET);
    }
    fprintf(s_log.fp, "\n");
    fflush(s_log.fp);

    pthread_mutex_unlock(&s_log.lock);

    /* FATAL 级别直接退出 */
    if (level == LOG_LEVEL_FATAL) {
        abort();
    }
    return 0;
}
