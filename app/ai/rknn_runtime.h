#ifndef OCR_AI_RKNN_RUNTIME_H
#define OCR_AI_RKNN_RUNTIME_H

/**
 * @file rknn_runtime.h
 * @brief Small, ownership-safe wrapper around the RKNN runtime API.
 *
 * RKNN 运行时封装：加载模型和张量属性，设置普通内存输入，运行推理并管理输出生命周期。
 * 输出指针由上下文持有，下次推理或销毁前有效；DMA-BUF 导入因缺少原生张量布局约定而返回不支持。
 * 同一实例的 set_input/run/get_output/destroy 须由调用者串行化，不含内部互斥锁。
 */

#include <stdint.h>

#include "rknn_api.h"

#define RKNN_MAX_IO 16
#define OCR_RKNN_MAX_DIMS 16

/** Tensor metadata copied from rknn_tensor_attr. */
typedef struct {
    uint32_t index; /**< 当前模型的输入或输出索引，0 起；仅在对应 inputs/outputs 数组内有意义。 */
    uint32_t dims[OCR_RKNN_MAX_DIMS]; /**< 张量各维度大小，前 n_dims 项有效；维度顺序结合 fmt 解读。 */
    uint32_t n_dims; /**< 有效维度数，不超过 OCR_RKNN_MAX_DIMS；不是元素数量。 */
    uint32_t n_elems; /**< 逻辑元素总数，通常为各 dims 的乘积；输入字节数还须乘调用者源类型大小。 */
    uint32_t type; /**< 模型侧 rknn_tensor_type 枚举，如 INT8/FLOAT16；普通接口源类型可以不同。 */
    uint32_t size; /**< SDK 查询的模型张量大小，字节；不等于 want_float=1 后返回的浮点输出大小。 */
    uint32_t fmt; /**< rknn_tensor_format 枚举，如 NCHW/NHWC；决定通道及高宽的排列。 */
    uint32_t qnt_type; /**< SDK 报告的量化方式，决定 fractional_length 或 zero_point/scale 的解释。 */
    int8_t   fractional_length; /**< 定点量化的小数位参数，复制自 SDK fl；仅对应量化方式下有效。 */
    int32_t  zero_point; /**< 仿射量化零点 zp；对应类型可按 real=(quantized-zp)*scale 解读。 */
    float    scale; /**< 仿射量化比例；当前输出请求浮点，业务后处理不再手动反量化一次。 */
    uint32_t width_stride; /**< SDK w_stride 原值，描述张量宽方向存储跨度；当前仅保存，不作图像字节 stride。 */
    uint32_t height_stride; /**< SDK h_stride 原值，描述张量高方向存储跨度；未用于建立原生 DMA-BUF 输入。 */
    uint32_t size_with_stride; /**< 包含 SDK 布局填充的存储大小，字节；与逻辑 n_elems 及普通源张量大小区分。 */
    char     name[64]; /**< SDK 节点名称的本地副本，最多 63 字节加 NUL；不借用 SDK 字符串内存。 */
} ocr_rknn_tensor_t;

/** RKNN model/runtime context. Output pointers are owned by RKNN. */
typedef struct {
    rknn_context      rknn_ctx; /**< 本实例拥有的运行时上下文句柄，load 创建、destroy 释放。 */
    int               input_num; /**< 模型输入数量，1..RKNN_MAX_IO；每轮 run 前都须设置全部输入。 */
    int               output_num; /**< 模型输出数量，1..RKNN_MAX_IO；决定获取与归还输出的项数。 */
    ocr_rknn_tensor_t inputs[RKNN_MAX_IO]; /**< 输入属性副本，前 input_num 项有效；初始化后供预处理和长度校验读取。 */
    ocr_rknn_tensor_t outputs[RKNN_MAX_IO]; /**< 输出属性副本，前 output_num 项有效；原始 type/size 与浮点返回视图区分。 */
    const void       *input_bufs[RKNN_MAX_IO]; /**< 借用的本轮输入指针及已设置标记；不 free，run 后清零，上层保证推理期间有效。 */
    void             *output_bufs[RKNN_MAX_IO]; /**< 由 RKNN 持有的浮点结果指针；只能 outputs_release，下一次 run/destroy 后失效。 */
    uint32_t          output_sizes[RKNN_MAX_IO]; /**< 本轮各输出实际返回的字节数；want_float=1 时按 float 数组校验。 */
    int               outputs_acquired; /**< 本实例当前持有待归还输出时为 1；防止重复 release 或借出失效指针。 */
    int               initialized; /**< 上下文与全部张量属性查询成功标志；为 0 时不能提交推理。 */
} ocr_rknn_t;

/** Load a model and query all input/output attributes.
 *
 * 读取模型、创建上下文并查询输入输出属性；任何步骤失败都撤销已分配资源。
 */
int ocr_rknn_load(ocr_rknn_t *rknn, const char *model_path);

/**
 * Set an input whose memory already uses the model tensor's type and format.
 * The caller retains ownership and must keep data alive through ocr_rknn_run().
 */
int ocr_rknn_set_input(ocr_rknn_t *rknn, int idx,
                       const void *data, uint32_t size);

/**
 * Set an input while explicitly describing the caller-side type and layout.
 * RKNN performs supported type/layout conversion because pass_through is zero.
 *
 * 校验输入元素大小与数量后提交普通内存张量，允许运行时按模型要求转换。
 * @param[in,out] rknn 已加载的模型上下文
 * @param[in] idx 输入索引，范围为 0..input_num-1
 * @param[in] data 调用者持有的连续张量，须保持有效至同步 run 返回
 * @param[in] size 源张量字节数，须等于 inputs[idx].n_elems 乘源 type 的元素大小
 * @param[in] type 源内存元素类型，不一定与模型内部类型相同
 * @param[in] fmt 源内存布局，按当前模型和 SDK 支持的转换约定填写
 * @return 0=已设置，-EINVAL=参数无效，-ENOTSUP=源类型不支持，
 *         -EMSGSIZE=大小不符，-EIO=SDK 提交失败
 */
int ocr_rknn_set_input_ex(ocr_rknn_t *rknn, int idx,
                          const void *data, uint32_t size,
                          rknn_tensor_type type, rknn_tensor_format fmt);

/** DMA-BUF input is unsupported until an RKNN memory-object contract is chosen.
 *
 * 占位接口：尚未定义模型原生布局及 stride 约定，明确返回 -ENOTSUP。
 */
int ocr_rknn_set_input_dmabuf(ocr_rknn_t *rknn, int idx,
                              int fd, uint32_t size);

/** Run inference and acquire float32 outputs owned by RKNN.
 *
 * 释放上次输出、执行推理并获取本次输出；当前输出统一请求浮点格式。
 * @return 0=成功，-EINVAL=上下文无效或本轮输入未全部设置，-EIO=推理或取输出失败
 * @note 每轮须重新设置全部输入。新一轮开始释放旧输出后，即使推理失败，旧指针也已失效。
 */
int ocr_rknn_run(ocr_rknn_t *rknn);

/** Borrow an output pointer. It remains valid until the next run or destroy.
 *
 * 借用上下文持有的输出指针；调用者不能 free，下一次推理前应完成使用。
 * @param[in] rknn 已成功完成推理、仍持有输出的上下文
 * @param[in] idx 输出索引，范围为 0..output_num-1
 * @param[out] data 借用的 FLOAT32 数据指针，调用者不能修改其所有权
 * @param[out] size 可选的实际输出字节数，允许 NULL；不是 float 元素数
 * @return 0=成功，负数=参数无效或没有可借用的输出
 */
int ocr_rknn_get_output(ocr_rknn_t *rknn, int idx,
                        void **data, uint32_t *size);

/** Release outstanding outputs and destroy the RKNN context. */
void ocr_rknn_destroy(ocr_rknn_t *rknn);

#endif /* OCR_AI_RKNN_RUNTIME_H */
