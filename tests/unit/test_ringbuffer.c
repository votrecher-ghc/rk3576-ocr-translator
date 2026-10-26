/**
 * @file test_ringbuffer.c
 * 验证满容量、环绕、非二次幂容量、非法参数及真实双线程 SPSC 顺序。
 * 并发测试只有一个生产者和一个消费者，与生产队列的使用约定一致。
 */
/*
 * Unit tests for the production lock-free SPSC ring buffer.
 */

#include "ringbuffer.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

/** @brief 确认声明的全部槽位都可用，额外一次 push 返回满且不覆盖旧元素。 */
static void test_full_capacity(void)
{
    ocr_ringbuffer_t rb;
    CHECK(ocr_ringbuffer_init(&rb, sizeof(uint32_t), 8) == 0);
    CHECK(ocr_ringbuffer_empty(&rb));

    for (uint32_t value = 0; value < 8; ++value) {
        CHECK(ocr_ringbuffer_push(&rb, &value) == 0);
        CHECK(ocr_ringbuffer_size(&rb) == (size_t)value + 1);
    }

    uint32_t extra = 99;
    CHECK(ocr_ringbuffer_size(&rb) == 8);
    CHECK(ocr_ringbuffer_push(&rb, &extra) == 1);

    for (uint32_t expected = 0; expected < 8; ++expected) {
        uint32_t actual = UINT32_MAX;
        CHECK(ocr_ringbuffer_pop(&rb, &actual) == 0);
        CHECK(actual == expected);
    }
    CHECK(ocr_ringbuffer_pop(&rb, &extra) == 1);
    CHECK(ocr_ringbuffer_empty(&rb));
    ocr_ringbuffer_destroy(&rb);
}

/** @brief 反复入队和出队跨越物理数组末端，验证元素顺序和槽位复用。 */
static void test_wraparound(void)
{
    ocr_ringbuffer_t rb;
    CHECK(ocr_ringbuffer_init(&rb, sizeof(uint32_t), 8) == 0);

    for (uint32_t value = 0; value < 8; ++value) {
        CHECK(ocr_ringbuffer_push(&rb, &value) == 0);
    }
    for (uint32_t expected = 0; expected < 3; ++expected) {
        uint32_t actual;
        CHECK(ocr_ringbuffer_pop(&rb, &actual) == 0);
        CHECK(actual == expected);
    }
    for (uint32_t value = 8; value < 11; ++value) {
        CHECK(ocr_ringbuffer_push(&rb, &value) == 0);
    }
    CHECK(ocr_ringbuffer_size(&rb) == 8);

    for (uint32_t expected = 3; expected < 11; ++expected) {
        uint32_t actual;
        CHECK(ocr_ringbuffer_pop(&rb, &actual) == 0);
        CHECK(actual == expected);
    }
    CHECK(ocr_ringbuffer_empty(&rb));
    ocr_ringbuffer_destroy(&rb);
}

/** @brief 覆盖取模索引路径，防止实现误把所有容量都当成二次幂。 */
static void test_non_power_of_two_capacity(void)
{
    ocr_ringbuffer_t rb;
    CHECK(ocr_ringbuffer_init(&rb, sizeof(int), 3) == 0);

    int values[] = {11, 12, 13, 14};
    CHECK(ocr_ringbuffer_push(&rb, &values[0]) == 0);
    CHECK(ocr_ringbuffer_push(&rb, &values[1]) == 0);
    CHECK(ocr_ringbuffer_push(&rb, &values[2]) == 0);
    CHECK(ocr_ringbuffer_push(&rb, &values[3]) == 1);

    int actual;
    CHECK(ocr_ringbuffer_pop(&rb, &actual) == 0 && actual == 11);
    CHECK(ocr_ringbuffer_push(&rb, &values[3]) == 0);
    CHECK(ocr_ringbuffer_pop(&rb, &actual) == 0 && actual == 12);
    CHECK(ocr_ringbuffer_pop(&rb, &actual) == 0 && actual == 13);
    CHECK(ocr_ringbuffer_pop(&rb, &actual) == 0 && actual == 14);
    CHECK(ocr_ringbuffer_pop(&rb, &actual) == 1);
    ocr_ringbuffer_destroy(&rb);
}

#define CONCURRENT_ITEMS    UINT32_C(100000)
#define CONCURRENT_CAPACITY ((size_t)256)

typedef struct {
    ocr_ringbuffer_t rb; /**< 内嵌受测 SPSC 队列，一个生产者写、一个消费者读，主测试负责销毁。 */
    atomic_size_t consumed; /**< 消费者已按顺序核验的元素数，原子发布给主测试线程。 */
} concurrent_fixture_t;

static void *producer_main(void *arg)
{
    concurrent_fixture_t *fixture = (concurrent_fixture_t *)arg;
    for (uint32_t value = 0; value < CONCURRENT_ITEMS; ++value) {
        int rc;
        while ((rc = ocr_ringbuffer_push(&fixture->rb, &value)) == 1) {
            sched_yield();
        }
        CHECK(rc == 0);
    }
    return NULL;
}

static void *consumer_main(void *arg)
{
    concurrent_fixture_t *fixture = (concurrent_fixture_t *)arg;
    for (uint32_t expected = 0; expected < CONCURRENT_ITEMS; ++expected) {
        uint32_t actual = UINT32_MAX;
        int rc;
        while ((rc = ocr_ringbuffer_pop(&fixture->rb, &actual)) == 1) {
            sched_yield();
        }
        CHECK(rc == 0);
        CHECK(actual == expected);
        atomic_fetch_add_explicit(&fixture->consumed, 1, memory_order_relaxed);
    }
    return NULL;
}

/** @brief 让生产者和消费者独立运行，检查大量元素跨线程传递仍保持顺序。 */
static void test_concurrent_spsc(void)
{
    concurrent_fixture_t fixture;
    CHECK(ocr_ringbuffer_init(&fixture.rb, sizeof(uint32_t),
                              CONCURRENT_CAPACITY) == 0);
    atomic_init(&fixture.consumed, 0);

    pthread_t producer;
    pthread_t consumer;
    CHECK(pthread_create(&producer, NULL, producer_main, &fixture) == 0);
    CHECK(pthread_create(&consumer, NULL, consumer_main, &fixture) == 0);
    CHECK(pthread_join(producer, NULL) == 0);
    CHECK(pthread_join(consumer, NULL) == 0);

    CHECK(atomic_load_explicit(&fixture.consumed, memory_order_relaxed) ==
          (size_t)CONCURRENT_ITEMS);
    CHECK(ocr_ringbuffer_empty(&fixture.rb));
    ocr_ringbuffer_destroy(&fixture.rb);
}

static void test_invalid_arguments(void)
{
    ocr_ringbuffer_t rb;
    int value = 1;
    CHECK(ocr_ringbuffer_init(NULL, sizeof(value), 8) < 0);
    CHECK(ocr_ringbuffer_init(&rb, 0, 8) < 0);
    CHECK(ocr_ringbuffer_init(&rb, sizeof(value), 0) < 0);

    CHECK(ocr_ringbuffer_init(&rb, sizeof(value), 1) == 0);
    CHECK(ocr_ringbuffer_push(NULL, &value) < 0);
    CHECK(ocr_ringbuffer_push(&rb, NULL) < 0);
    CHECK(ocr_ringbuffer_pop(NULL, &value) < 0);
    CHECK(ocr_ringbuffer_pop(&rb, NULL) < 0);
    ocr_ringbuffer_destroy(&rb);
    ocr_ringbuffer_destroy(NULL);
}

int main(void)
{
    test_invalid_arguments();
    test_full_capacity();
    test_wraparound();
    test_non_power_of_two_capacity();
    test_concurrent_spsc();
    puts("[PASS] production SPSC ring buffer tests");
    return EXIT_SUCCESS;
}
