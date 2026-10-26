/**
 * @file rga_proc.c
 * @brief 图像处理节点实现
 *
 * RGA 节点从输出池取得空闲缓冲，继承帧号/时间戳，完成缩放或转换后发布新图像。
 * 输入由节点借给回调；输出由 emit 转移所有权，返回正数防止节点再次转发原始输入。
 */
#include "rga_proc.h"
#include "rga_api.h"
#include "log.h"

#include <string.h>

int ocr_rga_proc_init(ocr_rga_proc_t *proc, ocr_buffer_pool_t *pool,
                      uint32_t out_w, uint32_t out_h, ocr_pixel_format_t out_fmt)
{
    if (!proc || !pool || out_w == 0 || out_h == 0 ||
        out_fmt <= OCR_FMT_UNKNOWN || out_fmt > OCR_FMT_BGRA8888) return -1;
    memset(proc, 0, sizeof(*proc));
    proc->out_pool = pool;
    proc->out_width = out_w;
    proc->out_height = out_h;
    proc->out_format = out_fmt;
    proc->do_cvtcolor = 1;
    return 0;
}

/** @brief 借用输入并取得独占输出，处理成功后 emit；返回 1 告知节点不要再转发输入。 */
int ocr_rga_proc_process(ocr_pipeline_node_t *node, ocr_buffer_t *buf)
{
    if (!node || !buf) return -1;
    ocr_rga_proc_t *proc = (ocr_rga_proc_t *)node->user_ctx;
    if (!proc || !proc->out_pool) return -2;

    /* 从输出池获取一个空闲缓冲 */
    ocr_buffer_t *out = ocr_pool_acquire(proc->out_pool);
    if (!out) {
        LOG_W("RGA 处理：输出缓冲池耗尽");
        return -3;
    }
    /* 处理配置与输出池必须一致；不能只改 out 的元数据来伪装尺寸，因为实际
     * 分配容量/stride 仍属于旧尺寸，RGA 可能越界写入。 */
    if (out->width != proc->out_width || out->height != proc->out_height ||
        out->format != proc->out_format || out->plane_count == 0) {
        LOG_E("RGA output pool layout does not match processor configuration");
        (void)ocr_pool_release(proc->out_pool, out);
        return -4;
    }
    out->timestamp = buf->timestamp;
    out->frame_id = buf->frame_id;

    int ret;
    if (proc->do_cvtcolor && buf->format != proc->out_format &&
        buf->width == out->width && buf->height == out->height) {
        ret = ocr_rga_cvtcolor(buf, out);
    } else {
        /* 需要缩放时让 librga 对支持的格式执行组合 blit，不在 CPU 中转色。
         * 即便尺寸、格式都相同，仍需把输入图像写入另一块 out，不能空操作返回。 */
        ret = ocr_rga_resize(buf, out);
    }

    if (ret != 0) {
        ocr_pool_release(proc->out_pool, out);
        return ret;
    }

    /* 成功提交后 emit 消费 out 的原引用，包括没有任何下游接收的情况。
     * 只在负数错误时，out 引用仍归本回调，需要显式归还。 */
    int emitted = ocr_node_emit(node, out);
    if (emitted < 0) {
        (void)ocr_pool_release(proc->out_pool, out);
        return -5;
    }
    return 1; /* 输出已由 ocr_node_emit 处理，禁止转发原输入。 */
}
