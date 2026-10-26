/**
 * @file pose_history.c
 * @brief 时间戳姿态环形缓冲与四元数 SLERP。
 *
 * 按严格递增的纳秒时间戳保存姿态，用图像采集时刻查询对应四元数。
 * 区间内使用 SLERP 插值，超出已有时间范围时取最近端点；调用方负责串行访问历史数据。
 */
#include "pose_history.h"

#include <math.h>
#include <string.h>

#define OCR_POSE_EPSILON 1.0e-8f

static int normalize_quaternion(quaternion_t *q)
{
    float norm;

    if (!q || !isfinite(q->w) || !isfinite(q->x) ||
        !isfinite(q->y) || !isfinite(q->z)) {
        return -1;
    }
    norm = hypotf(hypotf(q->w, q->x), hypotf(q->y, q->z));
    if (!isfinite(norm) || norm <= OCR_POSE_EPSILON) return -1;
    q->w /= norm;
    q->x /= norm;
    q->y /= norm;
    q->z /= norm;
    return 0;
}

static size_t physical_index(const ocr_pose_history_t *history,
                             size_t logical_index)
{
    return (history->head + logical_index) % OCR_POSE_HISTORY_CAPACITY;
}

static const ocr_pose_sample_t *sample_at(const ocr_pose_history_t *history,
                                          size_t logical_index)
{
    return &history->samples[physical_index(history, logical_index)];
}

/** @brief 四元数球面插值；点积为负时翻转终点，选择最短旋转弧，近重合时使用线性近似。 */
static int slerp(const quaternion_t *a, const quaternion_t *b,
                 float ratio, quaternion_t *out)
{
    quaternion_t end = *b;
    float dot = a->w * b->w + a->x * b->x +
                a->y * b->y + a->z * b->z;

    /* q 与 -q 表示同一姿态。把点积调为非负可沿最短球面弧插值，
     * 防止相邻样本仅因四元数符号不同就产生一整圈的虚假旋转。 */
    if (dot < 0.0f) {
        dot = -dot;
        end.w = -end.w;
        end.x = -end.x;
        end.y = -end.y;
        end.z = -end.z;
    }
    if (dot > 1.0f) dot = 1.0f;

    /* 几乎重合时 sin(theta) 太小，球面公式数值不稳；线性混合后归一化即可。 */
    if (dot > 0.9995f) {
        out->w = a->w + ratio * (end.w - a->w);
        out->x = a->x + ratio * (end.x - a->x);
        out->y = a->y + ratio * (end.y - a->y);
        out->z = a->z + ratio * (end.z - a->z);
        return normalize_quaternion(out);
    }

    {
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        float weight_a;
        float weight_b;

        if (!isfinite(theta) || fabsf(sin_theta) <= OCR_POSE_EPSILON)
            return -1;
        weight_a = sinf((1.0f - ratio) * theta) / sin_theta;
        weight_b = sinf(ratio * theta) / sin_theta;
        out->w = weight_a * a->w + weight_b * end.w;
        out->x = weight_a * a->x + weight_b * end.x;
        out->y = weight_a * a->y + weight_b * end.y;
        out->z = weight_a * a->z + weight_b * end.z;
    }
    return normalize_quaternion(out);
}

void ocr_pose_history_init(ocr_pose_history_t *history)
{
    if (!history) return;
    memset(history, 0, sizeof(*history));
}

void ocr_pose_history_reset(ocr_pose_history_t *history)
{
    ocr_pose_history_init(history);
}

/** @brief 只接受递增时间戳并归一化四元数；历史满时覆盖最旧样本。 */
int ocr_pose_history_push(ocr_pose_history_t *history,
                          int64_t timestamp_ns,
                          const quaternion_t *q)
{
    quaternion_t normalized;
    size_t write_index;

    if (!history || !q || timestamp_ns <= 0) return -1;
    normalized = *q;
    if (normalize_quaternion(&normalized) != 0) return -1;

    if (history->count > 0) {
        const ocr_pose_sample_t *latest =
            sample_at(history, history->count - 1U);
        if (timestamp_ns <= latest->timestamp_ns) return -2;
    }

    if (history->count < OCR_POSE_HISTORY_CAPACITY) {
        write_index = physical_index(history, history->count);
        ++history->count;
    } else {
        write_index = history->head;
        history->head = (history->head + 1U) % OCR_POSE_HISTORY_CAPACITY;
    }

    history->samples[write_index].timestamp_ns = timestamp_ns;
    history->samples[write_index].q = normalized;
    return 0;
}

/** @brief 二分定位包围目标时间的样本并插值，超出历史范围时返回最近端点。 */
int ocr_pose_history_sample(const ocr_pose_history_t *history,
                            int64_t timestamp_ns,
                            quaternion_t *q)
{
    const ocr_pose_sample_t *first;
    const ocr_pose_sample_t *last;
    size_t low;
    size_t high;

    if (!history || !q || history->count == 0 || timestamp_ns <= 0)
        return -1;

    first = sample_at(history, 0);
    last = sample_at(history, history->count - 1U);
    if (timestamp_ns <= first->timestamp_ns) {
        *q = first->q;
        return 0;
    }
    if (timestamp_ns >= last->timestamp_ns) {
        *q = last->q;
        return 0;
    }

    /* logical index 0 始终表示最旧样本，sample_at 再映射到环形数组的物理位置。
     * 时间严格递增保证二分有效，退出时 high/low 正好夹住图像查询时刻。 */
    low = 0;
    high = history->count - 1U;
    while (high - low > 1U) {
        size_t middle = low + (high - low) / 2U;
        if (sample_at(history, middle)->timestamp_ns <= timestamp_ns)
            low = middle;
        else
            high = middle;
    }

    {
        const ocr_pose_sample_t *a = sample_at(history, low);
        const ocr_pose_sample_t *b = sample_at(history, high);
        int64_t span = b->timestamp_ns - a->timestamp_ns;
        float ratio;

        if (span <= 0) return -2;
        /* 先在 int64 中做时间差，再以 double 相除；不要先把纳秒绝对时间转成
         * float，否则设备运行较久后相邻样本间的小时间差会被精度吞掉。 */
        ratio = (float)((double)(timestamp_ns - a->timestamp_ns) /
                        (double)span);
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        return slerp(&a->q, &b->q, ratio, q);
    }
}
