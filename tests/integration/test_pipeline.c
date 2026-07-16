/* Host integration test for the production threaded pipeline. */
#include "pipeline.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { FRAME_COUNT = 6 };

typedef struct {
    atomic_int calls;
    atomic_uint_fast64_t seen_mask;
} stage_ctx_t;

static atomic_int released;

static void release_frame(ocr_buffer_t *buffer, void *user_data)
{
    (void)user_data;
    atomic_fetch_add_explicit(&released, 1, memory_order_relaxed);
    free(buffer);
}

static int process_frame(ocr_pipeline_node_t *node, ocr_buffer_t *buffer)
{
    stage_ctx_t *stage = node->user_ctx;
    assert(buffer->frame_id < FRAME_COUNT);
    atomic_fetch_or_explicit(&stage->seen_mask, UINT64_C(1) << buffer->frame_id,
                             memory_order_relaxed);
    atomic_fetch_add_explicit(&stage->calls, 1, memory_order_relaxed);
    return 0;
}

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

int main(void)
{
    ocr_pipeline_t pipeline;
    ocr_pipeline_node_t capture, process, display;
    stage_ctx_t capture_ctx = {0}, process_ctx = {0}, display_ctx = {0};
    atomic_init(&capture_ctx.calls, 0);
    atomic_init(&capture_ctx.seen_mask, 0);
    atomic_init(&process_ctx.calls, 0);
    atomic_init(&process_ctx.seen_mask, 0);
    atomic_init(&display_ctx.calls, 0);
    atomic_init(&display_ctx.seen_mask, 0);
    atomic_init(&released, 0);

    assert(ocr_pipeline_init(&pipeline) == 0);
    assert(ocr_node_init(&capture, "capture", process_frame, &capture_ctx) == 0);
    assert(ocr_node_init(&process, "process", process_frame, &process_ctx) == 0);
    assert(ocr_node_init(&display, "display", process_frame, &display_ctx) == 0);
    assert(ocr_node_link(&capture, &process) == 0);
    assert(ocr_node_link(&process, &display) == 0);
    assert(ocr_pipeline_add(&pipeline, &capture) == 0);
    assert(ocr_pipeline_add(&pipeline, &process) == 0);
    assert(ocr_pipeline_add(&pipeline, &display) == 0);
    assert(ocr_pipeline_start(&pipeline) == 0);

    for (uint64_t id = 0; id < FRAME_COUNT; ++id) {
        ocr_buffer_t *buffer = malloc(sizeof(*buffer));
        assert(buffer);
        assert(ocr_buffer_init(buffer) == 0);
        buffer->frame_id = id;
        assert(ocr_buffer_set_release_callback(buffer, release_frame, NULL) == 0);
        assert(ocr_node_push_input(&capture, buffer) == 0);
    }

    assert(wait_for(&display_ctx.calls, FRAME_COUNT, 2000) == 0);
    assert(wait_for(&released, FRAME_COUNT, 2000) == 0);
    assert(ocr_pipeline_destroy(&pipeline) == 0);

    const uint64_t expected_mask = (UINT64_C(1) << FRAME_COUNT) - 1U;
    assert(atomic_load(&capture_ctx.calls) == FRAME_COUNT);
    assert(atomic_load(&process_ctx.calls) == FRAME_COUNT);
    assert(atomic_load(&display_ctx.calls) == FRAME_COUNT);
    assert(atomic_load(&capture_ctx.seen_mask) == expected_mask);
    assert(atomic_load(&process_ctx.seen_mask) == expected_mask);
    assert(atomic_load(&display_ctx.seen_mask) == expected_mask);
    puts("[PASS] production threaded pipeline integration test");
    return EXIT_SUCCESS;
}
