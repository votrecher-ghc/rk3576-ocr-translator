/**
 * @file rga_api.c
 * @brief RGA 硬件加速封装实现
 *
 * 输入、输出通过 DMA-BUF fd 引用内存，布局由宽高、格式和行跨度描述。
 * 所有图像操作采用同步模式；成功返回代表本次 RGA 读写完成，调用方仍负责引用和缓冲复用。
 */
#include "rga_api.h"
#include "log.h"

#include <limits.h>
#include <string.h>

/* Rockchip RGA 头文件 */
#include "im2d.h"

/* ocr 像素格式 → RGA 格式 */
static int fmt_to_rga(ocr_pixel_format_t fmt)
{
    switch (fmt) {
        case OCR_FMT_NV12:     return RK_FORMAT_YCbCr_420_SP;
        case OCR_FMT_NV16:     return RK_FORMAT_YCbCr_422_SP;
        case OCR_FMT_YUYV:     return RK_FORMAT_YUYV_422;
        case OCR_FMT_RGB565:   return RK_FORMAT_RGB_565;
        case OCR_FMT_RGB888:   return RK_FORMAT_RGB_888;
        /* 项目格式名沿用 DRM 的 32 位字序，RGA 名称描述内存字节顺序。
         * 例如小端 ARGB8888 的实际字节是 B,G,R,A，所以对应 RGA BGRA；
         * 名称相反来自命名约定，不能为“保持名字相同”而改成另一种格式。 */
        case OCR_FMT_ARGB8888: return RK_FORMAT_BGRA_8888;
        case OCR_FMT_BGRA8888: return RK_FORMAT_ARGB_8888;
        default:               return -1;
    }
}

static uint32_t format_bytes_per_pixel(ocr_pixel_format_t format)
{
    switch (format) {
    case OCR_FMT_NV12:
    case OCR_FMT_NV16:     return 1; /* Y 每像素 1 字节；UV 另有平面，不能在此算总大小。 */
    case OCR_FMT_YUYV:
    case OCR_FMT_RGB565:   return 2;
    case OCR_FMT_RGB888:   return 3;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888: return 4;
    default:               return 0;
    }
}

static int wrap_buffer(const ocr_buffer_t *buffer, rga_buffer_t *rga_buffer)
{
    uint32_t bytes_per_pixel;
    uint64_t tight_stride;
    uint32_t byte_stride;
    uint32_t width_stride;
    uint32_t height_stride;
    int rga_format;

    if (!buffer || !rga_buffer || buffer->fd < 0 ||
        buffer->width == 0 || buffer->height == 0 ||
        buffer->width > INT_MAX || buffer->height > INT_MAX) {
        return -1;
    }
    /* RGA 只接收 fd 及布局，mmap_addr 是否存在不影响硬件读取。
     * uint32_t 转 librga 的 int 前检查 INT_MAX，避免大尺寸转换成负数。 */
    bytes_per_pixel = format_bytes_per_pixel(buffer->format);
    rga_format = fmt_to_rga(buffer->format);
    if (bytes_per_pixel == 0 || rga_format < 0) return -1;

    tight_stride = (uint64_t)buffer->width * bytes_per_pixel;
    if (tight_stride > UINT32_MAX) return -1;
    /* 优先用分配者记录的真实行跨度；仅未设置布局的旧调用回退为紧密排列。
     * 例如 RGB888 宽 640、字节 stride=1968，RGA wstride 应是 1968/3=656。 */
    byte_stride = buffer->plane_count > 0 ? buffer->strides[0] :
                  (uint32_t)tight_stride;
    if (byte_stride < tight_stride || byte_stride % bytes_per_pixel != 0) {
        return -1;
    }
    /* 项目保存字节跨度，RGA 接口需要像素跨度；不能直接把字节数传过去。 */
    width_stride = byte_stride / bytes_per_pixel;
    height_stride = buffer->height;
    if (width_stride > INT_MAX) return -1;

    if (buffer->plane_count > 0) {
        /* wrapbuffer_fd_t 没有独立的平面起点参数，要求 Y/RGB 从 fd 起点开始。
         * 非零 offset 不能简单忽略，否则 RGA 从错误地址读取。 */
        if (buffer->offsets[0] != 0) return -1;
        if (buffer->format == OCR_FMT_NV12 || buffer->format == OCR_FMT_NV16) {
            if (buffer->plane_count != 2 || buffer->strides[1] != byte_stride ||
                buffer->offsets[1] == 0 ||
                buffer->offsets[1] % byte_stride != 0) {
                return -1;
            }
            /* RGA 通过 wstride*hstride 推导 UV 起点，所以把实际 UV 偏移反解成
             * 分配高度。例：有效高 1080、UV 偏移=stride*1088，则 hstride=1088。
             * 上面的等跨度/整除检查保证这个布局可用 RGA 的参数完整表达。 */
            height_stride = buffer->offsets[1] / byte_stride;
            if (height_stride < buffer->height || height_stride > INT_MAX) {
                return -1;
            }
        } else if (buffer->plane_count != 1) {
            return -1;
        }
    }

    memset(rga_buffer, 0, sizeof(*rga_buffer));
    /* width/height 是有效图像范围，width_stride/height_stride 是分配布局。
     * 包装步骤只构建描述符，不缩放、不复制，也不取得一份应用层 refcount；
     * 源/目标引用由调用者持有到同步处理函数返回。 */
    *rga_buffer = wrapbuffer_fd_t(buffer->fd, (int)buffer->width,
                                  (int)buffer->height, (int)width_stride,
                                  (int)height_stride, rga_format);
    return rga_buffer->width > 0 && rga_buffer->height > 0 ? 0 : -1;
}

int ocr_rga_wrap_fd(int fd, uint32_t width, uint32_t height,
                    ocr_pixel_format_t format, void *handle)
{
    int rga_fmt = fmt_to_rga(format);
    if (rga_fmt < 0 || fd < 0 || width == 0 || height == 0 || !handle) return -1;

    rga_buffer_t *rga_buf = (rga_buffer_t *)handle;
    memset(rga_buf, 0, sizeof(*rga_buf));
    if (width > INT_MAX || height > INT_MAX) return -1;
    /* 简化接口假定有效宽高就是分配跨度。存在对齐填充时应使用 ocr_buffer_t
     * 携带的真实布局走 wrap_buffer，不能把有 padding 的 fd 当作紧密图像。 */
    *rga_buf = wrapbuffer_fd_t(fd, (int)width, (int)height,
                               (int)width, (int)height, rga_fmt);
    if (rga_buf->width == 0) {
        LOG_E("wrapbuffer_fd 失败 fd=%d", fd);
        return -2;
    }
    return 0;
}

int ocr_rga_resize(const ocr_buffer_t *src, ocr_buffer_t *dst)
{
    if (!src || !dst || src->fd < 0 || dst->fd < 0 ||
        src->width == 0 || src->height == 0 ||
        dst->width == 0 || dst->height == 0) return -1;
    /* src 是采集内存 A 的只读描述，dst 是输出内存 B 的写入描述。
     * 两个 fd 分别包装，不需要把 A 的像素先复制到 CPU 临时数组。 */
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    /* 同步点二：末尾 sync=1，等 RGA 读完 src、写完 dst 后才返回。
     * 不能只改成异步后立即发布 dst；异步模式还需要完成 fence 和引用保活。 */
    IM_STATUS status = imresize_t(src_rga, dst_rga, 0.0, 0.0,
                                  IM_INTERP_DEFAULT, 1);
    /* librga 的状态枚举与本项目的 0/负数返回约定不同，在封装边界统一转换。
     * 失败不向下游发布 dst；上层负责归还这次取得的输出池引用。 */
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imresize 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_crop(const ocr_buffer_t *src, ocr_buffer_t *dst,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    /* 用 w > width-x 而不是 x+w > width，避免无符号加法溢出。
     * NV12 的 x/y/w/h 必须全为偶数以对齐 2x2 色度采样；NV16/YUYV 只需
     * x/w 偶数。按位或后检查最低位，可一次判断是否有任一参数为奇数。 */
    if (!src || !dst || w == 0 || h == 0 || x >= src->width || y >= src->height ||
        w > src->width - x || h > src->height - y ||
        (src->format == OCR_FMT_NV12 && ((x | y | w | h) & 1U)) ||
        ((src->format == OCR_FMT_NV16 || src->format == OCR_FMT_YUYV) &&
         ((x | w) & 1U))) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    /* src_rect 描述输入 A 的裁剪区域，dst 描述目标 B 的完整尺寸。
     * 这里只传裁剪参数，源元数据的 fd/width/offset 不会被改成“子图”。 */
    im_rect src_rect = { .x = (int)x, .y = (int)y, .width = (int)w, .height = (int)h };

    /* 防抖路径也采用同步裁剪，返回后才能交接输出或归还输入。 */
    IM_STATUS status = imcrop_t(src_rga, dst_rga, src_rect, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imcrop 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_rotate(const ocr_buffer_t *src, ocr_buffer_t *dst, ocr_rga_rotate_t angle)
{
    if (!src || !dst) return -1;
    /* 90/270 度会交换有效宽高；180/0 度保持尺寸，否则目标布局与旋转结果不符。 */
    if ((angle == OCR_RGA_ROTATE_90 || angle == OCR_RGA_ROTATE_270) &&
        (dst->width != src->height || dst->height != src->width)) return -1;
    if ((angle == OCR_RGA_ROTATE_0 || angle == OCR_RGA_ROTATE_180) &&
        (dst->width != src->width || dst->height != src->height)) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    int transform;
    switch (angle) {
    case OCR_RGA_ROTATE_0:   transform = 0; break;
    case OCR_RGA_ROTATE_90:  transform = IM_HAL_TRANSFORM_ROT_90; break;
    case OCR_RGA_ROTATE_180: transform = IM_HAL_TRANSFORM_ROT_180; break;
    case OCR_RGA_ROTATE_270: transform = IM_HAL_TRANSFORM_ROT_270; break;
    default: return -1;
    }
    /* 0 度仍需要把输入内容写入另一块输出，所以执行硬件 copy，而非直接返回。
     * 所有分支均同步；这属于设备内存带宽开销，零 CPU 拷贝并不等于没有数据搬运。 */
    IM_STATUS status = transform == 0 ? imcopy_t(src_rga, dst_rga, 1) :
                                        imrotate_t(src_rga, dst_rga,
                                                   transform, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imrotate 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_translate(const ocr_buffer_t *src, ocr_buffer_t *dst,
                      int32_t dx, int32_t dy)
{
    if (!src || !dst || src->width != dst->width || src->height != dst->height ||
        dx <= -(int32_t)src->width || dx >= (int32_t)dst->width ||
        dy <= -(int32_t)src->height || dy >= (int32_t)dst->height) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    /* dx/dy 是像素位移，不是 fd 字节偏移；源/目标保持等尺寸，由 RGA 完成搬移。 */
    IM_STATUS status = imtranslate_t(src_rga, dst_rga, dx, dy, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imtranslate 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_cvtcolor(const ocr_buffer_t *src, ocr_buffer_t *dst)
{
    /* 此入口只做等尺寸颜色转换，缩放并转色的组合由 resize 路径交给 librga。 */
    if (!src || !dst || src->format == dst->format ||
        src->width != dst->width || src->height != dst->height) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    IM_STATUS status = imcvtcolor_t(src_rga, dst_rga,
                                    fmt_to_rga(src->format),
                                    fmt_to_rga(dst->format),
                                    IM_COLOR_SPACE_DEFAULT, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imcvtcolor 失败: %d", status);
        return -4;
    }
    return 0;
}
