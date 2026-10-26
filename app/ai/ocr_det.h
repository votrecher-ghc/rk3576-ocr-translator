#ifndef OCR_AI_OCR_DET_H
#define OCR_AI_OCR_DET_H

/** @file ocr_det.h @brief OCR text detection.
 *
 * 文字检测流程：CPU 缩放图像并准备张量 -> RKNN 推理 -> 概率图连通域后处理。
 * 模型加载时检查输入/输出形状，预分配工作区；同一个上下文不应被多个推理线程并发使用。
 */

#include "rknn_runtime.h"
#include "ocr_postproc.h"
#include "buffer.h"

#define OCR_DET_INPUT_W 960
#define OCR_DET_INPUT_H 960

typedef struct {
    ocr_rknn_t rknn; /**< 本检测器拥有的 RKNN 模型上下文，destroy 时销毁；同一实例串行推理。 */
    float     *pre_buf; /**< 本检测器分配/释放的 FLOAT32 CHW 工作区，元素数为 input_w*input_h*3。 */
    uint8_t   *resize_buf; /**< CPU 缩放后的 UINT8 RGB HWC 工作区，字节数为 input_w*input_h*3。 */
    uint8_t   *post_binary; /**< 输出概率图的二值工作区，每像素 1 字节；搜索连通域时原位清除访问标记。 */
    size_t    *post_queue; /**< 连通域洪泛搜索队列，每项是概率图像素的线性索引，容量以元素数计。 */
    size_t     post_capacity; /**< post_binary/post_queue 可处理的像素数，初始化取 output_w*output_h。 */
    int        input_w; /**< 模型输入有效宽度，像素；初始化查询张量属性得到。 */
    int        input_h; /**< 模型输入有效高度，像素；CPU 将源图缩放到此尺寸。 */
    int        output_w; /**< 模型输出概率图宽度，像素；可能不同于输入宽度。 */
    int        output_h; /**< 模型输出概率图高度，像素；后处理坐标先处于此图中。 */
    float      thresh; /**< 像素概率二值化阈值，0..1；决定哪些像素进入连通域。 */
    float      box_thresh; /**< 连通域平均概率的保留阈值，0..1；与像素阈值不同。 */
    float      unclip_ratio; /**< 框外扩的无量纲系数；距离按面积*系数/周长计算，不是宽高直接乘倍数。 */
} ocr_det_t;

/**
 * @brief 加载模型并预分配图像预处理及连通域工作区
 * @param[out] det 待初始化的检测器；已有实例须先销毁
 * @param[in] model_path 单输入、单概率图输出的 RKNN 模型文件
 * @return 0=成功，负数=模型加载、张量契约或资源分配失败
 * @note 实际尺寸来自模型属性；OCR_DET_INPUT_W/H 不覆盖模型中的尺寸。
 */
int ocr_det_init(ocr_det_t *det, const char *model_path);

/**
 * @brief 检测一帧，输出原图坐标中按近似阅读顺序排列的文本框
 * @param[in,out] det 已初始化且独占使用的检测器
 * @param[in] buf 已映射的源图，调用方持有引用并完成 CPU 读访问同步
 * @param[out] boxes 调用者提供的结果结构，坐标单位为 buf 中的像素
 * @return 0=成功（count 可为 0），负数=预处理、推理或后处理失败
 * @note 不接管 buf 或 boxes；失败时不能使用 boxes 作为本帧检测结果。
 */
int ocr_det_run(ocr_det_t *det, const ocr_buffer_t *buf,
                ocr_text_box_list_t *boxes);

/** @brief 推理结束后释放模型和全部工作区；不释放输入图像或检测结果结构。 */
void ocr_det_destroy(ocr_det_t *det);

#endif /* OCR_AI_OCR_DET_H */
