#ifndef OCR_PIPELINE_NODE_H
#define OCR_PIPELINE_NODE_H

/**
 * @file node.h
 * @brief Pipeline worker node, bounded input queue, and fan-out handling.
 */

#include "buffer.h"
#include "thread.h"

#include <stdatomic.h>

#define OCR_NODE_NAME_LEN 32
#define OCR_NODE_QUEUE_DEPTH 8
#define OCR_NODE_MAX_DOWNSTREAM 4

struct ocr_pipeline_node;

/**
 * The input buffer is borrowed for the duration of the callback.
 * Return < 0 to drop it, 0 to let the node forward the same input, or > 0
 * when the callback handled output itself (normally through ocr_node_emit()).
 */
typedef int (*ocr_node_process_fn)(struct ocr_pipeline_node *node,
                                   ocr_buffer_t *buf);

typedef struct ocr_pipeline_node {
    char                name[OCR_NODE_NAME_LEN];
    ocr_node_process_fn process;
    void               *user_ctx;

    ocr_buffer_t       *in_queue[OCR_NODE_QUEUE_DEPTH];
    int                 in_head;
    int                 in_tail;
    ocr_mutex_t         in_lock;
    ocr_cond_t          in_cond;
    atomic_int          in_count;

    struct ocr_pipeline_node *downstream[OCR_NODE_MAX_DOWNSTREAM];
    int                 downstream_count;

    ocr_mutex_t         lifecycle_lock;
    ocr_thread_t        thread;
    atomic_int          initialized;
    atomic_int          thread_started;
    atomic_int          running;
    atomic_int          processed;
    atomic_int          dropped;
} ocr_pipeline_node_t;

int ocr_node_init(ocr_pipeline_node_t *node, const char *name,
                  ocr_node_process_fn process, void *ctx);

/** Link topology before starting the source node. Duplicate links are ignored. */
int ocr_node_link(ocr_pipeline_node_t *node,
                  ocr_pipeline_node_t *downstream);

/**
 * Queue one caller-owned reference. On success ownership transfers to the
 * queue; on failure the caller still owns the reference.
 */
int ocr_node_push_input(ocr_pipeline_node_t *node, ocr_buffer_t *buf);

/**
 * Fan out a callback-produced buffer. For valid arguments this function
 * consumes the caller's one reference, including when there are no consumers.
 * It returns the number of downstream queues that accepted a reference.
 * On a negative return the caller retains ownership.
 */
int ocr_node_emit(ocr_pipeline_node_t *node, ocr_buffer_t *buf);

void *ocr_node_thread_entry(void *arg);
int ocr_node_start(ocr_pipeline_node_t *node);
int ocr_node_stop(ocr_pipeline_node_t *node);

/** Stop, drain, and release node synchronization objects; safe to repeat. */
void ocr_node_destroy(ocr_pipeline_node_t *node);

#endif /* OCR_PIPELINE_NODE_H */
