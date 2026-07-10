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
    stab->target = *params;
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

    /* 旋转（90 度整数倍用 RGA，任意角度 TODO: 需仿射变换） */
    int ret = 0;
    if (fabsf(stab->cur.angle) > 0.1f) {
        /* TODO: RGA 不支持任意角度旋转，需用 imresize + 平移近似，
         * 或改用 CPU 仿射变换。此处仅做平移补偿。 */
        LOG_D("防抖旋转角 %.2f 度（暂忽略）", stab->cur.angle);
    }

    /* 平移补偿（反向） */
    if (fabsf(stab->cur.dx) >= 1.0f || fabsf(stab->cur.dy) >= 1.0f) {
        ret = ocr_rga_translate(src, dst,
                                (int32_t)(-stab->cur.dx),
                                (int32_t)(-stab->cur.dy));
    } else {
        ret = ocr_rga_resize(src, dst);
    }

    return ret;
}

void ocr_rga_stab_enable(ocr_rga_stabilizer_t *stab, int enable)
{
    if (!stab) return;
    stab->enabled = enable;
}
