#ifndef OCR_PIPELINE_NODE_H
#define OCR_PIPELINE_NODE_H
/**
 * @file node.h
 * @brief 管线节点基类：输入队列/输出队列/处理回调
 *
 * 每个节点拥有一个输入队列和一个输出队列，
 * 输入到达后调用 process 回调进行处理，处理完投递到下游节点。
 */

#include "buffer.h"
#include "thread.h"
#include <stdatomic.h>

#define OCR_NODE_NAME_LEN 32
#define OCR_NODE_QUEUE_DEPTH 8

struct ocr_pipeline_node;

/** 节点处理回调函数原型
 * @param node   节点自身
 * @param buf    输入缓冲
 * @return 0=成功处理，负数=错误（丢弃该帧）
 */
typedef int (*ocr_node_process_fn)(struct ocr_pipeline_node *node, ocr_buffer_t *buf);

/** 管线节点 */
typedef struct ocr_pipeline_node {
    char                name[OCR_NODE_NAME_LEN]; /* 节点名 */
    ocr_node_process_fn process;                 /* 处理回调 */
    void               *user_ctx;                /* 用户上下文 */

    /* 输入队列（简单的有界数组 + 互斥锁） */
    ocr_buffer_t       *in_queue[OCR_NODE_QUEUE_DEPTH];
    int                 in_head, in_tail;
    ocr_mutex_t         in_lock;
    ocr_cond_t          in_cond;
    atomic_int          in_count;

    /* 输出：下游节点数组 */
    struct ocr_pipeline_node *downstream[4];
    int                 downstream_count;

    /* 线程 */
    ocr_thread_t        thread;
    atomic_int          running;
    atomic_int          processed;   /* 已处理帧数 */
    atomic_int          dropped;     /* 丢弃帧数 */
} ocr_pipeline_node_t;

/**
 * @brief 初始化节点
 * @param[in] node    节点
 * @param[in] name    节点名
 * @param[in] process 处理回调
 * @param[in] ctx     用户上下文
 * @return 0=成功，负数=错误
 */
int ocr_node_init(ocr_pipeline_node_t *node, const char *name,
                  ocr_node_process_fn process, void *ctx);

/**
 * @brief 连接：node 的输出接到 downstream
 */
int ocr_node_link(ocr_pipeline_node_t *node, ocr_pipeline_node_t *downstream);

/**
 * @brief 向节点输入队列投递缓冲（非阻塞，满则丢弃旧帧）
 */
int ocr_node_push_input(ocr_pipeline_node_t *node, ocr_buffer_t *buf);

/**
 * @brief 节点工作线程入口（供 ocr_thread_create 使用）
 */
void *ocr_node_thread_entry(void *arg);

/**
 * @brief 启动节点线程
 */
int ocr_node_start(ocr_pipeline_node_t *node);

/**
 * @brief 停止节点线程
 */
int ocr_node_stop(ocr_pipeline_node_t *node);

/**
 * @brief 销毁节点
 */
void ocr_node_destroy(ocr_pipeline_node_t *node);

#endif /* OCR_PIPELINE_NODE_H */
