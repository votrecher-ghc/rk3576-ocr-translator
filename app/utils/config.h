#ifndef OCR_UTILS_CONFIG_H
#define OCR_UTILS_CONFIG_H
/**
 * @file config.h
 * @brief JSON 配置解析（基于 cJSON）
 */

#include <stdint.h>

/** 配置根结构体 */
typedef struct {
    char     app_name[64];        /* 应用名 */
    int      run_mode;            /* 运行模式: 0=实时翻译, 1=拍照翻译 */
    int      log_level;           /* 日志级别 */
    char     log_path[256];       /* 日志文件路径 */

    /* 采集配置 */
    char     v4l2_device[64];     /* V4L2 设备节点 */
    uint32_t capture_width;       /* 采集宽度 */
    uint32_t capture_height;      /* 采集高度 */
    uint32_t capture_fps;         /* 采集帧率 */

    /* 显示配置 */
    char     drm_device[64];      /* DRM 设备节点 */
    uint32_t display_width;       /* 显示宽度 */
    uint32_t display_height;      /* 显示高度 */

    /* AI 模型路径 */
    char     det_model[256];      /* 文字检测模型路径 */
    char     rec_model[256];      /* 文字识别模型路径 */
    char     trans_model[256];    /* 翻译模型路径 */
    char     tokenizer_vocab[256];/* 分词词表路径 */

    /* 翻译配置 */
    char     src_lang[16];        /* 源语言 */
    char     tgt_lang[16];        /* 目标语言 */

    /* 字体 */
    char     font_path[256];      /* FreeType 字体路径 */
    int      font_size;           /* 字号 */

    /* 存档 */
    char     archive_dir[256];    /* 归档根目录 */
    int      archive_max_percent; /* 容量告警阈值（百分比） */

    /* IMU */
    char     imu_device[64];      /* IIO 设备节点 */
    int      imu_sample_hz;       /* 采样率 */
    float    stab_alpha;          /* 防抖低通系数 */

    /* 背光/风扇 */
    char     backlight_path[128]; /* 背光 sysfs 路径 */
    char     pwm_path[128];       /* PWM 风扇 sysfs 路径 */
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
