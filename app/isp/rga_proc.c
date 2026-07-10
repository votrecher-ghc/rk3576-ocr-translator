/**
 * @file rga_proc.c
 * @brief 图像处理节点实现
 */
#include "rga_proc.h"
#include "rga_api.h"
#include "log.h"

#include <string.h>

int ocr_rga_proc_init(ocr_rga_proc_t *proc, ocr_buffer_pool_t *pool,
                      uint32_t out_w, uint32_t out_h, ocr_pixel_format_t out_fmt)
{
    if (!proc) return -1;
    memset(proc, 0, sizeof(*proc));
    proc->out_pool = pool;
    proc->out_width = out_w;
    proc->out_height = out_h;
    proc->out_format = out_fmt;
    proc->do_cvtcolor = 1;
    return 0;
}

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
    out->width = proc->out_width;
    out->height = proc->out_height;
    out->format = proc->out_format;
    out->timestamp = buf->timestamp;
    out->frame_id = buf->frame_id;

    int ret;
    if (proc->do_cvtcolor && buf->format != proc->out_format) {
        /* 格式转换 + 缩放（RGA 一步完成） */
        ret = ocr_rga_resize(buf, out);
        if (ret != 0) {
            /* TODO: 若格式不同需先 cvtcolor */
            ret = ocr_rga_cvtcolor(buf, out);
        }
    } else {
        ret = ocr_rga_resize(buf, out);
    }

    if (ret != 0) {
        ocr_pool_release(proc->out_pool, out);
        return ret;
    }

    /* TODO: 将 out 投递到下游节点（display/ocr_det） */
    return 0;
}
