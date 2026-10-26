/**
 * @file buffer.c
 * @brief DMA-BUF buffer metadata and reference counting.
 *
 * 一个结构体描述一块 DMA-BUF 及其中的图像布局；多个逻辑平面可以共享同一个 fd。
 * refcount 是应用层使用计数，归零执行回收回调；它不等于内核 fd 引用计数，也不代表硬件已完成。
 */
#include "buffer.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>

int ocr_buffer_init(ocr_buffer_t *buf)
{
    if (!buf) {
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->fd = -1;
    buf->format = OCR_FMT_UNKNOWN;
    /* 普通独立对象初始交给调用者一份引用。池会在 init 中改成 0，等 acquire
     * 时再授予引用；这里只初始化元数据，并未申请 fd 或内存映射。 */
    atomic_init(&buf->refcount, 1);
    return 0;
}

int ocr_buffer_set_release_callback(ocr_buffer_t *buf,
                                    ocr_buffer_release_fn release_cb,
                                    void *user_data)
{
    if (!buf) {
        return -1;
    }

    /* 回调与上下文必须成对配置。这里没有加锁，要求发布前独占设置，
     * 否则最后一次 unref 可能读到新回调配旧上下文，回收错误的资源。 */
    buf->release_cb = release_cb;
    buf->release_user_data = user_data;
    return 0;
}

/** @brief 校验平面数量、行跨度、偏移和所需空间，拒绝平面重叠或超出分配边界。 */
int ocr_buffer_set_layout(ocr_buffer_t *buf, uint32_t plane_count,
                          const uint32_t *strides, const uint32_t *offsets)
{
    uint32_t expected_planes;
    uint64_t row_bytes[4] = {0};
    uint64_t rows[4] = {0};
    uint64_t previous_end = 0;

    if (!buf || !strides || !offsets || plane_count == 0 || plane_count > 4 ||
        buf->size == 0 || buf->width == 0 || buf->height == 0)
        return -1;

    /* row_bytes 是“这一行有效像素占多少字节”，rows 是各平面有效行数。
     * 它们决定最低空间需求；strides 则由分配者给出，可以比 row_bytes 更大。
     * NV12 每 2x2 像素共享一组 UV，宽高必须为偶数，UV 行数是 Y 的一半；
     * NV16 只在水平方向共享 UV，因此只要求偶数宽，UV 与 Y 行数相同。 */
    switch (buf->format) {
    case OCR_FMT_NV12:
        if ((buf->width & 1U) || (buf->height & 1U)) return -2;
        expected_planes = 2;
        row_bytes[0] = buf->width;
        row_bytes[1] = buf->width;
        rows[0] = buf->height;
        rows[1] = buf->height / 2U;
        break;
    case OCR_FMT_NV16:
        if (buf->width & 1U) return -2;
        expected_planes = 2;
        row_bytes[0] = buf->width;
        row_bytes[1] = buf->width;
        rows[0] = buf->height;
        rows[1] = buf->height;
        break;
    case OCR_FMT_YUYV:
        if (buf->width & 1U) return -2;
        expected_planes = 1;
        row_bytes[0] = (uint64_t)buf->width * 2U;
        rows[0] = buf->height;
        break;
    case OCR_FMT_RGB565:
        expected_planes = 1;
        row_bytes[0] = (uint64_t)buf->width * 2U;
        rows[0] = buf->height;
        break;
    case OCR_FMT_RGB888:
        expected_planes = 1;
        row_bytes[0] = (uint64_t)buf->width * 3U;
        rows[0] = buf->height;
        break;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888:
        expected_planes = 1;
        row_bytes[0] = (uint64_t)buf->width * 4U;
        rows[0] = buf->height;
        break;
    default:
        return -2;
    }
    if (plane_count != expected_planes) return -2;

    for (uint32_t i = 0; i < plane_count; ++i) {
        uint64_t end;
        uint64_t allocation_end;

        /* 一行连有效像素都放不下属于非法 stride。后一平面的起点还必须在
         * 前一平面（含行尾填充）的末端之后，防止 Y/UV 等平面相互覆盖。 */
        if ((uint64_t)strides[i] < row_bytes[i] || offsets[i] >= buf->size ||
            (i > 0 && offsets[i] < previous_end)) {
            return -2;
        }
        /* end 检查最后一个有效像素；allocation_end 还覆盖末行尾部填充。
         * 两者都用 64 位计算，避免 uint32_t 乘法先溢出后绕过 size 校验。 */
        end = (uint64_t)offsets[i] +
              (uint64_t)strides[i] * (rows[i] - 1U) + row_bytes[i];
        allocation_end = (uint64_t)offsets[i] +
                         (uint64_t)strides[i] * rows[i];
        if (end > buf->size || allocation_end > buf->size) return -2;
        previous_end = allocation_end;
    }
    /* 全部验证成功才提交布局；中途失败保持旧布局，避免下游看到半更新状态。 */
    buf->plane_count = plane_count;
    memcpy(buf->strides, strides, plane_count * sizeof(strides[0]));
    memcpy(buf->offsets, offsets, plane_count * sizeof(offsets[0]));
    for (uint32_t i = plane_count; i < 4; ++i) {
        buf->strides[i] = 0;
        buf->offsets[i] = 0;
    }
    return 0;
}

/** @brief 只给仍有效的引用计数加一；不能对已经释放的缓冲复活引用。 */
int ocr_buffer_ref(ocr_buffer_t *buf)
{
    int current;

    if (!buf) {
        return -1;
    }

    /* ref 不能替代“已经拥有有效引用”或外部指针锁；裸指针可能已被池回收复用。
     * 例如 get_latest 必须持 latest_lock 取指针并加引用，不能解锁后再 ref。 */
    current = atomic_load_explicit(&buf->refcount, memory_order_acquire);
    for (;;) {
        if (current <= 0 || current == INT_MAX) {
            return -1;
        }
        /* CAS 只有在计数仍等于 current 时才写入 current+1；若其他线程抢先
         * 增减，CAS 会更新 current，我们重新检查，避免 load/add/store 丢失增量。
         * weak 允许无竞争时偶发失败，因而也必须放在重试循环中。 */
        if (atomic_compare_exchange_weak_explicit(&buf->refcount,
                                                  &current,
                                                  current + 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return current + 1;
        }
    }
}

/** @brief 用原子减计数保证只有从 1 到 0 的线程执行回收回调，重复释放返回错误。 */
int ocr_buffer_unref(ocr_buffer_t *buf)
{
    int current;
    int remaining;

    if (!buf) {
        return -1;
    }

    current = atomic_load_explicit(&buf->refcount, memory_order_acquire);
    for (;;) {
        /* 不直接 fetch_sub：计数已经为 0 时拒绝操作，防止下溢成负数。
         * 调用者仍须保证每份引用只释放一次，不能用此校验容忍任意悬空指针。 */
        if (current <= 0) {
            return -1;
        }
        /* 只有一个 CAS 能赢得 1->0，其余并发释放者不会重复执行 release_cb。
         * acq_rel 配对 CPU 侧引用交接，不会等待 RGA/显示硬件，也不会刷新 DMA 缓存。 */
        remaining = current - 1;
        if (atomic_compare_exchange_weak_explicit(&buf->refcount,
                                                  &current,
                                                  remaining,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }

    /* 最后一个使用者负责回收。这里只统计应用引用，调用前必须已结束硬件访问。 */
    if (remaining == 0 && buf->release_cb) {
        buf->release_cb(buf, buf->release_user_data);
    }
    return remaining;
}

/** @brief 为已有 fd 建立共享 CPU 虚拟地址映射；映射成功不代表可以跳过缓存同步。 */
int ocr_buffer_mmap(ocr_buffer_t *buf)
{
    void *addr;

    if (!buf || buf->fd < 0 || buf->size == 0) {
        return -1;
    }
    if (buf->mmap_addr) {
        return 0;
    }

    /* 与采集模块 mmap(video_fd, QUERYBUF_offset) 不同，这里直接映射整块
     * DMA-BUF fd，所以起始偏移为 0。MAP_SHARED 表示映射共享底层存储。 */
    addr = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                buf->fd, 0);
    if (addr == MAP_FAILED) {
        LOG_E("mmap fd=%d size=%zu failed: %s",
              buf->fd, buf->size, strerror(errno));
        return -2;
    }
    buf->mmap_addr = addr;
    return 0;
}

/** @brief 仅撤销 CPU 映射，不关闭 fd，也不改变其他设备对内存的导入。 */
void ocr_buffer_munmap(ocr_buffer_t *buf)
{
    if (!buf || !buf->mmap_addr) {
        return;
    }

    if (buf->size > 0) {
        (void)munmap(buf->mmap_addr, buf->size);
    }
    buf->mmap_addr = NULL;
}

const char *ocr_pixel_format_str(ocr_pixel_format_t fmt)
{
    switch (fmt) {
    case OCR_FMT_NV12:     return "NV12";
    case OCR_FMT_NV16:     return "NV16";
    case OCR_FMT_YUYV:     return "YUYV";
    case OCR_FMT_RGB565:   return "RGB565";
    case OCR_FMT_RGB888:   return "RGB888";
    case OCR_FMT_ARGB8888: return "ARGB8888";
    case OCR_FMT_BGRA8888: return "BGRA8888";
    default:               return "UNKNOWN";
    }
}
