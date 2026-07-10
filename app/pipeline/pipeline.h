#ifndef OCR_PIPELINE_PIPELINE_H
#define OCR_PIPELINE_PIPELINE_H
/**
 * @file pipeline.h
 * @brief 管线编排：节点连接/启动/停止
 *
 * 管理所有节点的生命周期，提供统一启动/停止接口。
 */

#include "node.h"

#define OCR_PIPELINE_MAX_NODES 16

/** 管线上下文 */
typedef struct {
    ocr_pipeline_node_t *nodes[OCR_PIPELINE_MAX_NODES];
    int                  node_count;
    int                  started;
} ocr_pipeline_t;

/**
 * @brief 初始化管线
 */
int ocr_pipeline_init(ocr_pipeline_t *pl);

/**
 * @brief 注册节点到管线
 */
int ocr_pipeline_add(ocr_pipeline_t *pl, ocr_pipeline_node_t *node);

/**
 * @brief 启动所有节点线程（按注册顺序）
 */
int ocr_pipeline_start(ocr_pipeline_t *pl);

/**
 * @brief 停止所有节点线程（逆序停止）
 */
int ocr_pipeline_stop(ocr_pipeline_t *pl);

/**
 * @brief 打印管线各节点统计信息
 */
void ocr_pipeline_dump_stats(const ocr_pipeline_t *pl);

#endif /* OCR_PIPELINE_PIPELINE_H */
