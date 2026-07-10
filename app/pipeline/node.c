/**
 * @file node.c
 * @brief 管线节点基类实现
 *
 * 注：节点实现内嵌于 pipeline.c 中合并编译，
 * 此处单独保留以支持未来拆分。当前为占位实现。
 */
#include "node.h"
#include "log.h"

#include <string.h>
#include <unistd.h>

int ocr_node_init(ocr_pipeline_node_t *node, const char *name,
                  ocr_node_process_fn process, void *ctx)
{
    if (!node || !name) return -1;
    memset(node, 0, sizeof(*node));
    strncpy(node->name, name, OCR_NODE_NAME_LEN - 1);
    node->process = process;
    node->user_ctx = ctx;
    node->in_head = node->in_tail = 0;
    ocr_mutex_init(&node->in_lock);
    ocr_cond_init(&node->in_cond);
    atomic_init(&node->in_count, 0);
    atomic_init(&node->running, 0);
    atomic_init(&node->processed, 0);
    atomic_init(&node->dropped, 0);
    return 0;
}

int ocr_node_link(ocr_pipeline_node_t *node, ocr_pipeline_node_t *downstream)
{
    if (!node || !downstream) return -1;
    if (node->downstream_count >= 4) {
        LOG_E("节点 %s 下游数超过上限", node->name);
        return -2;
    }
    node->downstream[node->downstream_count++] = downstream;
    return 0;
}

int ocr_node_push_input(ocr_pipeline_node_t *node, ocr_buffer_t *buf)
{
    if (!node || !buf) return -1;
    ocr_mutex_lock(&node->in_lock);
    if (atomic_load(&node->in_count) >= OCR_NODE_QUEUE_DEPTH) {
        /* 队列满，丢弃最旧帧以保证实时性 */
        ocr_buffer_t *old = node->in_queue[node->in_head];
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub(&node->in_count, 1);
        atomic_fetch_add(&node->dropped, 1);
        if (old) ocr_buffer_unref(old);
        LOG_W("节点 %s 输入队列满，丢弃旧帧", node->name);
    }
    node->in_queue[node->in_tail] = buf;
    node->in_tail = (node->in_tail + 1) % OCR_NODE_QUEUE_DEPTH;
    atomic_fetch_add(&node->in_count, 1);
    ocr_cond_signal(&node->in_cond);
    ocr_mutex_unlock(&node->in_lock);
    return 0;
}

void *ocr_node_thread_entry(void *arg)
{
    ocr_pipeline_node_t *node = (ocr_pipeline_node_t *)arg;
    LOG_I("节点 %s 线程启动", node->name);

    while (atomic_load(&node->running)) {
        ocr_buffer_t *buf = NULL;

        ocr_mutex_lock(&node->in_lock);
        while (atomic_load(&node->in_count) == 0 && atomic_load(&node->running)) {
            ocr_cond_wait_ms(&node->in_cond, &node->in_lock, 100);
        }
        if (!atomic_load(&node->running)) {
            ocr_mutex_unlock(&node->in_lock);
            break;
        }
        buf = node->in_queue[node->in_head];
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub(&node->in_count, 1);
        ocr_mutex_unlock(&node->in_lock);

        if (buf && node->process) {
            int ret = node->process(node, buf);
            if (ret == 0) {
                atomic_fetch_add(&node->processed, 1);
                /* 投递到下游 */
                for (int i = 0; i < node->downstream_count; i++) {
                    ocr_buffer_ref(buf);
                    ocr_node_push_input(node->downstream[i], buf);
                }
            } else {
                atomic_fetch_add(&node->dropped, 1);
            }
            ocr_buffer_unref(buf);
        }
    }

    LOG_I("节点 %s 线程退出", node->name);
    return NULL;
}

int ocr_node_start(ocr_pipeline_node_t *node)
{
    if (!node) return -1;
    atomic_store(&node->running, 1);
    if (ocr_thread_create(&node->thread, ocr_node_thread_entry, node) != 0) {
        LOG_E("节点 %s 线程创建失败", node->name);
        return -2;
    }
    return 0;
}

int ocr_node_stop(ocr_pipeline_node_t *node)
{
    if (!node) return -1;
    atomic_store(&node->running, 0);
    ocr_cond_broadcast(&node->in_cond);
    ocr_thread_join(node->thread, NULL);
    return 0;
}

void ocr_node_destroy(ocr_pipeline_node_t *node)
{
    if (!node) return;
    /* 清空队列中残留缓冲 */
    ocr_mutex_lock(&node->in_lock);
    while (atomic_load(&node->in_count) > 0) {
        ocr_buffer_t *buf = node->in_queue[node->in_head];
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub(&node->in_count, 1);
        if (buf) ocr_buffer_unref(buf);
    }
    ocr_mutex_unlock(&node->in_lock);
    ocr_cond_destroy(&node->in_cond);
    ocr_mutex_destroy(&node->in_lock);
}
