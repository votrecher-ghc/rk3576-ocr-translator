#ifndef OCR_UTILS_THREAD_H
#define OCR_UTILS_THREAD_H
/**
 * @file thread.h
 * @brief 线程原语封装：mutex/cond/semaphore/thread
 *
 * 基于 pthread 的轻量封装，提供统一命名与错误检查。
 */

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>

/* -------------------- 互斥锁 -------------------- */
typedef pthread_mutex_t ocr_mutex_t;

/** 初始化互斥锁 */
static inline int ocr_mutex_init(ocr_mutex_t *m) {
    return pthread_mutex_init(m, NULL) == 0 ? 0 : -1;
}
/** 销毁互斥锁 */
static inline int ocr_mutex_destroy(ocr_mutex_t *m) {
    return pthread_mutex_destroy(m) == 0 ? 0 : -1;
}
/** 加锁 */
static inline int ocr_mutex_lock(ocr_mutex_t *m) {
    return pthread_mutex_lock(m) == 0 ? 0 : -1;
}
/** 解锁 */
static inline int ocr_mutex_unlock(ocr_mutex_t *m) {
    return pthread_mutex_unlock(m) == 0 ? 0 : -1;
}

/* -------------------- 条件变量 -------------------- */
typedef pthread_cond_t ocr_cond_t;

static inline int ocr_cond_init(ocr_cond_t *c) {
    return pthread_cond_init(c, NULL) == 0 ? 0 : -1;
}
static inline int ocr_cond_destroy(ocr_cond_t *c) {
    return pthread_cond_destroy(c) == 0 ? 0 : -1;
}
static inline int ocr_cond_wait(ocr_cond_t *c, ocr_mutex_t *m) {
    return pthread_cond_wait(c, m) == 0 ? 0 : -1;
}
/** 超时等待（毫秒） */
static inline int ocr_cond_wait_ms(ocr_cond_t *c, ocr_mutex_t *m, uint32_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int ret = pthread_cond_timedwait(c, m, &ts);
    return ret == 0 ? 0 : (ret == ETIMEDOUT ? 1 : -1);
}
static inline int ocr_cond_signal(ocr_cond_t *c) {
    return pthread_cond_signal(c) == 0 ? 0 : -1;
}
static inline int ocr_cond_broadcast(ocr_cond_t *c) {
    return pthread_cond_broadcast(c) == 0 ? 0 : -1;
}

/* -------------------- 信号量 -------------------- */
typedef sem_t ocr_sem_t;

static inline int ocr_sem_init(ocr_sem_t *s, uint32_t init_val) {
    return sem_init(s, 0, init_val) == 0 ? 0 : -1;
}
static inline int ocr_sem_destroy(ocr_sem_t *s) {
    return sem_destroy(s) == 0 ? 0 : -1;
}
static inline int ocr_sem_wait(ocr_sem_t *s) {
    return sem_wait(s) == 0 ? 0 : -1;
}
/** 超时等待（毫秒） */
static inline int ocr_sem_wait_ms(ocr_sem_t *s, uint32_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int ret = sem_timedwait(s, &ts);
    return ret == 0 ? 0 : (errno == ETIMEDOUT ? 1 : -1);
}
static inline int ocr_sem_post(ocr_sem_t *s) {
    return sem_post(s) == 0 ? 0 : -1;
}

/* -------------------- 线程 -------------------- */
typedef pthread_t ocr_thread_t;

/**
 * @brief 创建并启动线程
 * @param[out] t       线程句柄
 * @param[in]  routine 线程入口函数
 * @param[in]  arg     传入参数
 * @return 0=成功，负数=错误
 */
static inline int ocr_thread_create(ocr_thread_t *t,
                                    void *(*routine)(void *), void *arg) {
    return pthread_create(t, NULL, routine, arg) == 0 ? 0 : -1;
}

/** 等待线程结束 */
static inline int ocr_thread_join(ocr_thread_t t, void **retval) {
    return pthread_join(t, retval) == 0 ? 0 : -1;
}

/** 线程自旋等待退出标志的轻量辅助宏 */
#define OCR_THREAD_LOOP_RUN(flag) (*(volatile int *)(flag))

#endif /* OCR_UTILS_THREAD_H */
