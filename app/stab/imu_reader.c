/**
 * @file imu_reader.c
 * @brief IMU 读取实现（IIO triggered buffer）
 */
#include "imu_reader.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int ocr_imu_reader_init(ocr_imu_reader_t *reader, const char *dev_path, int buf_depth)
{
    if (!reader || !dev_path) return -1;
    memset(reader, 0, sizeof(*reader));

    reader->dev_fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (reader->dev_fd < 0) {
        LOG_E("打开 IIO 设备 %s 失败: %s", dev_path, strerror(errno));
        return -2;
    }
    strncpy(reader->dev_name, dev_path, sizeof(reader->dev_name) - 1);

    if (buf_depth <= 0) buf_depth = 256;
    if (ocr_ringbuffer_init(&reader->ring, sizeof(imu_sample_t), buf_depth) != 0) {
        close(reader->dev_fd);
        return -3;
    }

    /* TODO: 读取 IIO sysfs 获取 scale 参数
     * /sys/bus/iio/devices/iio:device0/in_accel_scale
     * /sys/bus/iio/devices/iio:device0/in_anglvel_scale */
    reader->accel_scale = 1;
    reader->gyro_scale = 1;

    atomic_init(&reader->running, 0);
    LOG_I("IMU 读取初始化: %s buf_depth=%d", dev_path, buf_depth);
    return 0;
}

/* IMU 读取线程入口 */
static void *imu_thread(void *arg)
{
    ocr_imu_reader_t *reader = (ocr_imu_reader_t *)arg;
    LOG_I("IMU 读取线程启动");

    /* IIO triggered buffer 的数据布局：
     * 每个 sample 包含 accel[3] + gyro[3] + timestamp，共 7*2 + 8 = 22 字节
     * 实际布局取决于通道使能情况，需根据 sysfs 扫描元素确定 */
    const int sample_bytes = sizeof(imu_sample_t);
    uint8_t *buf = (uint8_t *)malloc(sample_bytes);
    if (!buf) {
        LOG_E("IMU 读取缓冲分配失败");
        return NULL;
    }

    while (atomic_load(&reader->running)) {
        ssize_t n = read(reader->dev_fd, buf, sample_bytes);
        if (n < 0) {
            if (errno == EAGAIN) {
                /* 无数据，短暂等待 */
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
                nanosleep(&ts, NULL);
                continue;
            }
            LOG_W("IMU read 失败: %s", strerror(errno));
            continue;
        }
        if (n == sample_bytes) {
            imu_sample_t sample;
            memcpy(&sample, buf, sizeof(sample));
            /* TODO: 根据实际 IIO 扫描元素布局解析通道数据 */
            ocr_ringbuffer_push(&reader->ring, &sample);
            reader->sample_count++;
        }
    }

    free(buf);
    LOG_I("IMU 读取线程退出 (samples=%llu)", (unsigned long long)reader->sample_count);
    return NULL;
}

int ocr_imu_reader_start(ocr_imu_reader_t *reader)
{
    if (!reader || reader->dev_fd < 0) return -1;
    atomic_store(&reader->running, 1);
    if (ocr_thread_create(&reader->thread, imu_thread, reader) != 0) {
        LOG_E("IMU 读取线程创建失败");
        return -2;
    }
    return 0;
}

int ocr_imu_reader_stop(ocr_imu_reader_t *reader)
{
    if (!reader) return -1;
    atomic_store(&reader->running, 0);
    ocr_thread_join(reader->thread, NULL);
    return 0;
}

int ocr_imu_reader_get(ocr_imu_reader_t *reader, imu_sample_t *sample)
{
    if (!reader || !sample) return -1;
    return ocr_ringbuffer_pop(&reader->ring, sample);
}

void ocr_imu_reader_destroy(ocr_imu_reader_t *reader)
{
    if (!reader) return;
    ocr_imu_reader_stop(reader);
    ocr_ringbuffer_destroy(&reader->ring);
    if (reader->dev_fd >= 0) {
        close(reader->dev_fd);
        reader->dev_fd = -1;
    }
}
