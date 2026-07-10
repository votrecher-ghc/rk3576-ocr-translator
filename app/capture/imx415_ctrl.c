/**
 * @file imx415_ctrl.c
 * @brief IMX415 摄像头控制实现
 */
#include "imx415_ctrl.h"
#include "log.h"

#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* 设置 V4L2 控件通用函数 */
static int set_ctrl(int fd, uint32_t id, int32_t value)
{
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = value;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_W("VIDIOC_S_CTRL id=0x%08x value=%d 失败: %s", id, value, strerror(errno));
        return -1;
    }
    return 0;
}

int ocr_imx415_init(ocr_imx415_t *imx, int v4l2_fd)
{
    if (!imx) return -1;
    memset(imx, 0, sizeof(*imx));
    imx->v4l2_fd = v4l2_fd;

    /* 查询控件范围 */
    struct v4l2_queryctrl qctrl;
    memset(&qctrl, 0, sizeof(qctrl));

    /* 曝光上限 */
    qctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
    if (ioctl(v4l2_fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
        imx->max_exposure = qctrl.maximum;
    } else {
        imx->max_exposure = 100000; /* 默认 */
    }

    /* 增益上限 */
    memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = V4L2_CID_GAIN;
    if (ioctl(v4l2_fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
        imx->max_gain = qctrl.maximum;
    } else {
        imx->max_gain = 4800; /* 48dB */
    }

    LOG_I("IMX415 初始化: max_exposure=%d max_gain=%d", imx->max_exposure, imx->max_gain);
    return 0;
}

int ocr_imx415_set_exposure(ocr_imx415_t *imx, int32_t exposure)
{
    if (!imx || imx->v4l2_fd < 0) return -1;
    if (exposure > imx->max_exposure) exposure = imx->max_exposure;
    if (exposure < 1) exposure = 1;
    int ret = set_ctrl(imx->v4l2_fd, V4L2_CID_EXPOSURE_ABSOLUTE, exposure);
    if (ret == 0) imx->exposure = exposure;
    return ret;
}

int ocr_imx415_set_gain(ocr_imx415_t *imx, int32_t gain)
{
    if (!imx || imx->v4l2_fd < 0) return -1;
    if (gain > imx->max_gain) gain = imx->max_gain;
    if (gain < 0) gain = 0;
    int ret = set_ctrl(imx->v4l2_fd, V4L2_CID_GAIN, gain);
    if (ret == 0) imx->gain = gain;
    return ret;
}

int ocr_imx415_set_ae(ocr_imx415_t *imx, int enable)
{
    if (!imx || imx->v4l2_fd < 0) return -1;
    return set_ctrl(imx->v4l2_fd, V4L2_CID_EXPOSURE_AUTO,
                    enable ? V4L2_EXPOSURE_APERTURE_PRIORITY : V4L2_EXPOSURE_MANUAL);
}

int ocr_imx415_set_resolution(ocr_imx415_t *imx, uint32_t width, uint32_t height)
{
    if (!imx || imx->v4l2_fd < 0) return -1;
    /* 分辨率切换需重新 S_FMT，通常由 v4l2_capture 模块处理 */
    /* TODO: 通过事件通知 v4l2_capture 重新协商格式 */
    LOG_I("IMX415 分辨率切换请求: %ux%u（需重启采集流）", width, height);
    (void)width; (void)height;
    return 0;
}
