/**
 * @file test_node.c
 * 验证仅空闲接收的节点：已有待处理帧或正在处理时拒收新帧且不接管引用，
 * 处理完成并归还输入后恢复接收；不涉及摄像头、RGA 或显示硬件。
 */
/* Host unit test for the idle-only pipeline node policy. */
#include "node.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static atomic_int released;
static atomic_int entered;
static atomic_int gate_open;
static atomic_uint_fast64_t seen_mask;

/** @brief 最终引用归零时计数并释放替身元数据，用来确认每帧只回收一次。 */
static void release_frame(ocr_buffer_t *buffer, void *user_data)
{
    (void)user_data;
    atomic_fetch_add_explicit(&released, 1, memory_order_relaxed);
    free(buffer);
}

static ocr_buffer_t *make_frame(uint64_t id)
{
    ocr_buffer_t *buffer = malloc(sizeof(*buffer));
    assert(buffer);
    assert(ocr_buffer_init(buffer) == 0);
    buffer->frame_id = id;
    assert(ocr_buffer_set_release_callback(buffer, release_frame, NULL) == 0);
    return buffer;
}

/** @brief 记录帧号后停在闸门前，模拟一次耗时识别，直到测试放行。 */
static int slow_process(ocr_pipeline_node_t *node, ocr_buffer_t *buffer)
{
    struct timespec delay = {.tv_nsec = 1000000};
    (void)node;
    atomic_fetch_or_explicit(&seen_mask, UINT64_C(1) << buffer->frame_id,
                             memory_order_relaxed);
    atomic_fetch_add_explicit(&entered, 1, memory_order_release);
    while (!atomic_load_explicit(&gate_open, memory_order_acquire))
        nanosleep(&delay, NULL);
    return 0;
}

/** @brief 以有限时间等待线程结果，避免调度尚未完成时立即断言，也避免测试无限阻塞。 */
static int wait_for(atomic_int *value, int expected, int timeout_ms)
{
    struct timespec delay = {.tv_nsec = 1000000};
    for (int elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (atomic_load_explicit(value, memory_order_acquire) == expected)
            return 0;
        nanosleep(&delay, NULL);
    }
    return -1;
}

static void reset_counters(void)
{
    atomic_store(&released, 0);
    atomic_store(&entered, 0);
    atomic_store(&gate_open, 0);
    atomic_store(&seen_mask, 0);
}

/** @brief 未启动时第一帧留在队列里，第二帧必须被拒收，且引用仍归调用者。 */
static void test_rejects_while_pending(void)
{
    ocr_pipeline_node_t node;
    ocr_buffer_t *first = make_frame(0);
    ocr_buffer_t *second = make_frame(1);

    reset_counters();
    assert(ocr_node_init(&node, "idle", slow_process, NULL) == 0);
    assert(ocr_node_set_idle_only(&node, 1) == 0);
    assert(ocr_node_push_input(&node, first) == 0);
    assert(ocr_node_push_input(&node, second) == -2);
    assert(atomic_load(&node.skipped) == 1);
    assert(atomic_load(&node.dropped) == 0);
    assert(atomic_load(&second->refcount) == 1);
    assert(ocr_buffer_unref(second) == 0);
    ocr_node_destroy(&node);
    assert(atomic_load(&released) == 2);
}

/** @brief 处理中拒收全部新帧；处理完并归还输入后，下一帧照常接收。 */
static void test_rejects_while_busy(void)
{
    ocr_pipeline_node_t node;

    reset_counters();
    assert(ocr_node_init(&node, "idle", slow_process, NULL) == 0);
    assert(ocr_node_set_idle_only(&node, 1) == 0);
    assert(ocr_node_start(&node) == 0);
    assert(ocr_node_set_idle_only(&node, 0) == -2);

    assert(ocr_node_push_input(&node, make_frame(0)) == 0);
    assert(wait_for(&entered, 1, 2000) == 0);
    for (uint64_t id = 1; id <= 3; ++id) {
        ocr_buffer_t *frame = make_frame(id);
        assert(ocr_node_push_input(&node, frame) == -2);
        assert(ocr_buffer_unref(frame) == 0);
    }
    assert(atomic_load(&node.skipped) == 3);
    assert(atomic_load(&released) == 3);

    atomic_store_explicit(&gate_open, 1, memory_order_release);
    assert(wait_for(&released, 4, 2000) == 0);
    assert(wait_for(&node.busy, 0, 2000) == 0);
    assert(ocr_node_push_input(&node, make_frame(4)) == 0);
    assert(wait_for(&released, 5, 2000) == 0);

    assert(atomic_load(&node.processed) == 2);
    assert(atomic_load(&node.dropped) == 0);
    assert(atomic_load(&seen_mask) == ((UINT64_C(1) << 0) | (UINT64_C(1) << 4)));
    ocr_node_destroy(&node);
}

int main(void)
{
    test_rejects_while_pending();
    test_rejects_while_busy();
    puts("[PASS] idle-only node accepts a frame only when idle");
    return EXIT_SUCCESS;
}
