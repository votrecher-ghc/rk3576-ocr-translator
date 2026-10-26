/**
 * @file buffer_pool.c
 * @brief Fixed-capacity DMA-BUF pool implementation.
 *
 * 缓冲池只管理已注册 fd 的所有权和使用状态，不在 init 中分配图像内存。
 * acquire 得到一个引用，最后一次 unref 才回收；采集池还通过回收回调把缓冲重新交给摄像头。
 */
#include "buffer_pool.h"
#include "log.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int pool_buffer_index(const ocr_buffer_pool_t *pool,
                             const ocr_buffer_t *buf)
{
    uint32_t index;

    if (!pool || !buf) {
        return -1;
    }
    index = buf->index;
    /* 仅比较 index 不够：其他池也可能有相同槽号。还要检查对象地址确实落在
     * 本池对应槽位，防止把别的池或栈上的伪造对象归还到这里。 */
    if (index >= (uint32_t)pool->count ||
        buf != &pool->buffers[index]) {
        return -1;
    }
    return (int)index;
}

/** @brief 引用归零后在池锁内通知回收方，再把槽位发布为空闲，防止快速交接时状态错乱。 */
static void pool_recycle_buffer(ocr_buffer_t *buf, void *user_data)
{
    ocr_buffer_pool_t *pool = (ocr_buffer_pool_t *)user_data;
    int index;
    int recycled = 0;
    int callback_failed = 0;

    if (!pool || !buf || !pool->initialized) {
        return;
    }

    ocr_mutex_lock(&pool->lock);
    index = pool_buffer_index(pool, buf);
    /* 回调可能与其他 acquire 竞争，拿到锁后再次核对状态。
     * 必须是“属于本池、仍在使用、引用已归零”的槽位，且空闲表仍有空间，
     * 才能追加一次 free_list；否则重复追加会使同一内存被两个生产者同时取到。 */
    if (pool->initialized && index >= 0 &&
        pool->slot_state[index] == OCR_POOL_SLOT_IN_USE &&
        atomic_load_explicit(&buf->refcount, memory_order_acquire) == 0 &&
        pool->free_count < pool->registered_count) {
        /* 先通知再发布 FREE，并始终持有池锁。
         * 即使摄像头在 QBUF 后很快 DQBUF，下次 acquire 也要等本次回收完成。 */
        if (pool->recycle_cb &&
            pool->recycle_cb(buf, pool->recycle_user_data) != 0) {
            callback_failed = 1;
        }
        /* 只有应用引用完全消失，才把槽号放回可申请列表。
         * 回收通知失败时现有实现仍发布 FREE，但记录失败；V4L2 通知会另外
         * 设置 queue_error 并停采集，不能把 FREE 当成 QBUF 成功的凭证。 */
        pool->slot_state[index] = OCR_POOL_SLOT_FREE;
        pool->free_list[pool->free_count++] = index;
        recycled = 1;
    }
    ocr_mutex_unlock(&pool->lock);

    if (!recycled) {
        LOG_E("buffer pool rejected an invalid recycle request");
    } else if (callback_failed) {
        LOG_W("buffer pool recycle notification failed for slot %u", buf->index);
    }
}

int ocr_pool_set_recycle_callback(ocr_buffer_pool_t *pool,
                                  ocr_pool_recycle_fn callback,
                                  void *user_data)
{
    if (!pool || !pool->initialized) return -1;
    ocr_mutex_lock(&pool->lock);
    /* 不允许已经运行的池临时更换回收目标，避免有的缓冲被归还给旧设备、
     * 有的被归还给新设备；通知回调在 register 之前一次性配置。 */
    if (pool->registered_count != 0 || pool->free_count != 0) {
        ocr_mutex_unlock(&pool->lock);
        return -2;
    }
    pool->recycle_cb = callback;
    pool->recycle_user_data = user_data;
    ocr_mutex_unlock(&pool->lock);
    return 0;
}

int ocr_pool_is_idle(ocr_buffer_pool_t *pool)
{
    int idle;

    if (!pool || !pool->initialized) return -1;
    ocr_mutex_lock(&pool->lock);
    if (!pool->initialized) {
        ocr_mutex_unlock(&pool->lock);
        return -1;
    }
    /* 检查的是全部已注册槽位是否无应用引用；对采集池不表示摄像头已停流。
     * 因而 start 在旧线程和旧流收尾后才依此判断能否把所有槽重新 QBUF。 */
    idle = pool->registered_count == pool->free_count;
    ocr_mutex_unlock(&pool->lock);
    return idle;
}

int ocr_pool_init(ocr_buffer_pool_t *pool, int count)
{
    if (!pool || count <= 0 || count > OCR_POOL_MAX_BUFS) {
        return -1;
    }

    memset(pool, 0, sizeof(*pool));
    if (ocr_mutex_init(&pool->lock) != 0) {
        return -2;
    }

    pool->count = count;
    for (int i = 0; i < count; ++i) {
        if (ocr_buffer_init(&pool->buffers[i]) != 0) {
            ocr_mutex_destroy(&pool->lock);
            memset(pool, 0, sizeof(*pool));
            return -2;
        }
        /* 每个槽位的结构体一直留在数组原处，仅在复用时更新帧号/时间戳。
         * release_cb 指向池回收；recycle_cb 才是整池可选的设备通知，两层不要混淆。 */
        pool->buffers[i].index = (uint32_t)i;
        pool->buffers[i].owned = 1;
        ocr_buffer_set_release_callback(&pool->buffers[i],
                                        pool_recycle_buffer, pool);
        /* buffer_init 默认给独立对象 1 个引用，但池中未分配的槽位必须从 0 开始。
         * 这里直接 store 而不 unref，避免尚未注册的槽位误触发回收回调。 */
        atomic_store_explicit(&pool->buffers[i].refcount, 0,
                              memory_order_release);
        pool->slot_state[i] = OCR_POOL_SLOT_UNUSED;
    }
    pool->initialized = 1;
    return 0;
}

/** @brief 把 fd 所有权转交池并设置初始布局；失败时 fd 仍由调用者负责关闭。 */
int ocr_pool_register(ocr_buffer_pool_t *pool, int index, int fd, size_t size,
                      uint32_t width, uint32_t height,
                      ocr_pixel_format_t format)
{
    if (!pool || !pool->initialized || index < 0 || index >= pool->count ||
        fd < 0 || size == 0 || width == 0 || height == 0 ||
        format <= OCR_FMT_UNKNOWN || format > OCR_FMT_BGRA8888 ||
        fcntl(fd, F_GETFD) < 0) {
        return -1;
    }

    ocr_mutex_lock(&pool->lock);
    if (!pool->initialized ||
        pool->slot_state[index] != OCR_POOL_SLOT_UNUSED) {
        ocr_mutex_unlock(&pool->lock);
        return -2;
    }

    /* 同一个 fd 整数只能移交一次，否则销毁时可能重复 close。
     * dup 得到的另一 fd 整数仍可能指向同一内存；这里不检测底层内存别名，
     * 独立槽位是否确实拥有不同图像存储由分配者保证。 */
    for (int i = 0; i < pool->count; ++i) {
        if (pool->slot_state[i] != OCR_POOL_SLOT_UNUSED &&
            pool->buffers[i].fd == fd) {
            ocr_mutex_unlock(&pool->lock);
            return -2;
        }
    }

    /* register 先建立按有效宽度紧密排列的默认布局。V4L2 与 DMA-HEAP 调用者
     * 随后在发布槽位前以实际 stride/offset 覆盖；默认布局不代表硬件无对齐。 */
    uint32_t strides[4] = {0};
    uint32_t offsets[4] = {0};
    uint32_t planes = 1;
    uint64_t row_bytes = 0;
    uint64_t y_bytes = (uint64_t)width * height;
    uint64_t required = 0;
    switch (format) {
    case OCR_FMT_NV12:
        if ((width & 1U) || (height & 1U) || y_bytes > UINT32_MAX) {
            ocr_mutex_unlock(&pool->lock);
            return -3;
        }
        planes = 2;
        strides[0] = width;
        strides[1] = width;
        offsets[1] = (uint32_t)y_bytes;
        required = y_bytes + y_bytes / 2U;
        break;
    case OCR_FMT_NV16:
        if ((width & 1U) || y_bytes > UINT32_MAX) {
            ocr_mutex_unlock(&pool->lock);
            return -3;
        }
        planes = 2;
        strides[0] = width;
        strides[1] = width;
        offsets[1] = (uint32_t)y_bytes;
        required = y_bytes * 2U;
        break;
    case OCR_FMT_YUYV:
    case OCR_FMT_RGB565:
        row_bytes = (uint64_t)width * 2U;
        required = row_bytes * height;
        break;
    case OCR_FMT_RGB888:
        row_bytes = (uint64_t)width * 3U;
        required = row_bytes * height;
        break;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888:
        row_bytes = (uint64_t)width * 4U;
        required = row_bytes * height;
        break;
    default:
        ocr_mutex_unlock(&pool->lock);
        return -3;
    }
    if (planes == 1) {
        if (row_bytes > UINT32_MAX) {
            ocr_mutex_unlock(&pool->lock);
            return -3;
        }
        strides[0] = (uint32_t)row_bytes;
    }
    if (required > size) {
        ocr_mutex_unlock(&pool->lock);
        return -3;
    }

    ocr_buffer_t *buf = &pool->buffers[index];
    buf->fd = fd;
    buf->size = size;
    buf->width = width;
    buf->height = height;
    buf->format = format;
    if (ocr_buffer_set_layout(buf, planes, strides, offsets) != 0) {
        buf->fd = -1;
        buf->size = 0;
        ocr_mutex_unlock(&pool->lock);
        return -3;
    }
    /* 从此处开始 fd 的关闭责任属于池；之前任一错误返回都未成功接管 fd。
     * 注册仅增加可分配槽位，不给调用者帧引用，所以计数仍为 0。 */
    pool->slot_state[index] = OCR_POOL_SLOT_FREE;
    pool->free_list[pool->free_count++] = index;
    ++pool->registered_count;
    ocr_mutex_unlock(&pool->lock);
    return 0;
}

/** @brief 从空闲列表取一块缓冲并赋予一个引用；池耗尽时返回 NULL，不等待。 */
ocr_buffer_t *ocr_pool_acquire(ocr_buffer_pool_t *pool)
{
    ocr_buffer_t *buf;
    int index;

    if (!pool || !pool->initialized) {
        return NULL;
    }

    ocr_mutex_lock(&pool->lock);
    if (!pool->initialized || pool->free_count == 0) {
        ocr_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* free_list 是槽号栈，先减元素数再取末项；它不是摄像头完成队列。
     * 输出池可任取空闲项，采集池必须使用 acquire_index(DQBUF.index)。 */
    index = pool->free_list[--pool->free_count];
    if (index < 0 || index >= pool->count ||
        pool->slot_state[index] != OCR_POOL_SLOT_FREE) {
        ++pool->free_count;
        ocr_mutex_unlock(&pool->lock);
        LOG_E("buffer pool free-list corruption detected");
        return NULL;
    }

    buf = &pool->buffers[index];
    pool->slot_state[index] = OCR_POOL_SLOT_IN_USE;
    /* fd/映射/格式保持不变，清除的是上一帧标识；生产者填好新内容后再发布。
     * 这是从池中重新授予一个引用，允许 0->1；普通 buffer_ref 则不允许复活。 */
    buf->timestamp = 0;
    buf->frame_id = 0;
    atomic_store_explicit(&buf->refcount, 1, memory_order_release);
    ocr_mutex_unlock(&pool->lock);
    return buf;
}

/** @brief 认领 DQBUF 指定的槽位，防止拿到与摄像头实际完成帧不同的缓冲。 */
ocr_buffer_t *ocr_pool_acquire_index(ocr_buffer_pool_t *pool, int index)
{
    ocr_buffer_t *buf;
    int free_pos = -1;

    if (!pool || !pool->initialized || index < 0 || index >= pool->count) {
        return NULL;
    }

    ocr_mutex_lock(&pool->lock);
    if (!pool->initialized ||
        pool->slot_state[index] != OCR_POOL_SLOT_FREE) {
        ocr_mutex_unlock(&pool->lock);
        return NULL;
    }
    for (int i = 0; i < pool->free_count; ++i) {
        if (pool->free_list[i] == index) {
            free_pos = i;
            break;
        }
    }
    if (free_pos < 0) {
        ocr_mutex_unlock(&pool->lock);
        LOG_E("buffer pool free-list corruption detected for slot %d", index);
        return NULL;
    }

    /* 删除指定槽号时用最后一项填洞，无需移动整个数组；空闲表不保证帧顺序。
     * 从检查 FREE 到设置 IN_USE 都在同一把锁内，避免两个线程认领同一槽。 */
    pool->free_list[free_pos] = pool->free_list[--pool->free_count];
    pool->slot_state[index] = OCR_POOL_SLOT_IN_USE;
    buf = &pool->buffers[index];
    buf->timestamp = 0;
    buf->frame_id = 0;
    atomic_store_explicit(&buf->refcount, 1, memory_order_release);
    ocr_mutex_unlock(&pool->lock);
    return buf;
}

/** @brief 确认缓冲属于此池后释放一个引用；真正回收由最后一次 unref 触发。 */
int ocr_pool_release(ocr_buffer_pool_t *pool, ocr_buffer_t *buf)
{
    int index;
    int valid;

    if (!pool || !buf || !pool->initialized) {
        return -1;
    }

    ocr_mutex_lock(&pool->lock);
    index = pool_buffer_index(pool, buf);
    valid = pool->initialized && index >= 0 &&
            pool->slot_state[index] == OCR_POOL_SLOT_IN_USE &&
            buf->release_cb == pool_recycle_buffer &&
            buf->release_user_data == pool;
    ocr_mutex_unlock(&pool->lock);

    /* 必须先解池锁再 unref：最后一次 unref 会同步调用 pool_recycle_buffer，
     * 后者要再次拿同一把锁，若仍持锁会在本线程内死锁。 */
    if (!valid || ocr_buffer_unref(buf) < 0) {
        return -2;
    }
    return 0;
}

/** @brief 仅在所有缓冲都空闲时解除映射并关闭 fd；忙碌时返回 -2，保留池供重试。 */
int ocr_pool_destroy(ocr_buffer_pool_t *pool)
{
    if (!pool || !pool->initialized) {
        return 0;
    }

    ocr_mutex_lock(&pool->lock);
    if (pool->registered_count != pool->free_count) {
        ocr_mutex_unlock(&pool->lock);
        return -2;
    }

    /* 销毁要求上层先停止新的发布/申请；此处的计数校验不能替代整个对象的
     * 外部生命周期管理。池忙时前面直接返回，不能强行关闭下游正在使用的 fd。 */
    pool->initialized = 0;
    for (int i = 0; i < pool->count; ++i) {
        ocr_buffer_t *buf = &pool->buffers[i];

        ocr_buffer_set_release_callback(buf, NULL, NULL);
        ocr_buffer_munmap(buf);
        if (buf->fd >= 0) {
            (void)close(buf->fd);
            buf->fd = -1;
        }
        buf->owned = 0;
        pool->slot_state[i] = OCR_POOL_SLOT_UNUSED;
    }
    pool->count = 0;
    pool->registered_count = 0;
    pool->free_count = 0;
    pool->recycle_cb = NULL;
    pool->recycle_user_data = NULL;
    ocr_mutex_unlock(&pool->lock);
    ocr_mutex_destroy(&pool->lock);
    return 0;
}
