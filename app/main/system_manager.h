#ifndef OCR_MAIN_SYSTEM_MANAGER_H
#define OCR_MAIN_SYSTEM_MANAGER_H
/**
 * @file system_manager.h
 * @brief 系统管理：信号处理(SIGINT/SIGTERM)、优雅退出、看门狗
 *
 * 统一管理应用生命周期：信号处理、优雅退出标志、看门狗监控线程。
 */

#include <stdatomic.h>
#include "thread.h"

/** 系统状态 */
typedef enum {
    SYS_STATE_INIT = 0,
    SYS_STATE_RUNNING,
    SYS_STATE_SHUTTING_DOWN,
    SYS_STATE_STOPPED,
} sys_state_t;

/** 系统管理器上下文 */
typedef struct {
    atomic_int      state;        /* 当前状态 */
    atomic_int      exit_code;    /* 退出码 */
    atomic_int      feed_count;   /* 看门狗喂狗计数 */
    ocr_thread_t    watchdog_tid; /* 看门狗线程 */
    atomic_int      watchdog_running; /* 看门狗运行标志 */
    uint32_t        watchdog_timeout_ms; /* 看门狗超时 */
    uint64_t        start_time;   /* 启动时间 */
} ocr_system_manager_t;

/**
 * @brief 初始化系统管理器
 * @param[in] sm 系统管理器
 * @return 0=成功，负数=错误
 */
int ocr_system_manager_init(ocr_system_manager_t *sm);

/**
 * @brief 注册信号处理（SIGINT/SIGTERM）
 */
int ocr_system_manager_install_signals(ocr_system_manager_t *sm);

/**
 * @brief 启动看门狗线程
 * @param[in] sm 系统管理器
 * @param[in] timeout_ms 超时阈值（毫秒，超过未喂狗则强制退出）
 */
int ocr_system_manager_start_watchdog(ocr_system_manager_t *sm, uint32_t timeout_ms);

/**
 * @brief 喂狗（重置看门狗计时）
 */
void ocr_system_manager_feed(ocr_system_manager_t *sm);

/**
 * @brief 请求优雅退出
 * @param[in] sm 系统管理器
 * @param[in] code 退出码
 */
void ocr_system_manager_request_exit(ocr_system_manager_t *sm, int code);

/**
 * @brief 检查是否请求退出
 * @return 1=已请求退出，0=未请求
 */
int ocr_system_manager_should_exit(ocr_system_manager_t *sm);

/**
 * @brief 等待退出请求（阻塞）
 */
int ocr_system_manager_wait_exit(ocr_system_manager_t *sm);

/**
 * @brief 停止看门狗
 */
void ocr_system_manager_stop_watchdog(ocr_system_manager_t *sm);

/**
 * @brief 销毁系统管理器
 */
void ocr_system_manager_destroy(ocr_system_manager_t *sm);

#endif /* OCR_MAIN_SYSTEM_MANAGER_H */
