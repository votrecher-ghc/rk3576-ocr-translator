#ifndef OCR_MAIN_SYSTEM_MANAGER_H
#define OCR_MAIN_SYSTEM_MANAGER_H
/**
 * @file system_manager.h
 * @brief 系统管理：信号处理(SIGINT/SIGTERM)、优雅退出、看门狗
 *
 * 统一管理应用生命周期：信号处理、优雅退出标志、看门狗监控线程。
 *
 * 系统生命周期、退出信号和软件看门狗管理。
 * 信号处理只记录退出信号，常规线程再处理退出；看门狗检查主循环喂狗时间并请求统一退出。
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
    atomic_int      state; /**< 原子 sys_state_t 状态；大于等于 SHUTTING_DOWN 表示主循环应退出。 */
    atomic_int      exit_code; /**< 原子退出码，request_exit 写入、主循环最终返回；正常通常为 0。 */
    atomic_int      feed_count; /**< 原子喂狗次数，每次 feed 增 1；看门狗观察是否变化，不是上次喂狗时间戳。 */
    ocr_thread_t    watchdog_tid; /**< 软件看门狗线程句柄，start 创建、stop 在清运行标志后 join。 */
    atomic_int      watchdog_running; /**< 原子看门狗运行标志，用于重复启动保护及退出循环请求。 */
    uint32_t        watchdog_timeout_ms; /**< 允许喂狗计数不变化的最长时间，毫秒；在线程启动前设置。 */
    uint64_t        start_time; /**< 系统管理器初始化时的 CLOCK_MONOTONIC 毫秒值，不是墙上日期时间。 */
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
