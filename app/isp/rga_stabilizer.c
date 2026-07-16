/**
 * @file rga_stabilizer.c
 * @brief 防抖补偿实现
 */
#include "rga_stabilizer.h"
#include "rga_api.h"
#include "math_utils.h"
#include "log.h"

#include <string.h>
#include <math.h>

int ocr_rga_stab_init(ocr_rga_stabilizer_t *stab, float alpha)
{
    if (!stab) return -1;
    memset(stab, 0, sizeof(*stab));
    stab->alpha = ocr_clamp_f(alpha, 0.0f, 1.0f);
    stab->enabled = 1;
    stab->cur.scale = 1.0f;
    stab->target.scale = 1.0f;
    return 0;
}

int ocr_rga_stab_update(ocr_rga_stabilizer_t *stab, const ocr_stab_params_t *params)
{
    if (!stab || !params) return -1;
    if (!isfinite(params->dx) || !isfinite(params->dy) ||
        !isfinite(params->angle) || !isfinite(params->scale) ||
        params->scale < 1.0f) return -2;
    stab->target = *params;
    stab->target.scale = ocr_clamp_f(stab->target.scale, 1.0f, 2.0f);
    return 0;
}

int ocr_rga_stab_apply(ocr_rga_stabilizer_t *stab, ocr_buffer_t *src, ocr_buffer_t *dst)
{
    if (!stab || !src || !dst) return -1;
    if (!stab->enabled) {
        /* 未启用：直接拷贝/透传 */
        return ocr_rga_resize(src, dst);
    }

    /* 低通滤波平滑补偿参数，避免抖动 */
    stab->cur.dx    = ocr_lowpass(stab->cur.dx,    stab->target.dx,    stab->alpha);
    stab->cur.dy    = ocr_lowpass(stab->cur.dy,    stab->target.dy,    stab->alpha);
    stab->cur.angle = ocr_lowpass(stab->cur.angle, stab->target.angle, stab->alpha);
    stab->cur.scale = ocr_lowpass(stab->cur.scale, stab->target.scale, stab->alpha);

    int ret;
    if (fabsf(stab->cur.angle) > 0.1f) {
        /* RK3576 RGA exposes only right-angle rotation through im2d. Small
         * arbitrary roll is compensated by the crop/translation envelope. */
        LOG_D("RGA arbitrary rotation %.2f deg approximated by crop/shift",
              stab->cur.angle);
    }

    float scale = ocr_clamp_f(stab->cur.scale, 1.0f, 2.0f);
    uint32_t crop_w = (uint32_t)((float)src->width / scale);
    uint32_t crop_h = (uint32_t)((float)src->height / scale);
    crop_w &= ~1U;
    crop_h &= ~1U;
    if (crop_w < 2 || crop_h < 2) return -2;

    int max_x = (int)src->width - (int)crop_w;
    int max_y = (int)src->height - (int)crop_h;
    int crop_x = max_x / 2 - (int)lrintf(stab->cur.dx);
    int crop_y = max_y / 2 - (int)lrintf(stab->cur.dy);
    crop_x = ocr_clamp_i(crop_x, 0, max_x);
    crop_y = ocr_clamp_i(crop_y, 0, max_y);
    if (src->format == OCR_FMT_NV12) {
        crop_x &= ~1;
        crop_y &= ~1;
    } else if (src->format == OCR_FMT_NV16 || src->format == OCR_FMT_YUYV) {
        crop_x &= ~1;
    }
    ret = ocr_rga_crop(src, dst, (uint32_t)crop_x, (uint32_t)crop_y,
                       crop_w, crop_h);

    return ret;
}

void ocr_rga_stab_enable(ocr_rga_stabilizer_t *stab, int enable)
{
    if (!stab) return;
    stab->enabled = enable;
}
