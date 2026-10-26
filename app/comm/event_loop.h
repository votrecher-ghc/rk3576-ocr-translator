#ifndef OCR_COMM_EVENT_LOOP_H
#define OCR_COMM_EVENT_LOOP_H
/**
 * @file event_loop.h
 * @brief 事件循环：epoll 封装
 *
 * 单线程事件循环，监听多个 fd（eventfd/input/iio 等），
 * 事件就绪时调用注册的回调。
 *
 * epoll 事件循环：注册 fd 与回调，等待可读/可写事件后分发。
 * 内部 eventfd 用于唤醒阻塞的 epoll_wait，使另一线程发出的 stop 请求及时生效。
 */

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/** 事件回调函数原型
 * @param fd     就绪的文件描述符
 * @param events epoll 事件掩码
 * @param user   用户自定义上下文
 * @note 在 run 所在线程同步执行；耗时操作会推迟其他 fd 的分发及 stop 生效。
 *       回调可调用 add/mod/del/stop；不能在回调尚未返回时销毁循环。
 */
typedef void (*ocr_event_cb_t)(int fd, uint32_t events, void *user);

/** 事件项 */
typedef struct {
    int            fd; /**< 借用的被监听 fd，循环不负责关闭；其有效期由注册者管理。 */
    uint32_t       events; /**< epoll 关注事件位掩码，如 EPOLLIN/EPOLLET；不是事件计数。 */
    ocr_event_cb_t cb; /**< 事件就绪时在运行 loop 的线程中同步调用的函数，不能为 NULL。 */
    void          *user; /**< 借给 cb 的业务上下文，循环不释放；删除注册后仍须等待已开始回调结束。 */
} ocr_event_t;

/** 事件循环上下文 */
typedef struct {
    int          epoll_fd; /**< 本循环拥有的 epoll 描述符，init 创建，destroy 关闭；-1 表示未建立。 */
    int          wake_fd; /**< 本循环拥有的非阻塞 eventfd，stop 写入以唤醒 epoll_wait，销毁时关闭。 */
    atomic_int   running; /**< run/stop 跨线程共享的原子运行标志；不表示对象可在回调未结束时销毁。 */
    int          max_events; /**< 单次 epoll_wait 最多返回的事件条数，决定临时数组大小；不是注册总数上限。 */
    void        *impl; /**< 拥有的私有 ocr_event_loop_impl_t，内含注册表及锁；destroy 释放。 */
} ocr_event_loop_t;

/**
 * @brief 初始化事件循环
 * @param[in] max_events 单次 epoll_wait 最大返回事件数
 * @return 0=成功，负数=错误
 */
int ocr_event_loop_init(ocr_event_loop_t *loop, int max_events);

/**
 * @brief 销毁事件循环
 * @param[in,out] loop 已初始化的循环，允许为 NULL
 * @note stop 只请求退出。调用者须先等待运行 run 的线程结束，再释放循环和 user。
 *       这里只关闭内部 epoll/eventfd，不关闭业务注册的 fd。
 */
void ocr_event_loop_destroy(ocr_event_loop_t *loop);

/**
 * @brief 添加 fd 监听
 * @param[in] loop   事件循环
 * @param[in] ev     事件描述（fd/events/cb/user）
 * @return 0=成功，负数=错误
 * @note 按值复制 ev，借用其中的 fd/user；注册者须维持它们的有效期。
 */
int ocr_event_loop_add(ocr_event_loop_t *loop, const ocr_event_t *ev);

/**
 * @brief 删除 fd 监听
 * @return 0=删除成功，-1=参数无效，-2=未找到活动注册，-3=内核删除失败
 * @note 返回成功不等待已取得回调快照的线程；释放 user 前仍须确认旧回调结束。
 */
int ocr_event_loop_del(ocr_event_loop_t *loop, int fd);

/**
 * @brief 修改已注册 fd 的事件掩码、回调或用户数据
 * @return 0=成功，-1=参数无效，-2=未找到活动注册，-3=内核修改失败
 * @note 已取得的旧回调快照仍可能执行，旧 user 不能在 mod 返回后立即无条件释放。
 */
int ocr_event_loop_mod(ocr_event_loop_t *loop, const ocr_event_t *ev);

/**
 * @brief 运行事件循环（阻塞直到 ocr_event_loop_stop 被调用）
 * @return 0=正常退出，-1=参数无效，-2=已有运行者，-3=分配失败，-4=等待失败
 * @note 同一循环只允许一个运行线程；每次进入 run 都重新设置 running。
 *       调用者应协调启动和停止，不能把 run 之前的 stop 当作持久取消请求。
 */
int ocr_event_loop_run(ocr_event_loop_t *loop);

/**
 * @brief 停止事件循环
 * @note 可由回调或其他线程请求退出并唤醒 epoll_wait；不 join 线程，也不释放对象。
 */
void ocr_event_loop_stop(ocr_event_loop_t *loop);

#endif /* OCR_COMM_EVENT_LOOP_H */
