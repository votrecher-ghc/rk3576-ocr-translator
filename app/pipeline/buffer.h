#ifndef OCR_PIPELINE_BUFFER_H
#define OCR_PIPELINE_BUFFER_H

/** @file buffer.h @brief DMA-BUF metadata and reference counting.
 *
 * 一个结构体描述一块 DMA-BUF 及其中的图像布局；多个逻辑平面可以共享同一个 fd。
 * refcount 是应用层使用计数，归零执行回收回调；它不等于内核 fd 引用计数，也不代表硬件已完成。
 */

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    OCR_FMT_UNKNOWN = 0,
    OCR_FMT_NV12,
    OCR_FMT_NV16,
    OCR_FMT_YUYV,
    OCR_FMT_RGB565,
    OCR_FMT_RGB888,
    OCR_FMT_ARGB8888,
    OCR_FMT_BGRA8888,
} ocr_pixel_format_t;

struct ocr_buffer;
typedef void (*ocr_buffer_release_fn)(struct ocr_buffer *buf,
                                      void *user_data);

typedef struct ocr_buffer {
    int               fd;         /* DMA-BUF 文件描述符，-1 表示无效；池注册后由池关闭，不是物理地址。 */
    void             *mmap_addr;  /* CPU 映射起点，NULL 表示未映射；访问像素前仍须执行相应缓存同步。 */
    size_t            size;       /* 整块缓冲的字节数，含行对齐和全部逻辑平面。 */
    uint32_t          plane_count; /* 共用 fd 的逻辑平面数，1..4；NV12 为 Y、UV 两平面，不是两个 fd。 */
    uint32_t          strides[4];  /* 前 plane_count 项有效；每平面一行占用的字节数，含对齐填充。 */
    uint32_t          offsets[4];  /* 前 plane_count 项有效；每平面相对整块 DMA-BUF 起点的字节偏移。 */
    uint32_t          width;       /* 有效图像宽度，单位为像素。 */
    uint32_t          height;      /* 有效图像高度，不包含布局填充行。 */
    ocr_pixel_format_t format;     /* 决定像素解释方式及平面布局。 */
    uint32_t          index;       /* 缓冲池槽位，采集时也对应 V4L2 buffer index。 */
    atomic_int        refcount;    /* 应用引用数；最后一次 unref 触发回收，本身不能证明摄像头/RGA 已完成。 */
    uint64_t          timestamp;   /* 当前图像帧时间戳；采集路径使用单调时钟纳秒。 */
    uint64_t          frame_id;    /* 图像帧序号；复用同一 fd 时该值会更新。 */
    int               owned;       /* 池初始化置 1、销毁置 0 的归属标记；unref 不依据它直接关闭 fd。 */
    ocr_buffer_release_fn release_cb; /* 引用从 1 到 0 的线程调用；池缓冲设为归还槽位，非释放图像内存。 */
    void             *release_user_data; /* 借用的回调上下文，池缓冲指向所属池；须活到最后一个引用释放。 */
} ocr_buffer_t;

/** @brief 初始化元数据，初始引用为 1；不分配 DMA 内存，fd 初始为 -1。 */
int ocr_buffer_init(ocr_buffer_t *buf);

/**
 * @brief 设置引用归零时的回调。
 * @note 必须在向其他线程发布缓冲前设置，修改期间由调用者保证独占访问。
 */
int ocr_buffer_set_release_callback(ocr_buffer_t *buf,
                                    ocr_buffer_release_fn release_cb,
                                    void *user_data);

/** Configure logical plane layout for DRM/RGA import.
 *
 * 校验平面数量、行跨度、偏移和所需空间，拒绝平面重叠或超出分配边界。
 */
int ocr_buffer_set_layout(ocr_buffer_t *buf, uint32_t plane_count,
                          const uint32_t *strides, const uint32_t *offsets);

/**
 * @brief 给仍有效的缓冲增加一个引用
 * @return 成功时为本次递增后的引用数，-1=参数无效、计数已归零或溢出
 * @note 调用前须已持有引用，或在保护共享指针的锁内操作；原子计数不能保护裸指针。
 *       返回值只是操作时的计数，其他线程随后可能继续增减。
 */
int ocr_buffer_ref(ocr_buffer_t *buf);

/**
 * @brief 消费调用者持有的一个引用；从 1 到 0 的线程同步执行 release_cb
 * @return 成功时为本次递减后的引用数，-1=参数无效或计数已归零
 * @note 即使返回正数，也不能继续使用刚释放的那份引用；其他使用者可能立即归还剩余引用。
 *       返回 0 时回调可能已将缓冲归还设备或池，调用者不得再访问该帧。
 *       调用前须结束自己提交的硬件访问；引用计数不等待设备完成，也不执行缓存同步。
 */
int ocr_buffer_unref(ocr_buffer_t *buf);

/** @brief 为已有 fd 建立共享 CPU 虚拟地址映射；映射成功不代表可以跳过缓存同步。 */
int ocr_buffer_mmap(ocr_buffer_t *buf);
/** @brief 仅撤销 CPU 映射，不关闭 fd，也不改变其他设备对内存的导入。 */
void ocr_buffer_munmap(ocr_buffer_t *buf);
const char *ocr_pixel_format_str(ocr_pixel_format_t fmt);

#endif /* OCR_PIPELINE_BUFFER_H */
