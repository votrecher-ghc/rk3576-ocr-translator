#ifndef OCR_STAB_POSE_HISTORY_H
#define OCR_STAB_POSE_HISTORY_H
/**
 * @file pose_history.h
 * @brief 按 IIO 时间戳保存四元数，并为摄像头帧插值姿态。
 */

#include "attitude_fusion.h"

#include <stddef.h>
#include <stdint.h>

#define OCR_POSE_HISTORY_CAPACITY 4096U

typedef struct {
    int64_t timestamp_ns;
    quaternion_t q;
} ocr_pose_sample_t;

typedef struct {
    ocr_pose_sample_t samples[OCR_POSE_HISTORY_CAPACITY];
    size_t head;
    size_t count;
} ocr_pose_history_t;

void ocr_pose_history_init(ocr_pose_history_t *history);
void ocr_pose_history_reset(ocr_pose_history_t *history);

/**
 * @brief 写入一个时间戳单调递增的姿态样本
 * @return 0=成功，-1=参数错误，-2=时间戳倒退
 */
int ocr_pose_history_push(ocr_pose_history_t *history,
                          int64_t timestamp_ns,
                          const quaternion_t *q);

/**
 * @brief 查询目标时间的姿态；区间内做 SLERP，区间外钳位到最近样本
 */
int ocr_pose_history_sample(const ocr_pose_history_t *history,
                            int64_t timestamp_ns,
                            quaternion_t *q);

#endif /* OCR_STAB_POSE_HISTORY_H */
