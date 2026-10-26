#ifndef OCR_AI_OCR_REC_H
#define OCR_AI_OCR_REC_H

/** @file ocr_rec.h @brief OCR recognition and UTF-8 CTC decoding.
 *
 * 文字识别流程：按检测框四点采样 -> CPU 准备张量 -> RKNN 推理 -> CTC 解码。
 * 词表行号与输出类别必须匹配，初始化阶段确认 blank 类别，避免识别结果发生整体偏移。
 */

#include "rknn_runtime.h"
#include "ocr_postproc.h"
#include "buffer.h"

#define OCR_REC_INPUT_W 320
#define OCR_REC_INPUT_H 48
#define OCR_MAX_TEXT_LEN 256
#define OCR_REC_MAX_TOKEN_LEN 256

typedef struct {
    ocr_rknn_t rknn; /**< 本识别器拥有的 RKNN 上下文；逐框顺序复用，不能并发运行同一实例。 */
    float     *pre_buf; /**< 本实例分配的 FLOAT32 CHW 工作区，input_w*input_h*3 个元素，destroy 释放。 */
    uint8_t   *resize_buf; /**< 四点透视采样后的 UINT8 RGB HWC 工作区，input_w*input_h*3 字节。 */
    int        input_w; /**< 模型识别输入宽度，像素；由模型查询，不假设固定为 320。 */
    int        input_h; /**< 模型识别输入高度，像素；文本框被展开到此高度。 */
    int        vocab_size; /**< 模型每个 CTC 时间步的类别数 C，包含 blank，未必等于词表行数。 */
    char     (*vocab)[OCR_REC_MAX_TOKEN_LEN]; /**< 本实例拥有的定长 UTF-8 词项数组；load_vocab 分配，必要时追加空格，destroy 释放。 */
    int        vocab_len; /**< 当前 vocab 有效项数；可能包含初始化时补入的空格，不只是文件原始行数。 */
    int        blank_id; /**< CTC 空白类别编号；跳过该类但仍更新 previous，以区分被 blank 隔开的重复字符。 */
    int        class_to_vocab_offset; /**< 类别号转词项索引的偏移；隐式类别 0 为 blank 时是 -1，显式词表含 blank 时为 0。 */
    int        timesteps; /**< CTC 输出时间步数 T，由输出元素总数/C 得到，不是输入图像的帧数。 */
} ocr_rec_t;

/**
 * @brief 加载识别模型、UTF-8 词表及预处理工作区，校验 CTC 类别映射
 * @param[out] rec 待初始化的识别器；已有实例须先销毁
 * @param[in] model_path RKNN 识别模型文件
 * @param[in] vocab_path 每行一个词项的词表文件，顺序须与模型输出类别一致
 * @return 0=成功，负数=加载、模型契约或资源分配失败
 */
int ocr_rec_init(ocr_rec_t *rec, const char *model_path,
                 const char *vocab_path);

/**
 * @brief 对源图中的一个四点文本框做透视采样、推理和 CTC 解码
 * @param[in,out] rec 已初始化的识别器，同一实例须串行调用
 * @param[in] buf 已映射的源图，调用方持有引用并完成 CPU 读访问同步
 * @param[in] box 有效文本框，顶点为 buf 原图中的像素坐标
 * @param[out] text 调用者分配的 UTF-8 输出缓冲
 * @param[in] text_size 输出字节容量，包含结尾 NUL
 * @return 0=成功（可为空文本），负数=预处理、推理或解码失败
 * @note 输入图像仅借用；失败时输出可能含部分文本，仅返回 0 才表示完整结果。
 */
int ocr_rec_run(ocr_rec_t *rec, const ocr_buffer_t *buf,
                const ocr_text_box_t *box, char *text, int text_size);

/**
 * @brief 顺序识别多个文本框，复用同一模型上下文和工作区
 * @param[in,out] rec 已初始化且独占使用的识别器
 * @param[in] buf 已映射并完成 CPU 读访问同步的源图
 * @param[in] boxes 待识别文本框，count 范围为 0..OCR_MAX_TEXT_BOXES
 * @param[out] texts 至少 boxes->count 个非 NULL 指针，各指向至少 OCR_MAX_TEXT_LEN 字节
 * @return 0=全部处理成功，负数=首个失败项的错误
 * @note 不分配 texts 或其元素。无效框写空串；遇错立即返回，后续元素保持原状。
 */
int ocr_rec_run_batch(ocr_rec_t *rec, const ocr_buffer_t *buf,
                      const ocr_text_box_list_t *boxes, char **texts);

/** @brief 推理结束后释放模型、词表和工作区；不释放调用者的图像或文本缓冲。 */
void ocr_rec_destroy(ocr_rec_t *rec);

#endif /* OCR_AI_OCR_REC_H */
