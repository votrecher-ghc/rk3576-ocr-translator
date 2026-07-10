#ifndef OCR_COMM_MSGBUS_H
#define OCR_COMM_MSGBUS_H
/**
 * @file msgbus.h
 * @brief 消息总线：无锁环形缓冲 + eventfd 唤醒
 *
 * 用于线程间异步消息传递。生产者投递消息后通过 eventfd 唤醒消费者。
 * 支持多生产者单消费者（MPSC）模式。
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
    uint32_t type;    /* ocr_msg_type_t */
    uint32_t seq;     /* 序列号 */
    int64_t  param_i; /* 整型参数 */
    void    *param_p; /* 指针参数（如 buffer 句柄） */
    uint64_t timestamp;/* 时间戳 */
} ocr_msg_t;

/** 消息总线上下文 */
typedef struct {
    int      eventfd;    /* 唤醒 eventfd */
    int      epoll_fd;   /* 可选：内嵌 epoll */
    void    *ring;       /* 内部环形缓冲（ocr_ringbuffer_t*） */
    int      running;    /* 运行标志 */
} ocr_msgbus_t;

/**
 * @brief 初始化消息总线
 * @param[in] capacity 环形缓冲消息容量
 * @return 0=成功，负数=错误
 */
int ocr_msgbus_init(ocr_msgbus_t *bus, size_t capacity);

/**
 * @brief 销毁消息总线
 */
void ocr_msgbus_destroy(ocr_msgbus_t *bus);

/**
 * @brief 投递消息（多生产者安全）
 * @param[in] bus  消息总线
 * @param[in] msg  消息内容
 * @return 0=成功，1=满，负数=错误
 */
int ocr_msgbus_post(ocr_msgbus_t *bus, const ocr_msg_t *msg);

/**
 * @brief 取消息（阻塞等待）
 * @param[out] msg 取出的消息
 * @param[in]  timeout_ms 超时毫秒（0=非阻塞，-1=永久阻塞）
 * @return 0=成功，1=超时/空，负数=错误
 */
int ocr_msgbus_recv(ocr_msgbus_t *bus, ocr_msg_t *msg, int timeout_ms);

/**
 * @brief 获取 eventfd（可加入外部 epoll）
 */
int ocr_msgbus_get_fd(const ocr_msgbus_t *bus);

#endif /* OCR_COMM_MSGBUS_H */
