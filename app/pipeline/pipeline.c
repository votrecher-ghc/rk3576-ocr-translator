/**
 * @file pipeline.c
 * @brief 管线编排实现
 */
#include "pipeline.h"
#include "log.h"

#include <string.h>

int ocr_pipeline_init(ocr_pipeline_t *pl)
{
    if (!pl) return -1;
    memset(pl, 0, sizeof(*pl));
    return 0;
}

int ocr_pipeline_add(ocr_pipeline_t *pl, ocr_pipeline_node_t *node)
{
    if (!pl || !node) return -1;
    if (pl->node_count >= OCR_PIPELINE_MAX_NODES) {
        LOG_E("管线节点数超过上限 %d", OCR_PIPELINE_MAX_NODES);
        return -2;
    }
    pl->nodes[pl->node_count++] = node;
    return 0;
}

int ocr_pipeline_start(ocr_pipeline_t *pl)
{
    if (!pl) return -1;
    for (int i = 0; i < pl->node_count; i++) {
        if (ocr_node_start(pl->nodes[i]) != 0) {
            LOG_E("节点 %s 启动失败", pl->nodes[i]->name);
            return -2;
        }
    }
    pl->started = 1;
    return 0;
}

int ocr_pipeline_stop(ocr_pipeline_t *pl)
{
    if (!pl) return -1;
    /* 逆序停止 */
    for (int i = pl->node_count - 1; i >= 0; i--) {
        ocr_node_stop(pl->nodes[i]);
    }
    pl->started = 0;
    return 0;
}

void ocr_pipeline_dump_stats(const ocr_pipeline_t *pl)
{
    if (!pl) return;
    LOG_I("==== 管线统计 ====");
    for (int i = 0; i < pl->node_count; i++) {
        const ocr_pipeline_node_t *n = pl->nodes[i];
        LOG_I("  [%s] processed=%d dropped=%d",
              n->name, atomic_load(&n->processed), atomic_load(&n->dropped));
    }
}
