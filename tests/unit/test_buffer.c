/**
 * Production tests for app/pipeline/buffer.c and buffer_pool.c.
 * These tests use /dev/zero as a hardware-independent, mmap-capable fd.
 */
#include "buffer.h"
#include "buffer_pool.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_POOL_COUNT 4
#define TEST_BUFFER_SIZE 8192U
#define REF_THREADS 4
#define REF_ITERATIONS 10000

typedef struct {
    atomic_int calls;
    ocr_buffer_t *last_buffer;
} release_probe_t;

typedef struct {
    ocr_buffer_t *buffer;
} ref_worker_ctx_t;

typedef struct {
    ocr_buffer_pool_t *pool;
    atomic_int calls;
    int state_seen;
} recycle_probe_t;

static int open_test_fd(void)
{
    int fd = open("/dev/zero", O_RDWR);
    assert(fd >= 0);
    return fd;
}

static void release_probe_callback(ocr_buffer_t *buf, void *user_data)
{
    release_probe_t *probe = (release_probe_t *)user_data;
    probe->last_buffer = buf;
    atomic_fetch_add_explicit(&probe->calls, 1, memory_order_relaxed);
}

static void *ref_worker(void *arg)
{
    ref_worker_ctx_t *ctx = (ref_worker_ctx_t *)arg;

    for (int i = 0; i < REF_ITERATIONS; ++i) {
        assert(ocr_buffer_ref(ctx->buffer) >= 2);
        assert(ocr_buffer_unref(ctx->buffer) >= 1);
    }
    return NULL;
}

static void test_reference_lifecycle(void)
{
    ocr_buffer_t buffer;
    release_probe_t probe;
    pthread_t threads[REF_THREADS];
    ref_worker_ctx_t worker_ctx;

    memset(&probe, 0, sizeof(probe));
    atomic_init(&probe.calls, 0);
    assert(ocr_buffer_init(&buffer) == 0);
    assert(atomic_load(&buffer.refcount) == 1);
    assert(ocr_buffer_set_release_callback(&buffer,
                                           release_probe_callback,
                                           &probe) == 0);

    worker_ctx.buffer = &buffer;
    for (int i = 0; i < REF_THREADS; ++i) {
        assert(pthread_create(&threads[i], NULL, ref_worker, &worker_ctx) == 0);
    }
    for (int i = 0; i < REF_THREADS; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
    }

    assert(atomic_load(&buffer.refcount) == 1);
    assert(ocr_buffer_ref(&buffer) == 2);
    assert(ocr_buffer_unref(&buffer) == 1);
    assert(ocr_buffer_unref(&buffer) == 0);
    assert(atomic_load(&probe.calls) == 1);
    assert(probe.last_buffer == &buffer);

    /* A duplicate release cannot underflow or run the callback twice. */
    assert(ocr_buffer_unref(&buffer) < 0);
    assert(ocr_buffer_ref(&buffer) < 0);
    assert(atomic_load(&buffer.refcount) == 0);
    assert(atomic_load(&probe.calls) == 1);
    puts("[PASS] reference lifecycle");
}

static void test_mmap_lifecycle(void)
{
    ocr_buffer_t buffer;
    void *first_mapping;

    assert(ocr_buffer_init(&buffer) == 0);
    buffer.fd = open_test_fd();
    buffer.size = TEST_BUFFER_SIZE;

    assert(ocr_buffer_mmap(&buffer) == 0);
    assert(buffer.mmap_addr != NULL);
    first_mapping = buffer.mmap_addr;
    memset(buffer.mmap_addr, 0x5a, 64);

    assert(ocr_buffer_mmap(&buffer) == 0);
    assert(buffer.mmap_addr == first_mapping);
    ocr_buffer_munmap(&buffer);
    assert(buffer.mmap_addr == NULL);
    ocr_buffer_munmap(&buffer);
    assert(close(buffer.fd) == 0);
    buffer.fd = -1;
    puts("[PASS] mmap lifecycle");
}

static void test_image_layout_validation(void)
{
    ocr_buffer_t buffer;
    uint32_t strides[4] = {64, 64, 0, 0};
    uint32_t offsets[4] = {0, 4096, 0, 0};

    assert(ocr_buffer_init(&buffer) == 0);
    buffer.size = 6144;
    buffer.width = 64;
    buffer.height = 64;
    buffer.format = OCR_FMT_NV12;
    assert(ocr_buffer_set_layout(&buffer, 2, strides, offsets) == 0);
    assert(buffer.plane_count == 2);

    strides[0] = 63;
    assert(ocr_buffer_set_layout(&buffer, 2, strides, offsets) < 0);
    strides[0] = 64;
    assert(ocr_buffer_set_layout(&buffer, 1, strides, offsets) < 0);
    offsets[1] = 4000;
    assert(ocr_buffer_set_layout(&buffer, 2, strides, offsets) < 0);
    offsets[1] = 4096;
    buffer.size = 6143;
    assert(ocr_buffer_set_layout(&buffer, 2, strides, offsets) < 0);

    buffer.size = TEST_BUFFER_SIZE;
    buffer.width = 63;
    buffer.height = 32;
    buffer.format = OCR_FMT_YUYV;
    strides[0] = 126;
    offsets[0] = 0;
    assert(ocr_buffer_set_layout(&buffer, 1, strides, offsets) < 0);
    puts("[PASS] image layout validation");
}

static int recycle_probe_callback(ocr_buffer_t *buffer, void *user_data)
{
    recycle_probe_t *probe = (recycle_probe_t *)user_data;
    probe->state_seen = probe->pool->slot_state[buffer->index];
    atomic_fetch_add_explicit(&probe->calls, 1, memory_order_relaxed);
    return 0;
}

static void test_pool_recycle_notification_order(void)
{
    ocr_buffer_pool_t pool;
    recycle_probe_t probe;
    int fd;

    memset(&probe, 0, sizeof(probe));
    probe.pool = &pool;
    atomic_init(&probe.calls, 0);
    assert(ocr_pool_init(&pool, 1) == 0);
    assert(ocr_pool_set_recycle_callback(&pool, recycle_probe_callback,
                                         &probe) == 0);
    fd = open_test_fd();
    assert(ocr_pool_register(&pool, 0, fd, TEST_BUFFER_SIZE,
                             64, 64, OCR_FMT_NV12) == 0);
    assert(ocr_pool_set_recycle_callback(&pool, NULL, NULL) < 0);
    assert(ocr_pool_is_idle(&pool) == 1);

    ocr_buffer_t *buffer = ocr_pool_acquire_index(&pool, 0);
    assert(buffer != NULL);
    assert(ocr_pool_is_idle(&pool) == 0);
    assert(ocr_pool_release(&pool, buffer) == 0);
    assert(atomic_load(&probe.calls) == 1);
    assert(probe.state_seen == OCR_POOL_SLOT_IN_USE);
    assert(ocr_pool_is_idle(&pool) == 1);
    assert(ocr_pool_destroy(&pool) == 0);
    puts("[PASS] pool recycle notification order");
}

static void register_pool_buffers(ocr_buffer_pool_t *pool, int *fds, int count)
{
    for (int i = 0; i < count; ++i) {
        fds[i] = open_test_fd();
        assert(ocr_pool_register(pool, i, fds[i], TEST_BUFFER_SIZE,
                                 64, 64, OCR_FMT_NV12) == 0);
    }
}

static void test_pool_reference_recycling(void)
{
    ocr_buffer_pool_t pool;
    ocr_buffer_t foreign;
    ocr_buffer_t *buffers[TEST_POOL_COUNT];
    int fds[TEST_POOL_COUNT];

    assert(ocr_pool_init(&pool, TEST_POOL_COUNT) == 0);
    assert(pool.free_count == 0);
    assert(pool.registered_count == 0);
    assert(ocr_pool_acquire(&pool) == NULL);
    register_pool_buffers(&pool, fds, TEST_POOL_COUNT);
    assert(pool.free_count == TEST_POOL_COUNT);

    ocr_buffer_t *indexed = ocr_pool_acquire_index(&pool, 2);
    assert(indexed == &pool.buffers[2]);
    assert(ocr_pool_acquire_index(&pool, 2) == NULL);
    assert(ocr_pool_release(&pool, indexed) == 0);
    assert(pool.free_count == TEST_POOL_COUNT);

    for (int i = 0; i < TEST_POOL_COUNT; ++i) {
        buffers[i] = ocr_pool_acquire(&pool);
        assert(buffers[i] != NULL);
        assert(atomic_load(&buffers[i]->refcount) == 1);
        for (int j = 0; j < i; ++j) {
            assert(buffers[i] != buffers[j]);
        }
    }
    assert(pool.free_count == 0);
    assert(ocr_pool_acquire(&pool) == NULL);

    /* Busy destroy is non-destructive and can be retried later. */
    assert(ocr_pool_destroy(&pool) == -2);
    assert(fcntl(fds[0], F_GETFD) >= 0);

    assert(ocr_buffer_ref(buffers[0]) == 2);
    assert(ocr_pool_release(&pool, buffers[0]) == 0);
    assert(atomic_load(&buffers[0]->refcount) == 1);
    assert(pool.free_count == 0);
    assert(ocr_pool_release(&pool, buffers[0]) == 0);
    assert(atomic_load(&buffers[0]->refcount) == 0);
    assert(pool.free_count == 1);

    /* Duplicate release neither underflows nor duplicates the free-list slot. */
    assert(ocr_pool_release(&pool, buffers[0]) < 0);
    assert(pool.free_count == 1);
    ocr_buffer_t *reused = ocr_pool_acquire(&pool);
    assert(reused == buffers[0]);
    assert(atomic_load(&reused->refcount) == 1);

    assert(ocr_buffer_init(&foreign) == 0);
    assert(ocr_pool_release(&pool, &foreign) < 0);
    assert(atomic_load(&foreign.refcount) == 1);
    assert(ocr_buffer_unref(&foreign) == 0);

    assert(ocr_pool_release(&pool, reused) == 0);
    for (int i = 1; i < TEST_POOL_COUNT; ++i) {
        assert(ocr_pool_release(&pool, buffers[i]) == 0);
    }
    assert(pool.free_count == TEST_POOL_COUNT);

    assert(ocr_pool_destroy(&pool) == 0);
    errno = 0;
    assert(fcntl(fds[0], F_GETFD) == -1);
    assert(errno == EBADF);
    assert(ocr_pool_destroy(&pool) == 0);
    puts("[PASS] pool reference recycling");
}

static void test_pool_parameter_validation(void)
{
    ocr_buffer_pool_t pool;
    int fd;

    assert(ocr_pool_init(NULL, 1) < 0);
    assert(ocr_pool_init(&pool, 0) < 0);
    assert(ocr_pool_init(&pool, OCR_POOL_MAX_BUFS + 1) < 0);
    assert(ocr_pool_init(&pool, 1) == 0);
    fd = open_test_fd();
    assert(ocr_pool_register(&pool, -1, fd, TEST_BUFFER_SIZE,
                             64, 64, OCR_FMT_NV12) < 0);
    assert(ocr_pool_register(&pool, 0, -1, TEST_BUFFER_SIZE,
                             64, 64, OCR_FMT_NV12) < 0);
    assert(ocr_pool_register(&pool, 0, fd, 0,
                             64, 64, OCR_FMT_NV12) < 0);
    assert(ocr_pool_register(&pool, 0, fd, TEST_BUFFER_SIZE,
                             64, 64, OCR_FMT_UNKNOWN) < 0);
    assert(ocr_pool_register(&pool, 0, fd, TEST_BUFFER_SIZE,
                             64, 64, OCR_FMT_NV12) == 0);
    assert(ocr_pool_register(&pool, 0, fd, TEST_BUFFER_SIZE,
                             64, 64, OCR_FMT_NV12) < 0);
    assert(ocr_pool_destroy(&pool) == 0);
    puts("[PASS] pool parameter validation");
}

int main(void)
{
    puts("=== production buffer tests ===");
    test_reference_lifecycle();
    test_mmap_lifecycle();
    test_image_layout_validation();
    test_pool_recycle_notification_order();
    test_pool_reference_recycling();
    test_pool_parameter_validation();
    puts("=== all production buffer tests passed ===");
    return 0;
}
