/** @file pipeline.c @brief Pipeline topology lifecycle orchestration. */
#include "pipeline.h"
#include "log.h"

#include <string.h>

int ocr_pipeline_init(ocr_pipeline_t *pl)
{
    if (!pl) {
        return -1;
    }
    memset(pl, 0, sizeof(*pl));
    return 0;
}

int ocr_pipeline_add(ocr_pipeline_t *pl, ocr_pipeline_node_t *node)
{
    if (!pl || !node ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        return -1;
    }
    if (pl->started || pl->started_count != 0) {
        return -2;
    }
    for (int i = 0; i < pl->node_count; ++i) {
        if (pl->nodes[i] == node) {
            return 0;
        }
    }
    if (pl->node_count >= OCR_PIPELINE_MAX_NODES) {
        LOG_E("pipeline node limit (%d) exceeded", OCR_PIPELINE_MAX_NODES);
        return -3;
    }
    pl->nodes[pl->node_count++] = node;
    return 0;
}

int ocr_pipeline_start(ocr_pipeline_t *pl)
{
    if (!pl) {
        return -1;
    }
    if (pl->started) {
        return 0;
    }

    pl->started_count = 0;
    for (int i = 0; i < pl->node_count; ++i) {
        if (!pl->nodes[i] || ocr_node_start(pl->nodes[i]) != 0) {
            if (pl->nodes[i]) {
                LOG_E("node %s failed to start", pl->nodes[i]->name);
            } else {
                LOG_E("pipeline contains a null node at index %d", i);
            }
            for (int j = pl->started_count - 1; j >= 0; --j) {
                (void)ocr_node_stop(pl->nodes[j]);
            }
            pl->started_count = 0;
            pl->started = 0;
            return -2;
        }
        ++pl->started_count;
    }

    pl->started = 1;
    return 0;
}

int ocr_pipeline_stop(ocr_pipeline_t *pl)
{
    int failed = 0;

    if (!pl) {
        return -1;
    }
    if (!pl->started && pl->started_count == 0) {
        return 0;
    }

    for (int i = pl->started_count - 1; i >= 0; --i) {
        if (pl->nodes[i] && ocr_node_stop(pl->nodes[i]) != 0) {
            failed = 1;
        }
    }
    if (failed) {
        /* Keep the range so an external caller can retry any self-stop join. */
        pl->started = 1;
        return -2;
    }

    pl->started_count = 0;
    pl->started = 0;
    return 0;
}

int ocr_pipeline_destroy(ocr_pipeline_t *pl)
{
    if (!pl) return -1;
    if (ocr_pipeline_stop(pl) != 0) return -2;
    for (int i = pl->node_count - 1; i >= 0; --i) {
        if (pl->nodes[i]) ocr_node_destroy(pl->nodes[i]);
        pl->nodes[i] = NULL;
    }
    pl->node_count = 0;
    pl->started_count = 0;
    pl->started = 0;
    return 0;
}

void ocr_pipeline_dump_stats(const ocr_pipeline_t *pl)
{
    if (!pl) {
        return;
    }

    LOG_I("pipeline statistics:");
    for (int i = 0; i < pl->node_count; ++i) {
        const ocr_pipeline_node_t *node = pl->nodes[i];
        if (!node) {
            continue;
        }
        LOG_I("  [%s] processed=%d dropped=%d",
              node->name,
              atomic_load_explicit(&node->processed, memory_order_relaxed),
              atomic_load_explicit(&node->dropped, memory_order_relaxed));
    }
}
