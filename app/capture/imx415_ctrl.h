#ifndef OCR_CAPTURE_IMX415_CTRL_H
#define OCR_CAPTURE_IMX415_CTRL_H
/**
 * @file imx415_ctrl.h
 * @brief IMX415 摄像头控制：曝光/增益/分辨率切换
 *
 * 通过 V4L2 扩展控件（VIDIOC_S_EXT_CTRLS）控制 IMX415 传感器参数。
 */

#include <stdint.h>

/** IMX415 控制上下文 */
typedef struct {
    int      v4l2_fd;     /* V4L2 设备 fd */
    int32_t  exposure;    /* 当前曝光值（单位取决于传感器） */
    int32_t  gain;        /* 当前增益（dB*100） */
    int32_t  max_exposure;/* 曝光上限 */
    int32_t  max_gain;    /* 增益上限 */
} ocr_imx415_t;

/**
 * @brief 初始化 IMX415 控制
 * @param[in] imx     控制上下文
 * @param[in] v4l2_fd V4L2 设备 fd
 * @return 0=成功，负数=错误
 */
int ocr_imx415_init(ocr_imx415_t *imx, int v4l2_fd);

/**
 * @brief 设置曝光时间
 * @param[in] exposure 曝光值
 */
int ocr_imx415_set_exposure(ocr_imx415_t *imx, int32_t exposure);

/**
 * @brief 设置增益
 * @param[in] gain 增益（dB*100）
 */
int ocr_imx415_set_gain(ocr_imx415_t *imx, int32_t gain);

/**
 * @brief 自动曝光/增益（AE）开关
 * @param[in] enable 1=开启自动，0=手动
 */
int ocr_imx415_set_ae(ocr_imx415_t *imx, int enable);

/**
 * @brief 切换分辨率（需重新协商格式）
 */
int ocr_imx415_set_resolution(ocr_imx415_t *imx, uint32_t width, uint32_t height);

#endif /* OCR_CAPTURE_IMX415_CTRL_H */
