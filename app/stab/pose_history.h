#ifndef OCR_STAB_POSE_HISTORY_H
#define OCR_STAB_POSE_HISTORY_H
/**
 * @file pose_history.h
 * @brief 按 IIO 时间戳保存四元数，并为摄像头帧插值姿态。
 *
 * 按严格递增的纳秒时间戳保存姿态，用图像采集时刻查询对应四元数。
 * 区间内使用 SLERP 插值，超出已有时间范围时取最近端点；调用方负责串行访问历史数据。
 */

#include "attitude_fusion.h"

#include <stddef.h>
#include <stdint.h>

#define OCR_POSE_HISTORY_CAPACITY 4096U

typedef struct {
    int64_t timestamp_ns; /**< 此姿态对应的采样时刻，单调纳秒；入历史要求严格大于上一个样本。 */
    quaternion_t q; /**< 该时刻单位四元数的值副本；入队时归一化，不借用融合器可变对象。 */
} ocr_pose_sample_t;

typedef struct {
    ocr_pose_sample_t samples[OCR_POSE_HISTORY_CAPACITY]; /**< 内嵌姿态环形历史，最多 OCR_POSE_HISTORY_CAPACITY 个；访问由姿态层串行化。 */
    size_t head; /**< 最旧有效样本的物理数组下标，历史满时覆盖该项并前移。 */
    size_t count; /**< 有效样本数；逻辑第 i 项映射到 (head+i)%capacity，用于按时间二分查找。 */
} ocr_pose_history_t;

/** @brief 清空内嵌历史，不分配内存；history 为 NULL 时不操作。 */
void ocr_pose_history_init(ocr_pose_history_t *history);
/** @brief 丢弃全部样本；调用者须与 push/sample 串行执行。 */
void ocr_pose_history_reset(ocr_pose_history_t *history);

/**
 * @brief 写入一个时间戳单调递增的姿态样本
 * @param[in,out] history 已初始化的历史，满时覆盖最旧样本
 * @param[in] timestamp_ns 正的采样时间，单位纳秒，须严格大于上一项
 * @param[in] q 有限且模长非零的四元数；保存归一化副本，不修改原值
 * @return 0=成功，-1=参数或四元数无效，-2=时间戳重复或倒退
 * @note 不含内部锁，调用方须串行访问。
 */
int ocr_pose_history_push(ocr_pose_history_t *history,
                          int64_t timestamp_ns,
                          const quaternion_t *q);

/**
 * @brief 查询目标时间的姿态；区间内做 SLERP，区间外钳位到最近样本
 * @param[in] history 含至少一项的历史
 * @param[in] timestamp_ns 正的查询时间，单位纳秒，须与样本使用同一时钟域
 * @param[out] q 调用者提供的四元数，成功时写入单位姿态四元数
 * @return 0=成功，-1=参数无效、历史为空或插值失败，-2=历史时间区间无效
 * @note 钳位返回成功且不做外推；调用者如需拒绝过旧姿态，应另外检查时间覆盖范围。
 */
int ocr_pose_history_sample(const ocr_pose_history_t *history,
                            int64_t timestamp_ns,
                            quaternion_t *q);

#endif /* OCR_STAB_POSE_HISTORY_H */
