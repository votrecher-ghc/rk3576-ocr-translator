#ifndef OCR_COMM_MSGBUS_H
#define OCR_COMM_MSGBUS_H
/**
 * @file msgbus.h
 * @brief 消息总线：生产者互斥锁 + SPSC 环形缓冲 + eventfd 唤醒
 *
 * 用于线程间异步消息传递。生产者投递消息后通过 eventfd 唤醒消费者。
 * 支持多生产者单消费者（MPSC）模式。
 *
 * 消息总线以环形队列保存消息，以 eventfd 唤醒单个消费者。
 * 多个生产者由 producer_lock 串行化；eventfd 只是通知，消息内容始终以队列为准。
 */

#include <stdint.h>
#include <stddef.h>

/** 消息类型枚举 */
typedef enum {
    MSG_NONE = 0,
    MSG_FRAME_READY,      /* 新帧就绪（采集→管线） */
    MSG_FRAME_DISPLAYED,  /* 显示完成（display→采集回收） */
    MSG_OCR_DET_DONE,     /* 文字检测完成 */
    MSG_OCR_REC_DONE,     /* 文字识别完成 */
    MSG_TRANSLATE_DONE,   /* 翻译完成 */
    MSG_KEY_CAMERA,       /* 拍照按键 */
    MSG_KEY_MODE,         /* 模式切换按键 */
    MSG_LIGHT_UPDATE,     /* 环境光更新 */
    MSG_TEMP_UPDATE,      /* 温度更新 */
    MSG_SHUTDOWN,         /* 关机 */
    MSG_USER = 0x100,     /* 用户自定义起始 */
} ocr_msg_type_t;

/** 消息结构体（定长，便于环形缓冲管理） */
typedef struct {
    uint32_t type; /**< ocr_msg_type_t 或 MSG_USER 以上的业务类型，决定如何解释参数。 */
    uint32_t seq; /**< 发送者填写的业务序号；总线不自动递增，不等于队列物理槽位。 */
    int64_t  param_i; /**< 按消息类型解释的有符号整数参数，单位由发送/接收双方约定。 */
    void    *param_p; /**< 按协议传递的外部指针；总线只拷地址，不 ref/free，双方须另约定生命周期。 */
    uint64_t timestamp; /**< 发送者填写的时间戳；总线不生成或换算，时钟来源和单位须由业务协议固定。 */
} ocr_msg_t;

/** 消息总线上下文 */
typedef struct {
    int      eventfd; /**< 本总线拥有的非阻塞、信号量模式 eventfd，用于唤醒单个消费者；销毁关闭。 */
    int      epoll_fd; /**< 保留的可选字段，当前 init 置 -1；recv 实际使用 poll(eventfd)。 */
    void    *ring; /**< 拥有的私有 ocr_msgbus_priv_t 指针，包含 ring 和生产者锁；并非直接指向 ringbuffer。 */
    int      running; /**< 总线可收发标志，init=1、destroy=0；非原子，不作为并发销毁的安全屏障。 */
} ocr_msgbus_t;

/**
 * @brief 初始化消息总线
 * @param[out] bus 待初始化的总线，不得覆盖仍在使用的实例
 * @param[in] capacity 环形缓冲消息容量
 * @return 0=成功，负数=错误
 */
int ocr_msgbus_init(ocr_msgbus_t *bus, size_t capacity);

/**
 * @brief 销毁消息总线
 * @param[in,out] bus 已初始化的总线，允许为 NULL
 * @note 先停止并等待所有收发线程退出；本函数不会等待正在执行的 recv/post。
 *       队列中 param_p 指向的资源由业务层清理，总线只释放消息存储和 eventfd。
 */
void ocr_msgbus_destroy(ocr_msgbus_t *bus);

/**
 * @brief 投递消息（多生产者安全）
 * @param[in] bus  消息总线
 * @param[in] msg  消息内容
 * @return 0=成功，1=满，负数=错误
 * @note 复制消息结构体，不转移 param_p 所指资源的所有权。
 *       返回 0 表示已入队；通知写入失败只记日志，调用者不应因此重复投递。
 */
int ocr_msgbus_post(ocr_msgbus_t *bus, const ocr_msg_t *msg);

/**
 * @brief 从单消费者线程取消息，可按参数阻塞等待
 * @param[in,out] bus 已初始化且尚未销毁的总线
 * @param[out] msg 取出的消息
 * @param[in]  timeout_ms 超时毫秒（0=非阻塞，-1=永久阻塞）
 * @return 0=成功，1=超时/空，负数=错误
 * @note 仅返回 0 时 msg 有效。先查队列，再等通知；过期通知也可能使调用返回 1。
 *       EINTR 后重新使用完整 timeout_ms 等待，因此它不是严格的总耗时上限。
 */
int ocr_msgbus_recv(ocr_msgbus_t *bus, ocr_msg_t *msg, int timeout_ms);

/**
 * @brief 获取 eventfd（可加入外部 epoll）
 * @return 借用的描述符；bus 为 NULL 或已销毁时为 -1
 * @note 调用者不得关闭或自行读取该 fd；收到就绪事件后应通过 recv 取消息。
 */
int ocr_msgbus_get_fd(const ocr_msgbus_t *bus);

#endif /* OCR_COMM_MSGBUS_H */
