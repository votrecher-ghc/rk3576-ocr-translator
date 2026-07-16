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
    char          name[32];   /* 模块名 */
    log_level_t   level;      /* 最小输出级别 */
    FILE         *fp;         /* 输出文件 */
    int           use_color;  /* 是否启用颜色（终端） */
    int           inited;     /* 是否已初始化 */
} s_log;

static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_level(log_level_t level)
{
    return level >= LOG_LEVEL_DEBUG && level <= LOG_LEVEL_FATAL;
}

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
