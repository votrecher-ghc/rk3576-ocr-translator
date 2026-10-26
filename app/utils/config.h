#ifndef OCR_UTILS_CONFIG_H
#define OCR_UTILS_CONFIG_H
/**
 * @file config.h
 * @brief JSON 配置解析（基于 cJSON）
 *
 * 配置加载先填默认值，再按 JSON 字段覆盖并校验范围、路径及模式相关要求。
 * 结构体内固定数组保存字符串，不借用 cJSON 内存；pipeline.json 仅为设计参考，运行时读取统一配置。
 */

#include <stdint.h>

/** 配置根结构体 */
typedef struct {
    char     app_name[64]; /**< 日志使用的应用名称，含 NUL，不是可执行文件路径。 */
    int      log_level; /**< 最小日志等级，0 DEBUG、1 INFO、2 WARN、3 ERROR、4 FATAL。 */
    char     log_path[256]; /**< 日志文件路径，含 NUL；空字符串表示使用 stderr。 */

    /* 采集配置 */
    char     v4l2_device[64];     /* V4L2 设备节点 */
    uint32_t capture_width; /**< 请求采集宽度，像素；驱动 S_FMT 可能调整，实际值保存在采集器。 */
    uint32_t capture_height; /**< 请求采集高度，像素；NV12 要求偶数。 */
    uint32_t capture_fps; /**< 请求帧率，帧/秒；不表示应用实测处理吞吐率。 */
    char     capture_format[16]; /**< 请求采集格式名称，当前允许 NV12/NV16/YUYV；运行时转换为像素枚举及 FourCC。 */
    int      capture_buffer_count; /**< 请求 V4L2 缓冲数，2..8；实际数以 REQBUFS 返回为准。 */

    /* 显示配置 */
    char     drm_device[64];      /* DRM 设备节点 */
    uint32_t display_width; /**< 期望显示模式宽度，像素；最终输出池使用 DRM 协商所得尺寸。 */
    uint32_t display_height; /**< 期望显示模式高度，像素；主流程 NV12 输出要求偶数。 */

    /* AI 模型路径 */
    char     det_model[256];      /* 文字检测模型路径 */
    char     rec_model[256];      /* 文字识别模型路径 */
    char     trans_model[256]; /**< 兼容旧配置的单翻译模型路径；当前主流程实际使用独立 encoder/decoder 路径。 */
    char     tokenizer_vocab[256]; /**< 兼容旧 ai.vocab 字段的词表路径；当前识别器实际读取 rec_vocab。 */
    char     rec_vocab[256];      /* OCR 识别词表路径 */
    char     trans_encoder_model[256]; /* 翻译编码器 RKNN 模型 */
    char     trans_decoder_model[256]; /* 翻译解码器 RKNN 模型 */
    char     trans_manifest[256];      /* 翻译接口与语言对 manifest */
    char     trans_src_vocab[256];     /* 翻译源语言词表 */
    char     trans_tgt_vocab[256];     /* 翻译目标语言词表 */
    float    det_threshold; /**< 检测概率图逐像素二值化阈值，0..1。 */
    float    det_box_threshold; /**< 文本连通域平均概率的保留阈值，0..1，与像素阈值区分。 */
    int      allow_ocr_only; /**< 翻译资产/契约不可用时允许仅 OCR 的开关；0 表示必须具备翻译能力。 */

    /* 翻译配置 */
    char     src_lang[16];        /* 源语言 */
    char     tgt_lang[16];        /* 目标语言 */

    /* 字体 */
    char     font_path[256];      /* FreeType 字体路径 */
    int      font_size; /**< 文字渲染像素字号，1..512；实际传给 FreeType pixel size。 */

    /* 存档 */
    char     archive_dir[256];    /* 归档根目录 */
    int      archive_max_percent; /**< 归档所在文件系统占用率触发阈值，百分比；清理目标通常再低 10 个百分点。 */
    int      jpeg_quality; /**< 拍照 JPEG 编码质量，1..100；数值越高通常文件越大，非压缩比。 */

    /* IMU */
    char     imu_device[64];      /* IIO 设备节点或 auto */
    int      imu_sample_hz; /**< 姿态融合的名义采样率，Hz，用于 sample_dt；此字段本身不修改芯片硬件 ODR。 */
    float    stab_alpha; /**< 虚拟摄像机姿态低通跟随系数，0..1；与 Madgwick beta 不同。 */
    float    madgwick_beta; /**< Madgwick 重力修正增益，配置允许 0..10，传给姿态融合初始化。 */
    float    stab_crop_ratio; /**< 线性保留比例，(0.5,1]；main 取倒数作为固定放大倍数，不是保留面积比例。 */

    /* 传感器、输入、背光/风扇 */
    char     light_iio_device[128]; /* 环境光 IIO sysfs 路径或 auto */
    char     temp_iio_device[128];  /* 温度 IIO sysfs 路径或 auto */
    char     key_device[64];        /* KEY_CAMERA input 节点或 auto */
    char     backlight_path[128]; /**< 背光 sysfs 设备目录或其 /brightness 属性路径，控制器初始化时规范化。 */
    char     pwm_path[128]; /**< kernel 表示内核温控；auto/显式 sysfs 路径表示用户态 PWM 接管，两条路径互斥。 */
} ocr_config_t;

/**
 * @brief 从 JSON 文件加载配置
 * @param[out] cfg  配置结构体
 * @param[in]  path JSON 文件路径
 * @return 0=成功，负数=错误
 */
int config_load(ocr_config_t *cfg, const char *path);

/**
 * @brief 释放配置资源
 * @param[in] cfg 配置结构体
 */
void config_free(ocr_config_t *cfg);

/**
 * @brief 打印配置内容（调试用）
 */
void config_dump(const ocr_config_t *cfg);

#endif /* OCR_UTILS_CONFIG_H */
