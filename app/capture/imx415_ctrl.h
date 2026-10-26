#ifndef OCR_CAPTURE_IMX415_CTRL_H
#define OCR_CAPTURE_IMX415_CTRL_H
/**
 * @file imx415_ctrl.h
 * @brief IMX415 摄像头控制：曝光/增益/分辨率切换
 *
 * 通过 V4L2 扩展控件（VIDIOC_S_EXT_CTRLS）控制 IMX415 传感器参数。
 *
 * 通过摄像头 V4L2 control 接口设置曝光、增益和自动曝光。
 * 这些控制只调整已打开设备的参数，不分配图像缓冲，也不负责启动采集流。
 */

#include <stdint.h>

/** IMX415 控制上下文 */
typedef struct {
    int      v4l2_fd; /**< 借用采集器打开的 video fd；此控制器不 close，也不独立持有图像缓冲。 */
    int32_t  exposure; /**< 最近一次成功 S_CTRL 写入的曝光控件值；不是每帧测量或自动曝光实时结果。 */
    int32_t  gain; /**< 最近一次成功设置的 V4L2_CID_GAIN 原始控件值；代码不做 dB 单位换算。 */
    int32_t  max_exposure; /**< QUERYCTRL 返回的曝光最大控件值；查询失败回退 100000，用于上限裁剪。 */
    int32_t  max_gain; /**< QUERYCTRL 返回的增益最大控件值；查询失败回退 4800，不保证不同驱动同单位。 */
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
