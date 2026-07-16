#ifndef OCR_PIPELINE_PIPELINE_H
#define OCR_PIPELINE_PIPELINE_H

/** @file pipeline.h @brief Pipeline topology lifecycle orchestration. */

#include "node.h"

#define OCR_PIPELINE_MAX_NODES 16

typedef struct {
    ocr_pipeline_node_t *nodes[OCR_PIPELINE_MAX_NODES];
    int                  node_count;
    int                  started_count;
    int                  started;
} ocr_pipeline_t;

int ocr_pipeline_init(ocr_pipeline_t *pl);
int ocr_pipeline_add(ocr_pipeline_t *pl, ocr_pipeline_node_t *node);

/** Start all nodes; a failure stops every node already started by this call. */
int ocr_pipeline_start(ocr_pipeline_t *pl);

/** Stop nodes in reverse order. Repeated calls are safe. */
int ocr_pipeline_stop(ocr_pipeline_t *pl);

/** Stop and destroy all registered nodes in reverse order. */
int ocr_pipeline_destroy(ocr_pipeline_t *pl);

void ocr_pipeline_dump_stats(const ocr_pipeline_t *pl);

#endif /* OCR_PIPELINE_PIPELINE_H */
