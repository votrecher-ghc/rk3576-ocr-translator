/*
 * test_ringbuffer.c - 环形缓冲测试
 *
 * 功能: 测试管线各阶段间的无锁环形缓冲区
 * 场景: capture -> ocr -> render 数据传递
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* --------------------------------------------------------------------
 * 环形缓冲区数据结构
 * -------------------------------------------------------------------- */

#define RING_SIZE 8  /* 环形缓冲区槽位数 (必须为 2 的幂) */
#define RING_MASK (RING_SIZE - 1)

/* 环形缓冲区槽位 */
typedef struct {
    void    *data;     /* 数据指针 */
    size_t   size;     /* 数据大小 */
    int      valid;    /* 槽位是否有效 */
} ring_slot_t;

/* 环形缓冲区 */
typedef struct {
    ring_slot_t slots[RING_SIZE];
    volatile unsigned int head;  /* 写入位置 */
    volatile unsigned int tail;  /* 读取位置 */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} ring_buffer_t;

/* --------------------------------------------------------------------
 * 环形缓冲区操作函数 (待测实现)
 * -------------------------------------------------------------------- */

static void ring_init(ring_buffer_t *rb)
{
    memset(rb->slots, 0, sizeof(rb->slots));
    rb->head = 0;
    rb->tail = 0;
    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);
}

static void ring_destroy(ring_buffer_t *rb)
{
    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
}

/* 入队 (生产者) */
static int ring_push(ring_buffer_t *rb, void *data, size_t size)
{
    pthread_mutex_lock(&rb->lock);
    unsigned int next = (rb->head + 1) & RING_MASK;
    while (next == rb->tail) {
        /* 缓冲区满, 等待 */
        pthread_cond_wait(&rb->not_full, &rb->lock);
    }
    rb->slots[rb->head].data  = data;
    rb->slots[rb->head].size  = size;
    rb->slots[rb->head].valid = 1;
    rb->head = next;
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);
    return 0;
}

/* 出队 (消费者) */
static int ring_pop(ring_buffer_t *rb, void **data, size_t *size)
{
    pthread_mutex_lock(&rb->lock);
    while (rb->head == rb->tail) {
        /* 缓冲区空, 等待 */
        pthread_cond_wait(&rb->not_empty, &rb->lock);
    }
    *data = rb->slots[rb->tail].data;
    *size = rb->slots[rb->tail].size;
    rb->slots[rb->tail].valid = 0;
    rb->tail = (rb->tail + 1) & RING_MASK;
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->lock);
    return 0;
}

/* 获取当前元素个数 */
static int ring_count(ring_buffer_t *rb)
{
    pthread_mutex_lock(&rb->lock);
    int count = (rb->head - rb->tail) & RING_MASK;
    pthread_mutex_unlock(&rb->lock);
    return count;
}

/* --------------------------------------------------------------------
 * 测试用例
 * -------------------------------------------------------------------- */

/* 测试1: 基本入队出队 */
static void test_basic_push_pop(void)
{
    ring_buffer_t rb;
    ring_init(&rb);

    int val = 42;
    ring_push(&rb, &val, sizeof(val));
    assert(ring_count(&rb) == 1);

    void *data;
    size_t size;
    ring_pop(&rb, &data, &size);
    assert(*(int *)data == 42);
    assert(size == sizeof(val));
    assert(ring_count(&rb) == 0);

    ring_destroy(&rb);
    printf("[PASS] test_basic_push_pop\n");
}

/* 测试2: FIFO 顺序保证 */
static void test_fifo_order(void)
{
    ring_buffer_t rb;
    ring_init(&rb);

    int values[RING_SIZE - 1];
    for (int i = 0; i < RING_SIZE - 1; i++) {
        values[i] = i * 10;
        ring_push(&rb, &values[i], sizeof(int));
    }

    void *data;
    size_t size;
    for (int i = 0; i < RING_SIZE - 1; i++) {
        ring_pop(&rb, &data, &size);
        assert(*(int *)data == i * 10);
    }

    ring_destroy(&rb);
    printf("[PASS] test_fifo_order\n");
}

/* 测试3: 填满后无法继续入队 (单线程下 next==tail) */
static void test_full_buffer(void)
{
    ring_buffer_t rb;
    ring_init(&rb);

    /* 填入 RING_SIZE-1 个 (留一个空位区分满/空) */
    int dummy = 0;
    for (int i = 0; i < RING_SIZE - 1; i++) {
        ring_push(&rb, &dummy, sizeof(int));
    }
    assert(ring_count(&rb) == RING_SIZE - 1);

    /* 逐个出队再入队, 验证环绕 */
    void *data;
    size_t size;
    ring_pop(&rb, &data, &size);
    assert(ring_count(&rb) == RING_SIZE - 2);

    ring_push(&rb, &dummy, sizeof(int));
    assert(ring_count(&rb) == RING_SIZE - 1);

    /* 清空 */
    for (int i = 0; i < RING_SIZE - 1; i++) {
        ring_pop(&rb, &data, &size);
    }
    assert(ring_count(&rb) == 0);

    ring_destroy(&rb);
    printf("[PASS] test_full_buffer\n");
}

/* 测试4: 多线程生产者-消费者 */
static ring_buffer_t g_rb;
static int g_produced[100];
static int g_consumed[100];
static volatile int g_done = 0;

static void *producer_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; i++) {
        g_produced[i] = i;
        ring_push(&g_rb, &g_produced[i], sizeof(int));
    }
    return NULL;
}

static void *consumer_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; i++) {
        void *data;
        size_t size;
        ring_pop(&g_rb, &data, &size);
        g_consumed[i] = *(int *)data;
    }
    g_done = 1;
    return NULL;
}

static void test_multithreaded(void)
{
    ring_init(&g_rb);

    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer_thread, NULL);
    pthread_create(&cons, NULL, consumer_thread, NULL);

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    /* 验证消费顺序与生产顺序一致 (单生产单消费 FIFO) */
    for (int i = 0; i < 100; i++) {
        assert(g_consumed[i] == i);
    }
    assert(g_done == 1);

    ring_destroy(&g_rb);
    printf("[PASS] test_multithreaded\n");
}

/* --------------------------------------------------------------------
 * 主函数
 * -------------------------------------------------------------------- */
int main(void)
{
    printf("=== 环形缓冲单元测试开始 ===\n");
    test_basic_push_pop();
    test_fifo_order();
    test_full_buffer();
    test_multithreaded();
    printf("=== 全部测试通过 ===\n");
    return 0;
}
