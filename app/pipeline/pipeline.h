#ifndef OCR_PIPELINE_PIPELINE_H
#define OCR_PIPELINE_PIPELINE_H

/** @file pipeline.h @brief Pipeline topology lifecycle orchestration.
 *
 * 管线对象登记外部提供的节点并统一启动、停止和销毁；节点间数据传递由 node 模块负责。
 * 启动失败会回滚已启动的节点；停止/销毁按登记的逆序执行，调用方需先停止上游继续投递。
 * 管线管理接口不含内部锁，须由同一管理线程调用或由上层串行化。
 */

#include "node.h"

#define OCR_PIPELINE_MAX_NODES 16

typedef struct {
    ocr_pipeline_node_t *nodes[OCR_PIPELINE_MAX_NODES]; /**< 借用的节点地址；管线不分配或 free 节点本体。 */
    int                  node_count; /**< 已登记节点数，nodes[0..node_count) 有效，最多 16 个。 */
    int                  started_count; /**< 已成功启动的节点前缀长度，供启动回滚和逆序停止使用。 */
    int                  started; /**< 整体启动/待停止标志；停止失败时保留 1，以便外部重试 join。 */
} ocr_pipeline_t;

/** @brief 清空管线登记表；返回 0，pl 为 NULL 时返回 -1；已有管线须先销毁。 */
int ocr_pipeline_init(ocr_pipeline_t *pl);
/**
 * @brief 在启动前登记节点地址，不复制节点；重复登记返回成功
 * @return 0=成功，-1=参数或节点无效，-2=管线已启动或待停止，-3=节点数量已满
 * @note 不建立节点之间的连接，也不排序；拓扑由 ocr_node_link 和登记顺序共同决定。
 */
int ocr_pipeline_add(ocr_pipeline_t *pl, ocr_pipeline_node_t *node);

/**
 * @brief 按登记顺序启动节点，任一失败则逆序请求停止已启动的节点
 * @return 0=已启动，-1=参数无效，-2=某个节点启动失败
 * @note 在所有节点启动成功后才允许外部生产者投递数据。
 */
int ocr_pipeline_start(ocr_pipeline_t *pl);

/**
 * @brief 逆序停止节点，可重复调用；尚未 join 成功时保留状态供管理线程重试
 * @return 0=已停止，-1=参数无效，-2=某个节点未能停止
 * @note 先停止外部生产者；本函数不清空节点队列，也不保证剩余帧被处理。
 */
int ocr_pipeline_stop(ocr_pipeline_t *pl);

/**
 * @brief 停止并逆序销毁全部登记节点，释放各节点的队列引用和同步对象
 * @return 0=成功，-1=参数无效，-2=停止失败且未进行销毁
 * @note 节点本体由调用者分配，本函数不 free；销毁完成前须保持缓冲池和回收上下文有效。
 */
int ocr_pipeline_destroy(ocr_pipeline_t *pl);

/** @brief 逐节点记录瞬时 processed/dropped 统计；读取原子计数，不保证各节点同一时刻快照。 */
void ocr_pipeline_dump_stats(const ocr_pipeline_t *pl);

#endif /* OCR_PIPELINE_PIPELINE_H */
