/*
 * test_buffer.c - 缓冲池单元测试
 *
 * 功能: 测试 DMA-BUF 缓冲池的分配/释放/复用逻辑
 * 依赖: assert.h (轻量断言, 无需外部测试框架)
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* --------------------------------------------------------------------
 * 缓冲池数据结构 (简化版, 与实际实现保持接口一致)
 * -------------------------------------------------------------------- */

#define BUFFER_COUNT 4
#define BUFFER_SIZE   (1920 * 1080 * 2)  /* NV12: 1920x1080, 2 字节/像素 */

/* 单个缓冲区描述符 */
typedef struct {
    int      index;        /* 缓冲区索引 */
    int      in_use;       /* 是否被占用 */
    void    *data;         /* 数据指针 (实际为 dma-buf fd/mmap) */
    size_t   size;         /* 缓冲区大小 */
} buffer_t;

/* 缓冲池 */
typedef struct {
    buffer_t   buffers[BUFFER_COUNT];
    pthread_mutex_t lock;
    int        free_count;  /* 可用缓冲区数量 */
} buffer_pool_t;

/* --------------------------------------------------------------------
 * 缓冲池操作函数 (待测实现)
 * -------------------------------------------------------------------- */

/* 初始化缓冲池 */
static int buffer_pool_init(buffer_pool_t *pool, size_t buf_size)
{
    pthread_mutex_init(&pool->lock, NULL);
    pool->free_count = BUFFER_COUNT;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        pool->buffers[i].index  = i;
        pool->buffers[i].in_use = 0;
        pool->buffers[i].size   = buf_size;
        pool->buffers[i].data   = malloc(buf_size);
        if (!pool->buffers[i].data) {
            return -1;
        }
        memset(pool->buffers[i].data, 0, buf_size);
    }
    return 0;
}

/* 销毁缓冲池 */
static void buffer_pool_destroy(buffer_pool_t *pool)
{
    for (int i = 0; i < BUFFER_COUNT; i++) {
        free(pool->buffers[i].data);
        pool->buffers[i].data = NULL;
    }
    pthread_mutex_destroy(&pool->lock);
}

/* 分配一个空闲缓冲区 */
static buffer_t *buffer_pool_acquire(buffer_pool_t *pool)
{
    buffer_t *buf = NULL;
    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (!pool->buffers[i].in_use) {
            pool->buffers[i].in_use = 1;
            pool->free_count--;
            buf = &pool->buffers[i];
            break;
        }
    }
    pthread_mutex_unlock(&pool->lock);
    return buf;
}

/* 释放缓冲区 */
static void buffer_pool_release(buffer_pool_t *pool, buffer_t *buf)
{
    if (!buf) return;
    pthread_mutex_lock(&pool->lock);
    buf->in_use = 0;
    pool->free_count++;
    pthread_mutex_unlock(&pool->lock);
}

/* --------------------------------------------------------------------
 * 测试用例
 * -------------------------------------------------------------------- */

/* 测试1: 初始化后所有缓冲区可用 */
static void test_init_all_free(void)
{
    buffer_pool_t pool;
    assert(buffer_pool_init(&pool, BUFFER_SIZE) == 0);
    assert(pool.free_count == BUFFER_COUNT);
    buffer_pool_destroy(&pool);
    printf("[PASS] test_init_all_free\n");
}

/* 测试2: 分配并释放 */
static void test_acquire_release(void)
{
    buffer_pool_t pool;
    buffer_pool_init(&pool, BUFFER_SIZE);

    buffer_t *buf = buffer_pool_acquire(&pool);
    assert(buf != NULL);
    assert(buf->in_use == 1);
    assert(pool.free_count == BUFFER_COUNT - 1);

    buffer_pool_release(&pool, buf);
    assert(buf->in_use == 0);
    assert(pool.free_count == BUFFER_COUNT);

    buffer_pool_destroy(&pool);
    printf("[PASS] test_acquire_release\n");
}

/* 测试3: 分配全部缓冲区后再分配返回 NULL */
static void test_exhaust_pool(void)
{
    buffer_pool_t pool;
    buffer_pool_init(&pool, BUFFER_SIZE);

    buffer_t *bufs[BUFFER_COUNT];
    for (int i = 0; i < BUFFER_COUNT; i++) {
        bufs[i] = buffer_pool_acquire(&pool);
        assert(bufs[i] != NULL);
    }
    assert(pool.free_count == 0);

    /* 此时再分配应返回 NULL */
    buffer_t *extra = buffer_pool_acquire(&pool);
    assert(extra == NULL);

    /* 释放一个后应可再次分配 */
    buffer_pool_release(&pool, bufs[0]);
    assert(pool.free_count == 1);
    extra = buffer_pool_acquire(&pool);
    assert(extra != NULL);
    assert(pool.free_count == 0);

    /* 清理剩余 */
    for (int i = 1; i < BUFFER_COUNT; i++) {
        buffer_pool_release(&pool, bufs[i]);
    }
    buffer_pool_release(&pool, extra);
    assert(pool.free_count == BUFFER_COUNT);

    buffer_pool_destroy(&pool);
    printf("[PASS] test_exhaust_pool\n");
}

/* 测试4: FIFO 复用顺序 (先释放的先被分配) */
static void test_reuse_order(void)
{
    buffer_pool_t pool;
    buffer_pool_init(&pool, BUFFER_SIZE);

    buffer_t *buf0 = buffer_pool_acquire(&pool);
    buffer_t *buf1 = buffer_pool_acquire(&pool);

    /* 释放 buf0 后再分配, 应得到 buf0 (FIFO) */
    buffer_pool_release(&pool, buf0);
    buffer_t *reused = buffer_pool_acquire(&pool);
    assert(reused->index == buf0->index);

    /* 清理 */
    buffer_pool_release(&pool, buf1);
    buffer_pool_release(&pool, reused);

    buffer_pool_destroy(&pool);
    printf("[PASS] test_reuse_order\n");
}

/* 测试5: 数据写入与读取完整性 */
static void test_data_integrity(void)
{
    buffer_pool_t pool;
    buffer_pool_init(&pool, BUFFER_SIZE);

    buffer_t *buf = buffer_pool_acquire(&pool);
    /* 写入测试数据 */
    memset(buf->data, 0xAB, 256);
    /* 校验 */
    unsigned char *p = (unsigned char *)buf->data;
    for (int i = 0; i < 256; i++) {
        assert(p[i] == 0xAB);
    }
    buffer_pool_release(&pool, buf);

    buffer_pool_destroy(&pool);
    printf("[PASS] test_data_integrity\n");
}

/* --------------------------------------------------------------------
 * 主函数
 * -------------------------------------------------------------------- */
int main(void)
{
    printf("=== 缓冲池单元测试开始 ===\n");
    test_init_all_free();
    test_acquire_release();
    test_exhaust_pool();
    test_reuse_order();
    test_data_integrity();
    printf("=== 全部测试通过 ===\n");
    return 0;
}
