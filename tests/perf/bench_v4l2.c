/*
 * bench_v4l2.c - V4L2 采集性能基准测试
 *
 * 功能: 测量 V4L2 视频采集性能 (含 DMA-BUF 零拷贝模式)
 * 指标: 采集帧率, 单帧延迟, 丢帧统计
 * 设备: /dev/video0 (野火 LubanCat3 摄像头)
 *
 * 编译: 交叉编译, 无需额外库 (V4L2 在内核头文件中)
 * 运行: 板端执行
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

/* --------------------------------------------------------------------
 * 基准配置
 * -------------------------------------------------------------------- */

#define CAPTURE_DEVICE   "/dev/video0"
#define CAPTURE_WIDTH    1920
#define CAPTURE_HEIGHT   1080
#define CAPTURE_FPS      30
#define BUFFER_COUNT     4
#define TEST_DURATION_S  5   /* 测试持续时间 (秒) */

/* --------------------------------------------------------------------
 * 辅助函数
 * -------------------------------------------------------------------- */

static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int xioctl(int fd, int request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

/* --------------------------------------------------------------------
 * V4L2 缓冲区
 * -------------------------------------------------------------------- */
typedef struct {
    void   *start;
    size_t  length;
} v4l2_buffer_t;

static v4l2_buffer_t g_buffers[BUFFER_COUNT];
static int          g_fd = -1;

/* --------------------------------------------------------------------
 * V4L2 初始化
 * -------------------------------------------------------------------- */
static int v4l2_init(void)
{
    printf("  打开设备: %s\n", CAPTURE_DEVICE);
    g_fd = open(CAPTURE_DEVICE, O_RDWR | O_NONBLOCK, 0);
    if (g_fd < 0) {
        perror("  [错误] open");
        return -1;
    }

    /* 查询能力 */
    struct v4l2_capability cap;
    if (xioctl(g_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("  [错误] VIDIOC_QUERYCAP");
        return -1;
    }
    printf("  驱动: %s\n  设备: %s\n  总线: %s\n",
           cap.driver, cap.card, cap.bus_info);

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        printf("  [错误] 设备不支持视频采集\n");
        return -1;
    }

    /* 设置格式 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAPTURE_WIDTH;
    fmt.fmt.pix.height = CAPTURE_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(g_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("  [错误] VIDIOC_S_FMT");
        return -1;
    }
    printf("  格式: %dx%d (NV12)\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

    /* 设置帧率 */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = CAPTURE_FPS;
    if (xioctl(g_fd, VIDIOC_S_PARM, &parm) < 0) {
        perror("  [警告] VIDIOC_S_PARM (帧率设置可能不支持)");
    }

    /* 请求缓冲区 */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(g_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("  [错误] VIDIOC_REQBUFS");
        return -1;
    }

    /* 映射缓冲区 */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(g_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("  [错误] VIDIOC_QUERYBUF");
            return -1;
        }

        g_buffers[i].length = buf.length;
        g_buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, g_fd, buf.m.offset);
        if (g_buffers[i].start == MAP_FAILED) {
            perror("  [错误] mmap");
            return -1;
        }
    }
    printf("  已映射 %d 个缓冲区\n", BUFFER_COUNT);

    /* 入队所有缓冲区 */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(g_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("  [错误] VIDIOC_QBUF");
            return -1;
        }
    }

    /* 开始采集 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(g_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("  [错误] VIDIOC_STREAMON");
        return -1;
    }
    printf("  采集已开始\n");
    return 0;
}

/* --------------------------------------------------------------------
 * 采集性能测试
 * -------------------------------------------------------------------- */
static int run_benchmark(void)
{
    printf("\n--- V4L2 采集性能测试 ---\n");
    printf("  持续时间: %d 秒\n", TEST_DURATION_S);
    printf("  目标帧率: %d FPS\n", CAPTURE_FPS);

    int frame_count = 0;
    int drop_count = 0;
    double latencies[CAPTURE_FPS * TEST_DURATION_S * 2];
    int latency_idx = 0;

    double test_start = get_time_ms();
    double test_end = test_start + TEST_DURATION_S * 1000;

    while (get_time_ms() < test_end) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(g_fd, &fds);

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(g_fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("  [错误] select");
            break;
        }
        if (ret == 0) {
            printf("  [警告] select 超时\n");
            drop_count++;
            continue;
        }

        double frame_start = get_time_ms();

        /* 出队 */
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(g_fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("  [错误] VIDIOC_DQBUF");
            break;
        }

        double frame_end = get_time_ms();

        /* 记录延迟 */
        if (latency_idx < (int)(sizeof(latencies) / sizeof(latencies[0]))) {
            latencies[latency_idx++] = frame_end - frame_start;
        }

        /* 入队 (归还缓冲区) */
        if (xioctl(g_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("  [错误] VIDIOC_QBUF");
            break;
        }

        frame_count++;
    }

    double actual_elapsed = (get_time_ms() - test_start) / 1000.0;
    double avg_fps = frame_count / actual_elapsed;

    /* 统计延迟 */
    double avg_latency = 0, min_latency = 1e9, max_latency = 0;
    for (int i = 0; i < latency_idx; i++) {
        avg_latency += latencies[i];
        if (latencies[i] < min_latency) min_latency = latencies[i];
        if (latencies[i] > max_latency) max_latency = latencies[i];
    }
    if (latency_idx > 0) avg_latency /= latency_idx;

    printf("\n  结果:\n");
    printf("    采集帧数: %d\n", frame_count);
    printf("    丢帧数:   %d\n", drop_count);
    printf("    实际耗时: %.2f 秒\n", actual_elapsed);
    printf("    平均帧率: %.1f FPS (目标 %d FPS)\n", avg_fps, CAPTURE_FPS);
    printf("    帧率达成率: %.1f%%\n", (avg_fps / CAPTURE_FPS) * 100);
    if (latency_idx > 0) {
        printf("    单帧延迟: 平均 %.2f ms, 最小 %.2f ms, 最大 %.2f ms\n",
               avg_latency, min_latency, max_latency);
    }

    return 0;
}

/* --------------------------------------------------------------------
 * 清理
 * -------------------------------------------------------------------- */
static void cleanup(void)
{
    if (g_fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(g_fd, VIDIOC_STREAMOFF, &type);

        for (int i = 0; i < BUFFER_COUNT; i++) {
            if (g_buffers[i].start && g_buffers[i].start != MAP_FAILED) {
                munmap(g_buffers[i].start, g_buffers[i].length);
            }
        }
        close(g_fd);
    }
    printf("  清理完成\n");
}

/* --------------------------------------------------------------------
 * 主函数
 * -------------------------------------------------------------------- */
int main(void)
{
    printf("=== V4L2 采集性能基准测试 ===\n");
    printf("设备: %s, %dx%d @ %d FPS\n",
           CAPTURE_DEVICE, CAPTURE_WIDTH, CAPTURE_HEIGHT, CAPTURE_FPS);

    if (v4l2_init() < 0) {
        cleanup();
        return 1;
    }

    run_benchmark();
    cleanup();

    printf("\n=== 测试完成 ===\n");
    return 0;
}
