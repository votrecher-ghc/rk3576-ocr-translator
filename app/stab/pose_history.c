/**
 * @file pose_history.c
 * @brief 时间戳姿态环形缓冲与四元数 SLERP。
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

static int slerp(const quaternion_t *a, const quaternion_t *b,
                 float ratio, quaternion_t *out)
{
    quaternion_t end = *b;
    float dot = a->w * b->w + a->x * b->x +
                a->y * b->y + a->z * b->z;

    if (dot < 0.0f) {
        dot = -dot;
        end.w = -end.w;
        end.x = -end.x;
        end.y = -end.y;
        end.z = -end.z;
    }
    if (dot > 1.0f) dot = 1.0f;

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
        ratio = (float)((double)(timestamp_ns - a->timestamp_ns) /
                        (double)span);
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        return slerp(&a->q, &b->q, ratio, q);
    }
}
