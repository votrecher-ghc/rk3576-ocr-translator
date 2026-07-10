#ifndef OCR_AI_MODEL_LOADER_H
#define OCR_AI_MODEL_LOADER_H
/**
 * @file model_loader.h
 * @brief 模型加载/版本管理
 *
 * 统一管理多个 RKNN 模型文件的加载、版本检查、热更新。
 */

#include "rknn_runtime.h"

#define MODEL_MAX 8

/** 模型类型 */
typedef enum {
    MODEL_DET = 0,
    MODEL_REC,
    MODEL_TRANSLATE_ENC,
    MODEL_TRANSLATE_DEC,
} ocr_model_type_t;

/** 模型条目 */
typedef struct {
    ocr_model_type_t type;       /* 模型类型 */
    char             path[256];  /* 模型文件路径 */
    char             version[32];/* 版本号 */
    uint64_t         file_size;  /* 文件大小 */
    uint64_t         load_time;  /* 加载时间戳 */
    int              loaded;     /* 是否已加载 */
} ocr_model_entry_t;

/** 模型加载器 */
typedef struct {
    ocr_model_entry_t entries[MODEL_MAX];
    int               count;
    char              model_dir[256]; /* 模型根目录 */
} ocr_model_loader_t;

/**
 * @brief 初始化模型加载器
 * @param[in] loader    加载器
 * @param[in] model_dir 模型根目录
 */
int model_loader_init(ocr_model_loader_t *loader, const char *model_dir);

/**
 * @brief 注册一个模型文件
 * @param[in] loader 加载器
 * @param[in] type   模型类型
 * @param[in] path   模型路径
 * @param[in] version 版本号
 */
int model_loader_register(ocr_model_loader_t *loader, ocr_model_type_t type,
                          const char *path, const char *version);

/**
 * @brief 查询已注册模型信息
 */
const ocr_model_entry_t *model_loader_query(ocr_model_loader_t *loader,
                                            ocr_model_type_t type);

/**
 * @brief 检查模型文件是否有更新（通过文件大小/mtime）
 * @return 1=有更新，0=无更新，负数=错误
 */
int model_loader_check_update(ocr_model_loader_t *loader, ocr_model_type_t type);

/**
 * @brief 销毁加载器
 */
void model_loader_destroy(ocr_model_loader_t *loader);

#endif /* OCR_AI_MODEL_LOADER_H */
