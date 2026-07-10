#ifndef OCR_AI_RKNN_RUNTIME_H
#define OCR_AI_RKNN_RUNTIME_H
/**
 * @file rknn_runtime.h
 * @brief RKNN 运行时封装：rknn_init/inputs_set/run/outputs_get
 *
 * 封装 Rockchip NPU RKNN 运行时，提供模型加载、输入设置、推理、输出获取接口。
 */

#include <stdint.h>
#include "buffer.h"

/* 前置声明 rknn 类型（避免直接包含 rknn_api.h） */
typedef struct rknn_context rknn_context_wrap;

#define RKNN_MAX_IO 16

/** RKNN 输入/输出张量描述 */
typedef struct {
    uint32_t  n_attrs;            /* 属性个数 */
    uint32_t  dims[8];            /* 维度 */
    uint32_t  n_dims;             /* 维度数 */
    uint32_t  type;               /* 数据类型 */
    uint32_t  size;               /* 数据字节大小 */
    uint32_t  fmt;                /* 格式 */
    char      name[64];           /* 张量名 */
} ocr_rknn_tensor_t;

/** RKNN 运行时上下文 */
typedef struct {
    void    *rknn_ctx;            /* rknn_context（不透明） */
    int      input_num;           /* 输入张量个数 */
    int      output_num;          /* 输出张量个数 */
    ocr_rknn_tensor_t inputs[RKNN_MAX_IO];   /* 输入描述 */
    ocr_rknn_tensor_t outputs[RKNN_MAX_IO];  /* 输出描述 */
    void    *input_bufs[RKNN_MAX_IO];        /* 输入缓冲指针 */
    void    *output_bufs[RKNN_MAX_IO];       /* 输出缓冲指针 */
    int      initialized;         /* 是否已初始化 */
} ocr_rknn_t;

/**
 * @brief 加载 RKNN 模型
 * @param[in] rknn      运行时上下文
 * @param[in] model_path 模型文件路径（.rknn）
 * @return 0=成功，负数=错误
 */
int ocr_rknn_load(ocr_rknn_t *rknn, const char *model_path);

/**
 * @brief 设置第 idx 个输入（直接绑定内存指针）
 * @param[in] rknn  运行时
 * @param[in] idx   输入索引
 * @param[in] data  输入数据指针
 * @param[in] size  数据字节大小
 */
int ocr_rknn_set_input(ocr_rknn_t *rknn, int idx, const void *data, uint32_t size);

/**
 * @brief 从 DMA-BUF fd 设置输入（零拷贝，NPU 直接访问）
 * @param[in] rknn  运行时
 * @param[in] idx   输入索引
 * @param[in] fd    DMA-BUF fd
 * @param[in] size  数据字节大小
 */
int ocr_rknn_set_input_dmabuf(ocr_rknn_t *rknn, int idx, int fd, uint32_t size);

/**
 * @brief 执行推理
 */
int ocr_rknn_run(ocr_rknn_t *rknn);

/**
 * @brief 获取第 idx 个输出
 * @param[out] data 输出数据指针（指向内部缓冲，无需释放）
 * @param[out] size 输出数据大小
 */
int ocr_rknn_get_output(ocr_rknn_t *rknn, int idx, void **data, uint32_t *size);

/**
 * @brief 销毁运行时，释放资源
 */
void ocr_rknn_destroy(ocr_rknn_t *rknn);

#endif /* OCR_AI_RKNN_RUNTIME_H */
