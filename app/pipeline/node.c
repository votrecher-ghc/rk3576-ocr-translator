/**
 * @file node.c
 * @brief Pipeline worker node implementation.
 */
#include "node.h"
#include "log.h"

#include <string.h>

static int node_forward_borrowed(ocr_pipeline_node_t *node,
                                 ocr_buffer_t *buf)
{
    int accepted = 0;

    for (int i = 0; i < node->downstream_count; ++i) {
        if (ocr_buffer_ref(buf) < 0) {
            break;
        }
        if (ocr_node_push_input(node->downstream[i], buf) == 0) {
            ++accepted;
        } else {
            (void)ocr_buffer_unref(buf);
        }
    }
    return accepted;
}

static int node_stop_locked(ocr_pipeline_node_t *node)
{
    int ret;

    atomic_store_explicit(&node->running, 0, memory_order_release);
    if (!atomic_load_explicit(&node->thread_started, memory_order_acquire)) {
        return 0;
    }

    ocr_mutex_lock(&node->in_lock);
    ocr_cond_broadcast(&node->in_cond);
    ocr_mutex_unlock(&node->in_lock);

    /* A callback may request stop, but it cannot join its own worker. */
    if (pthread_equal(node->thread, pthread_self())) {
        return -2;
    }

    ret = ocr_thread_join(node->thread, NULL);
    if (ret != 0) {
        return -2;
    }
    atomic_store_explicit(&node->thread_started, 0, memory_order_release);
    memset(&node->thread, 0, sizeof(node->thread));
    return 0;
}

int ocr_node_init(ocr_pipeline_node_t *node, const char *name,
                  ocr_node_process_fn process, void *ctx)
{
    if (!node || !name || name[0] == '\0') {
        return -1;
    }

    memset(node, 0, sizeof(*node));
    if (ocr_mutex_init(&node->lifecycle_lock) != 0) {
        return -2;
    }
    if (ocr_mutex_init(&node->in_lock) != 0) {
        ocr_mutex_destroy(&node->lifecycle_lock);
        return -2;
    }
    if (ocr_cond_init(&node->in_cond) != 0) {
        ocr_mutex_destroy(&node->in_lock);
        ocr_mutex_destroy(&node->lifecycle_lock);
        return -2;
    }

    strncpy(node->name, name, OCR_NODE_NAME_LEN - 1);
    node->name[OCR_NODE_NAME_LEN - 1] = '\0';
    node->process = process;
    node->user_ctx = ctx;
    atomic_init(&node->in_count, 0);
    atomic_init(&node->thread_started, 0);
    atomic_init(&node->running, 0);
    atomic_init(&node->processed, 0);
    atomic_init(&node->dropped, 0);
    atomic_init(&node->initialized, 1);
    return 0;
}

int ocr_node_link(ocr_pipeline_node_t *node,
                  ocr_pipeline_node_t *downstream)
{
    int ret = 0;

    if (!node || !downstream || node == downstream ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire) ||
        !atomic_load_explicit(&downstream->initialized,
                              memory_order_acquire)) {
        return -1;
    }

    ocr_mutex_lock(&node->lifecycle_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire) ||
        atomic_load_explicit(&node->thread_started, memory_order_acquire)) {
        ret = -2;
        goto out;
    }
    for (int i = 0; i < node->downstream_count; ++i) {
        if (node->downstream[i] == downstream) {
            goto out;
        }
    }
    if (node->downstream_count >= OCR_NODE_MAX_DOWNSTREAM) {
        ret = -3;
        goto out;
    }
    node->downstream[node->downstream_count++] = downstream;

out:
    ocr_mutex_unlock(&node->lifecycle_lock);
    return ret;
}

int ocr_node_push_input(ocr_pipeline_node_t *node, ocr_buffer_t *buf)
{
    ocr_buffer_t *evicted = NULL;

    if (!node || !buf ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire) ||
        atomic_load_explicit(&buf->refcount, memory_order_acquire) <= 0) {
        return -1;
    }

    ocr_mutex_lock(&node->in_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        ocr_mutex_unlock(&node->in_lock);
        return -1;
    }

    if (atomic_load_explicit(&node->in_count, memory_order_relaxed) >=
        OCR_NODE_QUEUE_DEPTH) {
        evicted = node->in_queue[node->in_head];
        node->in_queue[node->in_head] = NULL;
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub_explicit(&node->in_count, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&node->dropped, 1, memory_order_relaxed);
    }

    node->in_queue[node->in_tail] = buf;
    node->in_tail = (node->in_tail + 1) % OCR_NODE_QUEUE_DEPTH;
    atomic_fetch_add_explicit(&node->in_count, 1, memory_order_release);
    ocr_cond_signal(&node->in_cond);
    ocr_mutex_unlock(&node->in_lock);

    if (evicted) {
        (void)ocr_buffer_unref(evicted);
        LOG_W("node %s input queue full; oldest frame dropped", node->name);
    }
    return 0;
}

int ocr_node_emit(ocr_pipeline_node_t *node, ocr_buffer_t *buf)
{
    int accepted;

    if (!node || !buf ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire) ||
        atomic_load_explicit(&buf->refcount, memory_order_acquire) <= 0) {
        return -1;
    }

    accepted = node_forward_borrowed(node, buf);
    (void)ocr_buffer_unref(buf);
    return accepted;
}

void *ocr_node_thread_entry(void *arg)
{
    ocr_pipeline_node_t *node = (ocr_pipeline_node_t *)arg;

    if (!node) {
        return NULL;
    }
    LOG_I("node %s worker started", node->name);

    for (;;) {
        ocr_buffer_t *buf;
        int process_ret;

        ocr_mutex_lock(&node->in_lock);
        while (atomic_load_explicit(&node->in_count, memory_order_acquire) == 0 &&
               atomic_load_explicit(&node->running, memory_order_acquire)) {
            (void)ocr_cond_wait(&node->in_cond, &node->in_lock);
        }
        if (!atomic_load_explicit(&node->running, memory_order_acquire)) {
            ocr_mutex_unlock(&node->in_lock);
            break;
        }

        buf = node->in_queue[node->in_head];
        node->in_queue[node->in_head] = NULL;
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub_explicit(&node->in_count, 1, memory_order_release);
        ocr_mutex_unlock(&node->in_lock);

        if (!buf) {
            atomic_fetch_add_explicit(&node->dropped, 1,
                                      memory_order_relaxed);
            continue;
        }

        process_ret = node->process ? node->process(node, buf) : 0;
        if (process_ret < 0) {
            atomic_fetch_add_explicit(&node->dropped, 1,
                                      memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&node->processed, 1,
                                      memory_order_relaxed);
            if (process_ret == 0) {
                int accepted = node_forward_borrowed(node, buf);
                if (node->downstream_count > 0 && accepted == 0) {
                    atomic_fetch_add_explicit(&node->dropped, 1,
                                              memory_order_relaxed);
                }
            }
        }
        (void)ocr_buffer_unref(buf);
    }

    LOG_I("node %s worker stopped", node->name);
    return NULL;
}

int ocr_node_start(ocr_pipeline_node_t *node)
{
    if (!node ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        return -1;
    }

    ocr_mutex_lock(&node->lifecycle_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        return -1;
    }
    if (atomic_load_explicit(&node->thread_started, memory_order_acquire)) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        return 0;
    }

    atomic_store_explicit(&node->running, 1, memory_order_release);
    if (ocr_thread_create(&node->thread, ocr_node_thread_entry, node) != 0) {
        atomic_store_explicit(&node->running, 0, memory_order_release);
        memset(&node->thread, 0, sizeof(node->thread));
        ocr_mutex_unlock(&node->lifecycle_lock);
        LOG_E("node %s worker creation failed", node->name);
        return -2;
    }
    atomic_store_explicit(&node->thread_started, 1, memory_order_release);
    ocr_mutex_unlock(&node->lifecycle_lock);
    return 0;
}

int ocr_node_stop(ocr_pipeline_node_t *node)
{
    int ret;

    if (!node) {
        return -1;
    }
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        return 0;
    }

    ocr_mutex_lock(&node->lifecycle_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        return 0;
    }
    ret = node_stop_locked(node);
    ocr_mutex_unlock(&node->lifecycle_lock);
    return ret;
}

void ocr_node_destroy(ocr_pipeline_node_t *node)
{
    ocr_buffer_t *queued[OCR_NODE_QUEUE_DEPTH];
    int queued_count = 0;

    if (!node ||
        !atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        return;
    }

    ocr_mutex_lock(&node->lifecycle_lock);
    if (!atomic_load_explicit(&node->initialized, memory_order_acquire)) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        return;
    }
    if (node_stop_locked(node) != 0) {
        ocr_mutex_unlock(&node->lifecycle_lock);
        LOG_E("node %s cannot be destroyed from its worker", node->name);
        return;
    }

    atomic_store_explicit(&node->initialized, 0, memory_order_release);
    ocr_mutex_lock(&node->in_lock);
    while (atomic_load_explicit(&node->in_count, memory_order_relaxed) > 0) {
        ocr_buffer_t *buf = node->in_queue[node->in_head];
        node->in_queue[node->in_head] = NULL;
        node->in_head = (node->in_head + 1) % OCR_NODE_QUEUE_DEPTH;
        atomic_fetch_sub_explicit(&node->in_count, 1, memory_order_relaxed);
        if (buf && queued_count < OCR_NODE_QUEUE_DEPTH) {
            queued[queued_count++] = buf;
        }
    }
    node->in_head = 0;
    node->in_tail = 0;
    ocr_mutex_unlock(&node->in_lock);

    for (int i = 0; i < queued_count; ++i) {
        (void)ocr_buffer_unref(queued[i]);
    }

    ocr_cond_destroy(&node->in_cond);
    ocr_mutex_destroy(&node->in_lock);
    ocr_mutex_unlock(&node->lifecycle_lock);
    ocr_mutex_destroy(&node->lifecycle_lock);
}
