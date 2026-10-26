#ifndef OCR_PIPELINE_NODE_H
#define OCR_PIPELINE_NODE_H

/**
 * @file node.h
 * @brief Pipeline worker node, bounded input queue, and fan-out handling.
 *
 * 每个节点拥有工作线程和有界输入队列；入队传递一个引用，队列满时丢弃并释放最旧帧。
 * 回调借用输入；返回 0 转发原图，正数表示已自行输出，负数表示丢帧。分发每个下游都独立持有引用。
 */

#include "buffer.h"
#include "thread.h"

#include <stdatomic.h>

#define OCR_NODE_NAME_LEN 32
#define OCR_NODE_QUEUE_DEPTH 8
#define OCR_NODE_MAX_DOWNSTREAM 4

struct ocr_pipeline_node;

/**
 * @brief 节点处理回调，输入 buf 只在回调期间借用，由工作线程统一 unref。
 * @return 负数：丢弃；0：转发原输入；正数：回调已自行输出，不再转发原输入。
 * @note 需要在返回后继续使用输入时必须先 ref；生成新图像通常通过 emit 发布。
 */
typedef int (*ocr_node_process_fn)(struct ocr_pipeline_node *node,
                                   ocr_buffer_t *buf);

typedef struct ocr_pipeline_node {
    char                name[OCR_NODE_NAME_LEN]; /**< 节点日志名称，初始化时复制，含结尾 NUL。 */
    ocr_node_process_fn process; /**< 在本节点工作线程中执行的处理函数；NULL 表示直接转发输入。 */
    void               *user_ctx; /**< 借用的业务上下文，传给 process；节点不释放，必须活到线程结束。 */

    ocr_buffer_t       *in_queue[OCR_NODE_QUEUE_DEPTH]; /* 每个队列项持有一个引用。 */
    int                 in_head;   /* 下一次出队的位置。 */
    int                 in_tail;   /* 下一次入队的位置。 */
    ocr_mutex_t         in_lock;   /* 保护队列内容和头尾索引，不用于等待 DMA 完成。 */
    ocr_cond_t          in_cond;   /* 入队或停止时唤醒工作线程。 */
    atomic_int          in_count;  /* 当前排队数，不包含正在处理的帧。 */

    struct ocr_pipeline_node *downstream[OCR_NODE_MAX_DOWNSTREAM]; /* 启动前建立拓扑。 */
    int                 downstream_count; /**< downstream 的有效项数，最多 4 个；启动前固定。 */

    ocr_mutex_t         lifecycle_lock; /**< 串行化 link/start/stop，区别于保护输入队列的 in_lock。 */
    ocr_thread_t        thread; /**< 本节点工作线程句柄；thread_started=1 时仍需 join 回收。 */
    atomic_int          initialized; /**< 锁、条件变量及队列可用的标志；销毁后为 0。 */
    atomic_int          thread_started; /**< 线程已创建且尚未 join；线程自行退出后也可能仍为 1。 */
    atomic_int          running; /**< 工作循环的继续运行请求；置 0 后还要唤醒条件变量并 join。 */
    atomic_int          processed; /**< 回调返回非负的输入帧累计数，不等于成功显示或识别的帧数。 */
    atomic_int          dropped; /**< 队列淘汰、回调失败等丢帧统计；由工作线程和入队线程原子累加。 */
} ocr_pipeline_node_t;

/**
 * @brief 初始化空输入队列和同步对象，尚不创建工作线程
 * @param[out] node 待初始化的节点；已有实例须先销毁
 * @param[in] name 非空日志名称，最多保存 OCR_NODE_NAME_LEN-1 字节
 * @param[in] process 工作线程中的处理回调；NULL 表示转发原输入
 * @param[in] ctx 借用的业务上下文，节点不释放
 * @return 0=成功，-1=参数无效，-2=同步对象初始化失败
 */
int ocr_node_init(ocr_pipeline_node_t *node, const char *name,
                  ocr_node_process_fn process, void *ctx);

/**
 * @brief 在源节点启动前连接下游；重复边返回成功，不重复添加
 * @return 0=成功，-1=无效节点或自连，-2=源节点已启动，-3=下游数量已达上限
 * @note 借用下游地址，不延长节点寿命；整张图无环由装配层保证。
 */
int ocr_node_link(ocr_pipeline_node_t *node,
                  ocr_pipeline_node_t *downstream);

/**
 * @brief 把调用者已有的一份引用转移到输入队列，不复制像素，也不额外 ref。
 * @return 0 接管成功；负数 未接管，调用者仍需释放或另作处理。
 * @note 队列满时淘汰最旧项并接收本项，旧引用在解锁后释放，不以队列满阻塞生产者。
 *       队列项和正在执行的回调各自持有引用；工作线程从队列取出时转移而不新增引用。
 */
int ocr_node_push_input(ocr_pipeline_node_t *node, ocr_buffer_t *buf);

/**
 * @brief 将新生成的图像分发给所有下游，每个成功接收者各拥有一个引用。
 * @return 非负数为成功接收的下游数，原引用已经消费；负数为参数错误，原引用未消费。
 * @note 返回 0 也不能再 unref，图像可能已经回到池中。只有负数才能由调用者补释放。
 *       传入借用的 input 前必须额外 ref，不能把节点自己持有的输入引用误交给 emit。
 */
int ocr_node_emit(ocr_pipeline_node_t *node, ocr_buffer_t *buf);

/** @brief 等输入、执行借用式回调、按返回值转发，最后统一释放输入引用。 */
void *ocr_node_thread_entry(void *arg);
/** @brief 创建工作线程；已启动时返回 0，参数无效返回 -1，创建失败返回 -2。 */
int ocr_node_start(ocr_pipeline_node_t *node);
/**
 * @brief 请求退出并等待工作线程结束，未启动时直接返回成功
 * @return 0=已停止，-1=参数无效，-2=不能 join 自己或 join 失败，可由管理线程重试
 * @note 队列剩余项仍持有引用，stop 不清空队列；destroy 才释放这些引用。
 */
int ocr_node_stop(ocr_pipeline_node_t *node);

/** Stop, drain, and release node synchronization objects; safe to repeat.
 *
 * 停止线程并释放队列剩余帧，再销毁锁；不允许在线程自己的回调中完成销毁。
 */
void ocr_node_destroy(ocr_pipeline_node_t *node);

#endif /* OCR_PIPELINE_NODE_H */
