#ifndef OCR_AI_OCR_POSTPROC_H
#define OCR_AI_OCR_POSTPROC_H

/** @file ocr_postproc.h @brief DB probability-map post-processing.
 *
 * 检测后处理：阈值二值化 -> 搜索连通域 -> 按平均概率筛选并扩张边框 -> 阅读顺序排序。
 * 工作区版本复用调用方内存；便捷版本临时分配工作区。输出使用有容量上限的文本框数组。
 */

#include <stddef.h>
#include <stdint.h>

#define OCR_MAX_TEXT_BOXES 256
#define OCR_BOX_POINTS 4

typedef struct {
    float x[OCR_BOX_POINTS]; /**< 四个顶点的横坐标，像素；与 y 同索引配对，后处理在概率图中、det_run 后在源图中。 */
    float y[OCR_BOX_POINTS]; /**< 四个顶点的纵坐标，像素；顶点按边界周向排列，识别透视采样依赖该顺序。 */
    float score; /**< 文本连通域的平均概率，通常 0..1；用于筛选和容量满时保留高分框。 */
    int   valid; /**< 非零表示可用文本框，0 表示无效；透视预处理会拒绝无效框。 */
} ocr_text_box_t;

typedef struct {
    ocr_text_box_t boxes[OCR_MAX_TEXT_BOXES]; /**< 内嵌的定容量文本框数组，不需单独分配；仅前 count 项有效。 */
    int count; /**< 有效框数，0..OCR_MAX_TEXT_BOXES；排序后表示近似阅读顺序。 */
} ocr_text_box_list_t;

/**
 * @brief 对概率图提取八连通域，拟合及扩张四点框，再按阅读顺序排序
 * @param[in] prob_map 连续 width*height 个 float，按行存储；非有限像素不进入连通域
 * @param[in] width 概率图宽度，必须大于 0
 * @param[in] height 概率图高度，必须大于 0
 * @param[in] thresh 像素二值化阈值，范围 [0,1]
 * @param[in] box_thresh 连通域平均概率阈值，范围 [0,1]
 * @param[in] unclip_ratio 非负外扩系数，扩张距离按框面积*系数/周长计算
 * @param[out] out 调用者提供的结果结构，坐标位于概率图中，未映射到源图
 * @return 0=成功（可无文本框），负数=参数、容量或资源分配错误
 * @note 临时分配工作区；结果超过 OCR_MAX_TEXT_BOXES 时保留高分框，仍返回 0。
 *       实时逐帧处理优先使用工作区版本；失败时不应使用 out 作为本次结果。
 */
int ocr_db_postprocess(const float *prob_map, int width, int height,
                       float thresh, float box_thresh, float unclip_ratio,
                       ocr_text_box_list_t *out);

/**
 * @brief 使用调用者独占的工作区完成后处理，不在调用期间分配堆内存
 * @param[in] prob_map 输入概率图，尺寸和阈值约束同 ocr_db_postprocess
 * @param[out] binary 至少 workspace_pixels 字节的临时区，调用期间覆盖其内容
 * @param[out] queue 至少 workspace_pixels 个 size_t 元素的临时区
 * @param[in] workspace_pixels 两个临时区可容纳的像素数，至少为 width*height
 * @param[out] out 调用者提供的结果结构，坐标位于概率图中
 * @return 0=成功，-ENOSPC=工作区缺失或不足，其他负数=参数或尺寸错误
 * @note 输入、输出和两个工作区须使用互不重叠的内存；不接管任何调用者资源。
 */
int ocr_db_postprocess_with_workspace(
    const float *prob_map, int width, int height,
    float thresh, float box_thresh, float unclip_ratio,
    uint8_t *binary, size_t *queue, size_t workspace_pixels,
    ocr_text_box_list_t *out);

/**
 * @brief 原位移除 valid=0 的项并更新 count，再按从上到下、从左到右的近似顺序排序
 * @return 0=成功，-EINVAL=参数无效或 count 超过容量
 * @note 行分组基于框中心和高度容差，不提供多栏或复杂版面分析。
 */
int ocr_sort_boxes(ocr_text_box_list_t *list);

#endif /* OCR_AI_OCR_POSTPROC_H */
