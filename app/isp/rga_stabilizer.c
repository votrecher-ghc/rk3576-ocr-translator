/**
 * @file rga_stabilizer.c
 * @brief 固定裁剪窗口的 RGA 防抖补偿实现。
 */
#include "rga_stabilizer.h"
#include "rga_api.h"
#include "math_utils.h"
#include "log.h"

#include <math.h>
#include <string.h>

int ocr_rga_stab_init(ocr_rga_stabilizer_t *stab, float alpha)
{
    if (!stab || !isfinite(alpha)) return -1;
    memset(stab, 0, sizeof(*stab));
    stab->alpha = ocr_clamp_f(alpha, 0.0f, 1.0f);
    stab->enabled = 1;
    stab->cur.scale = 1.0f;
    stab->target.scale = 1.0f;
    return 0;
}

int ocr_rga_stab_update(ocr_rga_stabilizer_t *stab,
                        const ocr_stab_params_t *params)
{
    if (!stab || !params) return -1;
    if (!isfinite(params->dx) || !isfinite(params->dy) ||
        !isfinite(params->angle) || !isfinite(params->scale) ||
        params->scale < 1.0f) return -2;

    stab->target = *params;
    stab->target.scale = ocr_clamp_f(stab->target.scale, 1.0f, 2.0f);
    return 0;
}

static void fit_crop_to_destination(const ocr_buffer_t *src,
                                    const ocr_buffer_t *dst,
                                    float scale,
                                    uint32_t *crop_w,
                                    uint32_t *crop_h)
{
    float width = (float)src->width / scale;
    float height = (float)src->height / scale;
    float dst_aspect = (float)dst->width / (float)dst->height;
    float crop_aspect = width / height;

    if (crop_aspect > dst_aspect) {
        width = height * dst_aspect;
    } else if (crop_aspect < dst_aspect) {
        height = width / dst_aspect;
    }

    if (width > (float)src->width) width = (float)src->width;
    if (height > (float)src->height) height = (float)src->height;

    /* 先四舍五入再做色度对齐，避免 1600.0 的浮点误差变成 1598。 */
    *crop_w = (uint32_t)lroundf(width);
    *crop_h = (uint32_t)lroundf(height);
}

int ocr_rga_stab_apply(ocr_rga_stabilizer_t *stab,
                       ocr_buffer_t *src, ocr_buffer_t *dst)
{
    float scale;
    uint32_t crop_w;
    uint32_t crop_h;
    int max_x;
    int max_y;
    int crop_x;
    int crop_y;

    if (!stab || !src || !dst || src->width == 0 || src->height == 0 ||
        dst->width == 0 || dst->height == 0) {
        return -1;
    }
    if (!stab->enabled) return ocr_rga_resize(src, dst);

    /*
     * motion_compensate 已经生成平滑目标轨迹，这里必须直接使用最终
     * 参数。再次低通会增加一帧以上的延迟，使补偿出现拖尾和回摆。
     */
    stab->cur = stab->target;
    scale = ocr_clamp_f(stab->cur.scale, 1.0f, 2.0f);

    if (fabsf(stab->cur.angle) > 0.1f) {
        LOG_D("RGA cannot compensate arbitrary roll %.2f deg; ignored",
              stab->cur.angle);
    }

    fit_crop_to_destination(src, dst, scale, &crop_w, &crop_h);

    /* NV12/NV16/YUYV 的裁剪尺寸必须满足色度采样对齐。 */
    if (src->format == OCR_FMT_NV12) {
        crop_w &= ~1U;
        crop_h &= ~1U;
    } else if (src->format == OCR_FMT_NV16 || src->format == OCR_FMT_YUYV) {
        crop_w &= ~1U;
    }
    if (crop_w < 2 || crop_h < 2 ||
        crop_w > src->width || crop_h > src->height) {
        return -2;
    }

    max_x = (int)src->width - (int)crop_w;
    max_y = (int)src->height - (int)crop_h;

    /*
     * 1920x1080、scale=1.2 时得到固定 1600x900 窗口；dx/dy 只改变
     * 窗口位置，不改变窗口大小，因此预览不会出现动态缩放呼吸感。
     */
    crop_x = max_x / 2 - (int)lrintf(stab->cur.dx);
    crop_y = max_y / 2 - (int)lrintf(stab->cur.dy);
    crop_x = ocr_clamp_i(crop_x, 0, max_x);
    crop_y = ocr_clamp_i(crop_y, 0, max_y);

    if (src->format == OCR_FMT_NV12) {
        crop_x &= ~1;
        crop_y &= ~1;
    } else if (src->format == OCR_FMT_NV16 || src->format == OCR_FMT_YUYV) {
        crop_x &= ~1;
    }

    return ocr_rga_crop(src, dst, (uint32_t)crop_x, (uint32_t)crop_y,
                        crop_w, crop_h);
}

void ocr_rga_stab_enable(ocr_rga_stabilizer_t *stab, int enable)
{
    if (!stab) return;
    stab->enabled = enable;
}
