/**
 * @file system_manager.c
 * @brief 系统管理实现：信号处理/优雅退出/看门狗
 */
#include "system_manager.h"
#include "timestamp.h"
#include "log.h"

#include <string.h>
#include <signal.h>
#include <unistd.h>

/* 全局指针（信号处理函数需访问，不能传参） */
static ocr_system_manager_t *s_sm = NULL;

/* 信号处理函数 */
static void signal_handler(int sig)
{
    if (!s_sm) return;
    LOG_I("收到信号 %d，请求优雅退出", sig);
    ocr_system_manager_request_exit(s_sm, 0);
}

/* 看门狗线程入口 */
static void *watchdog_thread(void *arg)
{
    ocr_system_manager_t *sm = (ocr_system_manager_t *)arg;
    /* timeout 通过 user_ctx 传递不便，这里用固定 5 秒超时 */
    const uint32_t timeout_ms = 5000;
    uint64_t last_feed = 0;
    int last_count = 0;

    LOG_I("看门狗线程启动 (timeout=%ums)", timeout_ms);

    while (atomic_load(&sm->watchdog_running)) {
        int cur_count = atomic_load(&sm->feed_count);
        if (cur_count != last_count) {
            last_feed = timestamp_ms();
            last_count = cur_count;
        } else {
            /* 检查超时 */
            if (timestamp_diff_ms(last_feed, timestamp_ms()) > timeout_ms && last_feed != 0) {
                LOG_E("看门狗超时！强制退出");
                /* TODO: 可在此触发核心转储或重启 */
                ocr_system_manager_request_exit(sm, -1);
                break;
            }
        }
        timestamp_sleep_ns(500 * 1000000ULL); /* 500ms */
    }

    LOG_I("看门狗线程退出");
    return NULL;
}

int ocr_system_manager_init(ocr_system_manager_t *sm)
{
    if (!sm) return -1;
    memset(sm, 0, sizeof(*sm));
    atomic_init(&sm->state, SYS_STATE_INIT);
    atomic_init(&sm->exit_code, 0);
    atomic_init(&sm->feed_count, 0);
    atomic_init(&sm->watchdog_running, 0);
    sm->start_time = timestamp_ms();
    s_sm = sm;
    return 0;
}

int ocr_system_manager_install_signals(ocr_system_manager_t *sm)
{
    if (!sm) return -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        LOG_E("注册 SIGINT 失败");
        return -2;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        LOG_E("注册 SIGTERM 失败");
        return -3;
    }
    /* 忽略 SIGPIPE（socket 写对端关闭时触发） */
    signal(SIGPIPE, SIG_IGN);
    LOG_I("信号处理已注册 (SIGINT/SIGTERM)");
    return 0;
}

int ocr_system_manager_start_watchdog(ocr_system_manager_t *sm, uint32_t timeout_ms)
{
    if (!sm) return -1;
    (void)timeout_ms; /* 当前使用固定值，见线程函数 */
    atomic_store(&sm->watchdog_running, 1);
    ocr_system_manager_feed(sm); /* 初始喂狗 */
    if (ocr_thread_create(&sm->watchdog_tid, watchdog_thread, sm) != 0) {
        LOG_E("看门狗线程创建失败");
        return -2;
    }
    return 0;
}

void ocr_system_manager_feed(ocr_system_manager_t *sm)
{
    if (!sm) return;
    atomic_fetch_add(&sm->feed_count, 1);
}

void ocr_system_manager_request_exit(ocr_system_manager_t *sm, int code)
{
    if (!sm) return;
    atomic_store(&sm->exit_code, code);
    atomic_store(&sm->state, SYS_STATE_SHUTTING_DOWN);
}

int ocr_system_manager_should_exit(ocr_system_manager_t *sm)
{
    if (!sm) return 1;
    return atomic_load(&sm->state) >= SYS_STATE_SHUTTING_DOWN ? 1 : 0;
}

int ocr_system_manager_wait_exit(ocr_system_manager_t *sm)
{
    if (!sm) return -1;
    while (!ocr_system_manager_should_exit(sm)) {
        timestamp_sleep_ns(100 * 1000000ULL); /* 100ms */
    }
    return atomic_load(&sm->exit_code);
}

void ocr_system_manager_stop_watchdog(ocr_system_manager_t *sm)
{
    if (!sm) return;
    atomic_store(&sm->watchdog_running, 0);
    ocr_thread_join(sm->watchdog_tid, NULL);
}

void ocr_system_manager_destroy(ocr_system_manager_t *sm)
{
    if (!sm) return;
    ocr_system_manager_stop_watchdog(sm);
    atomic_store(&sm->state, SYS_STATE_STOPPED);
    s_sm = NULL;
}
