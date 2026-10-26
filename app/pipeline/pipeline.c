/** @file pipeline.c @brief Pipeline topology lifecycle orchestration.
 *
 * 管线对象登记外部提供的节点并统一启动、停止和销毁；节点间数据传递由 node 模块负责。
 * 启动失败会回滚已启动的节点；停止/销毁按登记的逆序执行，调用方需先停止上游继续投递。
 */
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

/** @brief 在启动前登记节点地址，不复制节点；同一节点重复登记不重复添加。 */
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

/** @brief 按登记顺序启动节点，任一失败则逆序停止本次已启动节点。 */
int ocr_pipeline_start(ocr_pipeline_t *pl)
{
    if (!pl) {
        return -1;
    }
    if (pl->started) {
        return 0;
    }

    /* 登记顺序由 main 决定，不在这里做拓扑排序。主流程等所有节点启动后才
     * 打开摄像头，因此依次创建 worker 期间还不会收到真实采集帧。 */
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

/** @brief 逆序停止节点；若线程尚未 join 成功则保留状态以便调用方重试。 */
int ocr_pipeline_stop(ocr_pipeline_t *pl)
{
    int failed = 0;

    if (!pl) {
        return -1;
    }
    if (!pl->started && pl->started_count == 0) {
        return 0;
    }

    /* 停止前由装配层先停止摄像头等外部生产者。这里只 join 各 worker，
     * 未处理队列项仍持引用，稍后的 node_destroy 才逐个释放，不承诺停机时处理完所有帧。 */
    for (int i = pl->started_count - 1; i >= 0; --i) {
        if (pl->nodes[i] && ocr_node_stop(pl->nodes[i]) != 0) {
            failed = 1;
        }
    }
    if (failed) {
        /* 某节点可能从自己的回调请求停止，无法自行 join；保留启动范围，
         * 让外部管理线程重试，不把仍有效的线程句柄当成已经回收。 */
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
