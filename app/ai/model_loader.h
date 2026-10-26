#ifndef OCR_AI_MODEL_LOADER_H
#define OCR_AI_MODEL_LOADER_H

/** @file model_loader.h @brief Model registry and update tracking.
 *
 * 模型文件登记与更新检测：记录路径、大小和修改时间，供上层决定何时重新加载。
 * 本模块不创建 RKNN 上下文，查询结果属于 loader，销毁或修改登记后不应继续引用。
 */

#include <stdint.h>

#include "rknn_runtime.h"

#define MODEL_MAX 8

typedef enum {
    MODEL_DET = 0,
    MODEL_REC,
    MODEL_TRANSLATE_ENC,
    MODEL_TRANSLATE_DEC,
} ocr_model_type_t;

typedef struct {
    ocr_model_type_t type; /**< 模型用途枚举：检测、识别、翻译 encoder 或 decoder；同一用途只登记一次。 */
    char             path[256]; /**< 登记后的模型文件路径，含 NUL；相对输入路径已与 model_dir 拼接。 */
    char             version[32]; /**< 调用者提供的版本标签，含 NUL；用于记录，不参与自动更新判断。 */
    uint64_t         file_size; /**< 登记时模型文件大小，字节；与当前文件大小比较以检测更新。 */
    int64_t          file_mtime_sec; /**< 登记时文件 mtime 的秒部分，与纳秒部分共同判断文件是否变化。 */
    int64_t          file_mtime_nsec; /**< 登记时 mtime 的纳秒小数部分，范围通常为 0..999999999。 */
    uint64_t         load_time; /**< 登记时的单调时钟毫秒值；字段名虽为 load，登记并未创建 RKNN 上下文。 */
    int              loaded; /**< 预留加载状态，register 置 0；当前登记器不实际加载模型或将其置 1。 */
} ocr_model_entry_t;

typedef struct {
    ocr_model_entry_t entries[MODEL_MAX]; /**< 内嵌登记表，前 count 项有效；query 借出的是这些元素的地址。 */
    int               count; /**< 已登记模型数量，最多 MODEL_MAX；不代表已创建的 RKNN 上下文数。 */
    char              model_dir[256]; /**< 模型相对路径的拼接根目录，含 NUL；绝对路径输入不使用此前缀。 */
} ocr_model_loader_t;

int model_loader_init(ocr_model_loader_t *loader, const char *model_dir);

/** @brief 验证普通模型文件并记录元数据，后续更新检查比较同一登记项。 */
int model_loader_register(ocr_model_loader_t *loader, ocr_model_type_t type,
                          const char *path, const char *version);

const ocr_model_entry_t *model_loader_query(ocr_model_loader_t *loader,
                                            ocr_model_type_t type);

/** Return 1 for a size/mtime change, 0 for no change, or a negative error.
 *
 * 只报告文件大小或修改时间变化，不在这里替换正在运行的模型。
 */
int model_loader_check_update(ocr_model_loader_t *loader,
                              ocr_model_type_t type);

void model_loader_destroy(ocr_model_loader_t *loader);

#endif /* OCR_AI_MODEL_LOADER_H */
