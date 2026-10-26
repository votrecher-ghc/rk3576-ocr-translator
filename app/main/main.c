/** @file main.c @brief RK3576 OCR translator application orchestration.
 *
 * 程序装配入口：读取配置，初始化设备和模型，连接采集/RGA/显示/OCR，最后按依赖释放资源。
 * 实时模式由 OCR 工作线程使用模型；拍照模式由按键会话使用模型，两者互斥以避免共享上下文并发推理。
 */
#include "system_manager.h"
#include "config.h"
#include "log.h"

#include "v4l2_capture.h"
#include "imx415_ctrl.h"
#include "dma_heap.h"
#include "zerocopy.h"
#include "rga_api.h"
#include "rga_proc.h"
#include "rga_stabilizer.h"
#include "drm_device.h"
#include "drm_plane.h"
#include "drm_overlay.h"
#include "drm_sink.h"
#include "ocr_det.h"
#include "ocr_rec.h"
#include "translator.h"
#include "imu_reader.h"
#include "attitude_fusion.h"
#include "pose_history.h"
#include "motion_compensate.h"
#include "iio_discovery.h"
#include "light_sensor.h"
#include "brightness_ctrl.h"
#include "temp_sensor.h"
#include "fan_ctrl.h"
#include "key_event.h"
#include "capture_session.h"
#include "archiver.h"
#include "storage_mgr.h"
#include "pipeline.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define LIVE_OVERLAY_MAX_ITEMS 32

/*
 * V4L2 和 IIO 都使用 CLOCK_MONOTONIC 时间戳。若实测发现图像相对陀螺仪
 * 固定超前或滞后，可先通过互相关标定，再修改这个偏移量。
 */
#define EIS_CAMERA_IMU_OFFSET_NS INT64_C(0)

typedef struct {
    int logging; /**< 日志初始化成功标记，cleanup 最后据此关闭日志文件。 */
    int system; /**< 系统管理器初始化成功标记，退出时停止软件看门狗并销毁。 */
    int capture; /**< V4L2 打开及缓冲初始化成功标记；不代表采集流正在运行。 */
    int drm; /**< DRM 设备已打开标记，显示依赖资源清理后才能关闭。 */
    int planes; /**< plane 管理器完成初始化标记，退出时仅清理本模块选中层。 */
    int overlay; /**< 文字层初始化成功标记，用于可选绘制及退出时撤销双缓冲。 */
    int imu; /**< IMU 读取器初始化成功标记，决定是否尝试姿态防抖；不保证线程持续运行。 */
    int det; /**< 检测模型及工作区初始化成功标记，cleanup 据此销毁。 */
    int rec; /**< 识别模型及词表初始化成功标记，cleanup 据此销毁。 */
    int translator; /**< 翻译资产及模型可用标记；为 0 时按配置降级或拒绝启动。 */
    int archive; /**< 归档器初始化成功标记，控制结果保存与存储清理。 */
    int key; /**< 拍照 input 监听器初始化成功标记，先停止其会话再释放模型资源。 */
    int pipeline; /**< 管线管理结构已初始化标记，包含部分装配时也据此执行回滚清理。 */
    int output_pool; /**< 独立 RGA 输出池已建立标记，必须等 worker/显示都释放引用后销毁。 */
    int sink; /**< 视频显示 sink 初始化成功标记，退出时先解除扫描再归还 current 引用。 */
    int overlay_state; /**< 共享文字结果锁已初始化标记，worker 退出后才能销毁该锁。 */
} init_state_t;

typedef struct {
    ocr_mutex_t lock; /**< 保护整组文字/坐标及版本，OCR 发布和显示取快照共用此锁。 */
    char text[LIVE_OVERLAY_MAX_ITEMS][OCR_MAX_TEXT_LEN]; /**< 前 count 项有效的 UTF-8 显示文字，每行含 NUL；译文失败时写入原文。 */
    int x[LIVE_OVERLAY_MAX_ITEMS]; /**< 对应 text 的屏幕横向绘制起点，像素，由检测框最小 x 得到。 */
    int y[LIVE_OVERLAY_MAX_ITEMS]; /**< 对应 text 的屏幕纵向绘制起点，像素，由检测框最小 y 得到。 */
    int count; /**< 当前有效文字项数，0..LIVE_OVERLAY_MAX_ITEMS；0 也表示需清掉旧文字的结果。 */
    uint64_t generation; /**< 每次完整发布结果后递增的版本号；不是视频 frame_id，不保证逐帧对应。 */
    uint64_t presented_generation; /**< 显示线程最近成功提交的文字版本；小于 generation 时需更新。 */
} live_overlay_state_t;

typedef struct {
    int count; /**< 本次从共享状态复制的有效项数，复制后只由显示线程使用。 */
    uint64_t generation; /**< 本快照对应的发布版本，0 表示本次没有新结果可提交。 */
    char text[LIVE_OVERLAY_MAX_ITEMS][OCR_MAX_TEXT_LEN]; /**< 文字的栈上值副本，使耗时绘制可在共享锁之外执行。 */
    int x[LIVE_OVERLAY_MAX_ITEMS]; /**< 本快照各项的横向绘制起点，像素，与 text 行一一对应。 */
    int y[LIVE_OVERLAY_MAX_ITEMS]; /**< 本快照各项的纵向绘制起点，像素；不借用共享数组地址。 */
} overlay_snapshot_t;

static ocr_config_t g_config;
static init_state_t g_init;
static ocr_system_manager_t g_system;
static ocr_v4l2_capture_t g_capture;
static ocr_imx415_t g_imx415;
static ocr_drm_device_t g_drm;
static ocr_drm_planes_t g_planes;
static ocr_drm_overlay_t g_overlay;
static ocr_drm_sink_t g_sink;
static ocr_det_t g_det;
static ocr_rec_t g_rec;
static ocr_translator_t g_translator;
static ocr_imu_reader_t g_imu;
static ocr_attitude_t g_attitude;
static ocr_pose_history_t g_pose_history;
static ocr_motion_comp_t g_motion;
static ocr_rga_stabilizer_t g_stabilizer;
static int64_t g_last_imu_timestamp;
static int g_gyro_bias_logged;
static ocr_light_sensor_t g_light;
static ocr_brightness_t g_brightness;
static ocr_temp_sensor_t g_temp;
static ocr_fan_ctrl_t g_fan;
static int g_light_ready, g_brightness_ready, g_temp_ready, g_fan_ready;
static int g_fan_kernel_managed;
static ocr_key_event_t_ctx g_key;
static ocr_capture_session_t g_session;
static ocr_archiver_t g_archiver;
static ocr_storage_mgr_t g_storage;
static ocr_pipeline_t g_pipeline;
static ocr_buffer_pool_t g_output_pool;
static ocr_rga_proc_t g_rga_proc;
static ocr_pipeline_node_t g_rga_node;
static ocr_pipeline_node_t g_display_node;
static ocr_pipeline_node_t g_ocr_node;
static live_overlay_state_t g_overlay_state;

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -c, --config FILE   configuration (default /etc/ocr/ocr_translator.json)\n"
            "  -m, --mode MODE     live|photo|0|1\n"
            "  -l, --log LEVEL     debug|info|warn|error|fatal|0..4\n"
            "  -d, --debug         enable debug logging\n"
            "  -h, --help          show this help\n", program);
}

static int parse_level(const char *text, int *level)
{
    if (!text || !level) return -1;
    const char *names[] = {"debug", "info", "warn", "error", "fatal"};
    for (int i = 0; i < 5; ++i) {
        if (strcasecmp(text, names[i]) == 0) {
            *level = i;
            return 0;
        }
    }
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < 0 || value > 4) return -1;
    *level = (int)value;
    return 0;
}

static int parse_mode(const char *text, int *mode)
{
    if (!text || !mode) return -1;
    if (strcasecmp(text, "live") == 0 || strcmp(text, "0") == 0) {
        *mode = 0;
        return 0;
    }
    if (strcasecmp(text, "photo") == 0 || strcasecmp(text, "capture") == 0 ||
        strcmp(text, "1") == 0) {
        *mode = 1;
        return 0;
    }
    return -1;
}

static int parse_args(int argc, char **argv, const char **config_path,
                      int *mode_override, int *level_override)
{
    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"mode", required_argument, NULL, 'm'},
        {"log", required_argument, NULL, 'l'},
        {"debug", no_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    *config_path = "/etc/ocr/ocr_translator.json";
    *mode_override = -1;
    *level_override = -1;

    int option;
    while ((option = getopt_long(argc, argv, "c:m:l:dh", options, NULL)) != -1) {
        switch (option) {
        case 'c': *config_path = optarg; break;
        case 'm': if (parse_mode(optarg, mode_override) != 0) return -1; break;
        case 'l': if (parse_level(optarg, level_override) != 0) return -1; break;
        case 'd': *level_override = LOG_LEVEL_DEBUG; break;
        case 'h': print_usage(argv[0]); return 1;
        default: return -1;
        }
    }
    return optind == argc ? 0 : -1;
}

static int init_logging(const ocr_config_t *config, int override)
{
    int level = override >= 0 ? override : config->log_level;
    return log_init(config->app_name, (log_level_t)level,
                    config->log_path[0] != '\0', config->log_path);
}

static ocr_pixel_format_t capture_format_from_config(const char *format)
{
    if (format && strcasecmp(format, "NV16") == 0) return OCR_FMT_NV16;
    if (format && strcasecmp(format, "YUYV") == 0) return OCR_FMT_YUYV;
    return OCR_FMT_NV12;
}

static int is_auto_device(const char *value)
{
    return !value || value[0] == '\0' || strcasecmp(value, "auto") == 0;
}

static int copy_device_path(char *destination, size_t destination_size,
                            const char *source)
{
    int length;

    if (!destination || destination_size == 0 || !source) return -1;
    length = snprintf(destination, destination_size, "%s", source);
    return length >= 0 && (size_t)length < destination_size ? 0 : -1;
}

static int resolve_iio_path(const char *configured, const char *iio_name,
                            int want_dev_node, char *resolved,
                            size_t resolved_size)
{
    char sysfs_path[128];
    char dev_path[64];
    const char *selected;

    if (!is_auto_device(configured)) {
        return copy_device_path(resolved, resolved_size, configured);
    }
    if (ocr_iio_find_device(iio_name, sysfs_path, sizeof(sysfs_path),
                            dev_path, sizeof(dev_path)) != 0) {
        return -1;
    }
    selected = want_dev_node ? dev_path : sysfs_path;
    if (copy_device_path(resolved, resolved_size, selected) != 0) return -1;
    LOG_I("自动发现 IIO 设备 %s: %s", iio_name, resolved);
    return 0;
}

/** @brief 先初始化采集和显示，再尝试传感器；可选设备不可用时按对应降级路径继续。 */
static int init_hardware(const ocr_config_t *config)
{
    char imu_path[64];
    char light_path[128];
    char temp_path[128];

    if (ocr_v4l2_open_ex(&g_capture, config->v4l2_device,
                         config->capture_width, config->capture_height,
                         config->capture_fps,
                         capture_format_from_config(config->capture_format),
                         config->capture_buffer_count) != 0) {
        return -1;
    }
    g_init.capture = 1;
    if (ocr_imx415_init(&g_imx415, g_capture.fd) != 0)
        LOG_W("IMX415 controls are unavailable; capture will continue");
    else
        (void)ocr_imx415_set_ae(&g_imx415, 1);

    if (ocr_drm_open(&g_drm, config->drm_device) != 0) return -2;
    g_init.drm = 1;
    if (ocr_drm_setup_crtc_mode(&g_drm, config->display_width,
                                config->display_height) != 0) return -3;
    if (ocr_drm_planes_init(&g_planes, &g_drm) != 0) return -4;
    g_init.planes = 1;

    if (ocr_overlay_init(&g_overlay, &g_drm, &g_planes,
                         config->font_path, config->font_size,
                         g_drm.width, g_drm.height) == 0) {
        g_init.overlay = 1;
    } else {
        LOG_W("text overlay unavailable; video/OCR will continue");
    }

    if (resolve_iio_path(config->imu_device, "icm42688", 1,
                         imu_path, sizeof(imu_path)) == 0 &&
        ocr_imu_reader_init(&g_imu, imu_path, 1024) == 0) {
        g_init.imu = 1;
    } else {
        LOG_W("IMU unavailable; electronic stabilization is disabled");
    }
    if (g_init.imu) {
        (void)ocr_attitude_init(&g_attitude, (float)config->imu_sample_hz,
                                config->madgwick_beta);
        ocr_pose_history_init(&g_pose_history);
        (void)ocr_motion_comp_init(&g_motion, 60.0f, 45.0f, config->stab_alpha);
        g_motion.max_scale = 1.0f / config->stab_crop_ratio;
        (void)ocr_rga_stab_init(&g_stabilizer, config->stab_alpha);
        LOG_I("EIS uses frame timestamp interpolation, camera/IMU offset=%lld ns",
              (long long)EIS_CAMERA_IMU_OFFSET_NS);
    }

    g_light_ready = resolve_iio_path(config->light_iio_device, "ap3216c", 0,
                                     light_path, sizeof(light_path)) == 0 &&
                    ocr_light_sensor_init(&g_light, light_path) == 0;
    if (!g_light_ready) LOG_W("环境光传感器不可用；自动亮度已禁用");
    g_brightness_ready = ocr_brightness_init(
        &g_brightness, config->backlight_path) == 0;
    g_temp_ready = resolve_iio_path(config->temp_iio_device, "adt7410", 0,
                                    temp_path, sizeof(temp_path)) == 0 &&
                   ocr_temp_sensor_init(&g_temp, temp_path) == 0;
    if (!g_temp_ready) LOG_W("温度传感器不可用");
    g_fan_kernel_managed = strcasecmp(config->pwm_path, "kernel") == 0;
    if (g_fan_kernel_managed) {
        LOG_I("PWM 风扇由 ocr-pwm-fan 内核模块管理");
        g_fan_ready = 0;
    } else {
        g_fan_ready = ocr_fan_ctrl_init(&g_fan, config->pwm_path) == 0;
    }
    if (!g_fan_ready && !g_fan_kernel_managed) {
        LOG_W("PWM 风扇不可用");
    } else if (!g_temp_ready) {
        if (g_fan_kernel_managed) {
            LOG_W("用户态温度反馈不可用；内核风扇驱动保持独立故障安全控制");
        } else {
            LOG_W("温度反馈不可用；风扇进入全速故障安全模式");
            (void)ocr_fan_ctrl_set_level(&g_fan, FAN_MAX_LEVEL);
        }
    }
    return 0;
}

/** @brief 加载检测/识别模型并校验翻译资产，是否允许仅 OCR 由配置决定。 */
static int init_ai(const ocr_config_t *config)
{
    if (ocr_det_init(&g_det, config->det_model) != 0) return -1;
    g_init.det = 1;
    g_det.thresh = config->det_threshold;
    g_det.box_thresh = config->det_box_threshold;
    if (ocr_rec_init(&g_rec, config->rec_model, config->rec_vocab) != 0)
        return -2;
    g_init.rec = 1;

    if (translator_validate_manifest(config->trans_manifest,
                                     config->src_lang,
                                     config->tgt_lang) == 0 &&
        translator_init(&g_translator,
                        config->trans_encoder_model,
                        config->trans_decoder_model,
                        config->trans_src_vocab,
                        config->trans_tgt_vocab) == 0) {
        g_init.translator = 1;
    } else {
        if (!config->allow_ocr_only) {
            LOG_E("translation model contract is unavailable and OCR-only fallback is disabled");
            return -3;
        }
        LOG_W("translation unavailable; explicitly falling back to OCR-only mode");
    }
    return 0;
}

/** @brief 准备归档和拍照按键；只有拍照模式接入按键会话以避免模型并发使用。 */
static int init_archive_and_input(const ocr_config_t *config)
{
    char discovered_key[64];
    const char *key_path = config->key_device;

    if (ocr_archiver_init(&g_archiver, config->archive_dir) == 0) {
        g_init.archive = 1;
        (void)ocr_storage_mgr_init(&g_storage, config->archive_dir,
                                   config->archive_max_percent);
    } else {
        LOG_W("archive directory unavailable; photo mode cannot save results");
        if (config->run_mode == 1) return -1;
    }

    /* Live OCR owns the single RKNN context on its worker thread.  Photo mode
     * is mutually exclusive so the key thread can safely run a synchronous
     * capture session with those same model objects. */
    if (config->run_mode != 1) return 0;

    if (ocr_capture_session_init(&g_session, &g_capture, &g_det, &g_rec,
                                 &g_translator,
                                 g_init.archive ? &g_archiver : NULL) != 0)
        return -1;
    (void)ocr_capture_session_configure(&g_session, config->src_lang,
                                        config->tgt_lang,
                                        config->jpeg_quality);
    if (is_auto_device(key_path)) {
        if (ocr_key_event_find_device(discovered_key, sizeof(discovered_key)) != 0) {
            LOG_E("未发现支持 KEY_CAMERA 的 input 设备");
            return -2;
        }
        key_path = discovered_key;
    }
    if (ocr_key_event_init(&g_key, key_path,
                           ocr_capture_session_on_key, &g_session) == 0) {
        g_init.key = 1;
    } else {
        LOG_E("photo mode requires a working KEY_CAMERA input device");
        return -2;
    }
    return 0;
}

static int capture_frame(ocr_buffer_t *buffer, void *user_data)
{
    /* 队列接管采集线程的这一份引用；只传指针，不复制 DMA-BUF 中的像素。
     * 入队失败时引用仍归采集线程，由它执行 unref。 */
    return ocr_node_push_input((ocr_pipeline_node_t *)user_data, buffer);
}

static int frame_pose_timestamp(uint64_t frame_timestamp,
                                int64_t *pose_timestamp)
{
    if (!pose_timestamp || frame_timestamp == 0 ||
        frame_timestamp > (uint64_t)INT64_MAX) {
        return -1;
    }
    if (EIS_CAMERA_IMU_OFFSET_NS > 0 &&
        (int64_t)frame_timestamp > INT64_MAX - EIS_CAMERA_IMU_OFFSET_NS) {
        return -1;
    }
    if (EIS_CAMERA_IMU_OFFSET_NS < 0 &&
        (int64_t)frame_timestamp < INT64_MIN - EIS_CAMERA_IMU_OFFSET_NS) {
        return -1;
    }
    *pose_timestamp = (int64_t)frame_timestamp + EIS_CAMERA_IMU_OFFSET_NS;
    return 0;
}

/** @brief 消费 IMU 样本并记录姿态，按图像时间戳插值后生成本帧防抖参数。 */
static int update_stabilizer_from_imu(uint32_t width, uint32_t height,
                                      uint64_t frame_timestamp)
{
    imu_sample_t sample;
    quaternion_t frame_q;
    ocr_attitude_t frame_attitude;
    int64_t target_timestamp;

    if (!g_init.imu) return 0;

    while (ocr_imu_reader_get(&g_imu, &sample) == 0) {
        float dt = 0.0f;

        if (g_last_imu_timestamp > 0 &&
            sample.timestamp <= g_last_imu_timestamp) {
            LOG_W("IMU timestamp moved backwards; resetting EIS history");
            g_last_imu_timestamp = 0;
            ocr_pose_history_reset(&g_pose_history);
            ocr_attitude_reset(&g_attitude);
            ocr_motion_comp_reset(&g_motion);
        }
        if (g_last_imu_timestamp > 0)
            dt = (float)((sample.timestamp - g_last_imu_timestamp) / 1.0e9);
        g_last_imu_timestamp = sample.timestamp;

        if (ocr_attitude_update(&g_attitude, sample.accel_si,
                                sample.gyro_si, dt) == 0) {
            int push_result = ocr_pose_history_push(&g_pose_history,
                                                     sample.timestamp,
                                                     &g_attitude.q);
            if (push_result == -2) {
                ocr_pose_history_reset(&g_pose_history);
                (void)ocr_pose_history_push(&g_pose_history,
                                            sample.timestamp,
                                            &g_attitude.q);
            }
        }
    }

    if (!g_gyro_bias_logged && ocr_attitude_bias_ready(&g_attitude)) {
        LOG_I("IMU gyro bias calibration complete");
        g_gyro_bias_logged = 1;
    }

    if (frame_pose_timestamp(frame_timestamp, &target_timestamp) != 0 ||
        ocr_pose_history_sample(&g_pose_history, target_timestamp,
                                &frame_q) != 0) {
        return 0;
    }

    /* 只替换查询时刻的四元数，保留融合器配置和校准状态。 */
    frame_attitude = g_attitude;
    frame_attitude.q = frame_q;

    {
        ocr_stab_params_t params;
        if (ocr_motion_comp_update(&g_motion, &frame_attitude,
                                   width, height, &params) == 0)
            return ocr_rga_stab_update(&g_stabilizer, &params);
    }
    return 0;
}

/**
 * @brief 主流程的 RGA 回调：借用采集图 A，独占生成输出图 B，再转交给显示/OCR。
 * @return 1 表示已发布 B，节点不再转发 A；负数表示本帧失败，节点仍会释放 A。
 * @note 此处是实际装配的回调；rga_proc.c 的通用回调没有直接接入主流程，
 *       因为这里还需要用 IMU 姿态选择防抖裁剪路径。
 */
static int rga_process(ocr_pipeline_node_t *node, ocr_buffer_t *input)
{
    ocr_rga_proc_t *proc = node ? node->user_ctx : NULL;
    if (!proc || !input) return -1;
    /* input 是 DQBUF 后交来的采集缓冲 A，回调期间由节点持有引用。
     * output 是另一个输出池中的空闲缓冲 B，RGA 负责从 A 读、向 B 写。 */
    ocr_buffer_t *output = ocr_pool_acquire(proc->out_pool);
    /* 输出池耗尽表示旧图还在显示、排队或 OCR 中，不能挑一块“看起来旧”的覆盖。
     * 直接丢当前输入并让节点归还引用，避免阻塞采集并积累更大延迟。 */
    if (!output) return -2;
    /* fd/index 标识长期复用的内存，timestamp/frame_id 标识本次存入这块内存的图像。
     * 复制帧标识把变换后的 B 与原始采集 A 关联，不是复制像素数据。 */
    output->timestamp = input->timestamp;
    output->frame_id = input->frame_id;

    int ret;
    /* 姿态按输入图曝光相关时间插值，而不是直接使用处理时刻的最新 IMU 值。
     * 查询/更新失败时沿用已有防抖状态，当前实现不因此取消本帧处理。 */
    (void)update_stabilizer_from_imu(input->width, input->height,
                                     input->timestamp);
    if (g_init.imu)
        ret = ocr_rga_stab_apply(&g_stabilizer, input, output);
    else
        ret = ocr_rga_resize(input, output);
    if (ret != 0) {
        /* B 尚未交给任何下游，只有本回调的一份引用，直接归还输出池。
         * A 是借来的，由 node_thread_entry 统一释放，不能在这里再 unref。 */
        (void)ocr_pool_release(proc->out_pool, output);
        return ret;
    }
    /* 上面的 RGA 调用已同步完成，才允许显示/OCR 读取 B。
     * emit 消费 output 的引用；本回调返回后，节点再释放 input 的引用。
     * A 最后一个引用归零时，采集回收回调才会 QBUF，允许摄像头重写 A。 */
    if (ocr_node_emit(node, output) < 0) {
        (void)ocr_pool_release(proc->out_pool, output);
        return -3;
    }
    /* 不能返回 0：0 会让 node_thread_entry 再把原始采集 A 转发一遍。
     * emit 非负返回已消费 B 的原引用，即使接收者数量为 0，也不能再 release B。 */
    return 1;
}

/** @brief 在互斥锁内发布完整识别结果快照及版本号，供显示线程下一次刷新读取。 */
static void publish_overlay(const ocr_text_box_list_t *boxes,
                            char (*texts)[OCR_MAX_TEXT_LEN],
                            char (*translated)[OCR_MAX_TEXT_LEN])
{
    ocr_mutex_lock(&g_overlay_state.lock);
    int count = boxes->count;
    if (count > LIVE_OVERLAY_MAX_ITEMS) count = LIVE_OVERLAY_MAX_ITEMS;
    g_overlay_state.count = count;
    for (int i = 0; i < count; ++i) {
        const char *value = translated[i][0] ? translated[i] : texts[i];
        snprintf(g_overlay_state.text[i], sizeof(g_overlay_state.text[i]),
                 "%s", value);
        float min_x = boxes->boxes[i].x[0], min_y = boxes->boxes[i].y[0];
        for (int p = 1; p < OCR_BOX_POINTS; ++p) {
            if (boxes->boxes[i].x[p] < min_x) min_x = boxes->boxes[i].x[p];
            if (boxes->boxes[i].y[p] < min_y) min_y = boxes->boxes[i].y[p];
        }
        g_overlay_state.x[i] = (int)fmaxf(0.0f, min_x);
        g_overlay_state.y[i] = (int)fmaxf(0.0f, min_y);
    }
    /* 所有文本/坐标更新完成后再递增版本号；显示线程拿同一把锁复制快照，
     * 不会看到新 count 配上未更新的旧文本。此版本不是摄像头的 frame_id。 */
    ++g_overlay_state.generation;
    ocr_mutex_unlock(&g_overlay_state.lock);
}

/** @brief 借用 RGA 输出，执行 CPU 预处理和模型推理，发布文本后结束 CPU 读访问。 */
static int ocr_process(ocr_pipeline_node_t *node, ocr_buffer_t *buffer)
{
    (void)node;
    ocr_text_box_list_t boxes;
    memset(&boxes, 0, sizeof(boxes));
    /* RGA 在发布前已经完成；这里开始 CPU 读访问，处理缓存一致性。
     * 当前路径在 CPU 上预处理图像，再向 RKNN 传普通张量，并未直接导入 fd。
     * 注意：现有代码忽略 sync 返回值；缓存同步失败目前只记录日志。 */
    if (buffer->fd >= 0) (void)ocr_zerocopy_sync(buffer->fd, 0);
    if (ocr_det_run(&g_det, buffer, &boxes) != 0) {
        if (buffer->fd >= 0) (void)ocr_zerocopy_sync(buffer->fd, 1);
        return -1;
    }

    /* 至少分配一个元素，避免无文本时 calloc(0) 的实现差异被误判为内存不足。
     * texts/translated 是连续的定长行，text_ptrs 为批识别接口构建逐行指针视图。 */
    size_t count = boxes.count > 0 ? (size_t)boxes.count : 1U;
    char (*texts)[OCR_MAX_TEXT_LEN] = calloc(count, sizeof(*texts));
    char (*translated)[OCR_MAX_TEXT_LEN] = calloc(count, sizeof(*translated));
    char **text_ptrs = calloc(count, sizeof(*text_ptrs));
    if (!texts || !translated || !text_ptrs) {
        if (buffer->fd >= 0) (void)ocr_zerocopy_sync(buffer->fd, 1);
        free(text_ptrs); free(translated); free(texts);
        return -2;
    }
    for (int i = 0; i < boxes.count; ++i) text_ptrs[i] = texts[i];
    if (boxes.count > 0 && ocr_rec_run_batch(&g_rec, buffer, &boxes, text_ptrs) != 0) {
        if (buffer->fd >= 0) (void)ocr_zerocopy_sync(buffer->fd, 1);
        free(text_ptrs); free(translated); free(texts);
        return -3;
    }
    /* 每个框按阅读顺序翻译。单条翻译失败只清空该译文，publish_overlay 会回退
     * 显示对应原文，其他框的成功结果仍保留。 */
    if (g_init.translator) {
        for (int i = 0; i < boxes.count; ++i) {
            if (translator_translate(&g_translator, texts[i], translated[i],
                                     OCR_MAX_TEXT_LEN) != 0)
                translated[i][0] = '\0';
        }
    }
    /* START/END 覆盖整个同步 OCR 回调，包括预处理和模型执行。
     * END 完成也没有归还图像；输入引用由工作线程随后 unref，显示端的引用独立存在。 */
    publish_overlay(&boxes, texts, translated);
    if (buffer->fd >= 0) (void)ocr_zerocopy_sync(buffer->fd, 1);
    free(text_ptrs);
    free(translated);
    free(texts);
    return 0;
}

/** @brief 先呈现视频帧，再按最新发布版本更新文字层；文字结果没有逐帧匹配屏幕图像。 */
static int display_process(ocr_pipeline_node_t *node, ocr_buffer_t *buffer)
{
    (void)node;
    int ret = ocr_drm_sink_present(&g_sink, buffer);
    if (ret != 0 || !g_init.overlay) return ret;

    /* 在锁内复制到栈上，随后的 FreeType 绘字和 DRM 提交在锁外执行。
     * 如果把耗时绘制放进锁内，会阻塞 OCR 发布下一次识别结果。 */
    overlay_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    ocr_mutex_lock(&g_overlay_state.lock);
    if (g_overlay_state.generation != g_overlay_state.presented_generation) {
        snapshot.count = g_overlay_state.count;
        snapshot.generation = g_overlay_state.generation;
        for (int i = 0; i < snapshot.count; ++i) {
            memcpy(snapshot.text[i], g_overlay_state.text[i],
                   sizeof(snapshot.text[i]));
            snapshot.x[i] = g_overlay_state.x[i];
            snapshot.y[i] = g_overlay_state.y[i];
        }
    }
    ocr_mutex_unlock(&g_overlay_state.lock);
    /* 0 表示这次没有新快照，保留当前文字层；新快照 count=0 则仍需 clear，
     * 用来清除上一帧残留文字，不能因为“没有文本框”就跳过刷新。 */
    if (snapshot.generation == 0) return 0;

    (void)ocr_overlay_clear(&g_overlay);
    for (int i = 0; i < snapshot.count; ++i)
        (void)ocr_overlay_render_text(&g_overlay, snapshot.text[i],
                                      snapshot.x[i], snapshot.y[i], 0xEFFFFFFFU);
    /* 只有真正提交成功才推进 presented_generation；失败保持旧版本供下次重试。
     * 若绘制期间 OCR 已发布更新版本，这里只确认本次 snapshot 的版本，不吞掉新结果。 */
    if (ocr_overlay_commit(&g_overlay) == 0) {
        ocr_mutex_lock(&g_overlay_state.lock);
        if (g_overlay_state.presented_generation < snapshot.generation)
            g_overlay_state.presented_generation = snapshot.generation;
        ocr_mutex_unlock(&g_overlay_state.lock);
    }
    return 0;
}

/** @brief 创建输出池并连接采集到 RGA，再把同一输出分发给显示及可选 OCR 分支。 */
static int init_pipeline(int live_ocr)
{
    /* 采集池由 V4L2 分配；这里另建四块 NV12 输出缓冲供 RGA 写入。
     * map_cpu=1 为 OCR 建立 CPU 映射，显示和 OCR 共享同一块输出图像。 */
    if (ocr_dma_heap_alloc_pool(&g_output_pool, 4, g_drm.width, g_drm.height,
                                OCR_FMT_NV12, NULL, 1) != 0) return -1;
    g_init.output_pool = 1;
    if (ocr_rga_proc_init(&g_rga_proc, &g_output_pool, g_drm.width,
                          g_drm.height, OCR_FMT_NV12) != 0) return -2;
    if (ocr_drm_sink_init(&g_sink, &g_drm, &g_planes) != 0) return -3;
    g_init.sink = 1;
    if (ocr_mutex_init(&g_overlay_state.lock) != 0) return -4;
    g_init.overlay_state = 1;

    if (ocr_pipeline_init(&g_pipeline) != 0) return -5;
    g_init.pipeline = 1;
    if (ocr_node_init(&g_rga_node, "rga", rga_process, &g_rga_proc) != 0 ||
        ocr_pipeline_add(&g_pipeline, &g_rga_node) != 0) return -6;
    if (ocr_node_init(&g_display_node, "display", display_process, &g_sink) != 0 ||
        ocr_pipeline_add(&g_pipeline, &g_display_node) != 0) return -7;
    /* 这里只登记拓扑，不运行处理。RGA 发出 B 后，node 层给每个下游各一份引用；
     * display 和 OCR 使用的是同一个 B，没有为两个分支另做整帧复制。 */
    if (ocr_node_link(&g_rga_node, &g_display_node) != 0) return -8;

    if (live_ocr) {
        if (ocr_node_init(&g_ocr_node, "ocr", ocr_process, NULL) != 0 ||
            ocr_pipeline_add(&g_pipeline, &g_ocr_node) != 0 ||
            ocr_node_link(&g_rga_node, &g_ocr_node) != 0) return -9;
    }
    /* 摄像头有自己的采集线程，通过此回调接入第一个 RGA 节点，不是管线里的
     * 一个普通 worker。live_ocr=0 时仅运行预览，模型由拍照会话独占使用。 */
    return ocr_v4l2_set_frame_callback(&g_capture, capture_frame, &g_rga_node);
}

/** @brief 启动工作线程，检查采集/按键存活情况，并周期处理传感器、存储和喂狗。 */
static int run_application(void)
{
    uint64_t last_frame_id = UINT64_MAX;
    int stalled_seconds = 0;
    /* 先启动全部消费者，再开摄像头供帧；这样首帧不会进入尚未工作的下游。
     * 初始化中途失败由 main 的 cleanup 根据 g_init 标志逐项撤销。 */
    if (ocr_pipeline_start(&g_pipeline) != 0) return -1;
    if (g_init.imu && ocr_imu_reader_start(&g_imu) != 0)
        LOG_W("IMU worker failed; stabilization will use its last state");
    if (g_init.key && ocr_key_event_start(&g_key) != 0) {
        LOG_E("camera key worker failed to start");
        return -2;
    }
    if (ocr_v4l2_start(&g_capture) != 0) return -3;
    if (ocr_system_manager_start_watchdog(&g_system, 5000) != 0) return -4;

    atomic_store(&g_system.state, SYS_STATE_RUNNING);
    LOG_I("system running in %s mode", g_config.run_mode == 1 ? "photo" : "live");
    while (!ocr_system_manager_should_exit(&g_system)) {
        if (!atomic_load(&g_capture.running) ||
            atomic_load(&g_capture.queue_error)) {
            LOG_E("capture worker stopped unexpectedly (queue_error=%d)",
                  atomic_load(&g_capture.queue_error));
            ocr_system_manager_request_exit(&g_system, 10);
            break;
        }
        if (g_init.key && !ocr_key_event_is_running(&g_key)) {
            int key_error = ocr_key_event_get_worker_error(&g_key);
            LOG_E("camera key worker stopped unexpectedly (error=%d)",
                  key_error);
            ocr_system_manager_request_exit(&g_system, 12);
            break;
        }
        /* running=1 不保证仍在产帧，因此另检查帧号是否持续变化。
         * get_latest 增加一份引用，检查完立即释放，不能让状态监控长期占住采集内存。 */
        ocr_buffer_t *latest = ocr_v4l2_get_latest(&g_capture);
        if (!latest || latest->frame_id == last_frame_id) {
            ++stalled_seconds;
        } else {
            last_frame_id = latest->frame_id;
            stalled_seconds = 0;
        }
        if (latest) (void)ocr_buffer_unref(latest);
        if (stalled_seconds >= 5) {
            LOG_E("capture produced no new frame for %d seconds",
                  stalled_seconds);
            ocr_system_manager_request_exit(&g_system, 11);
            break;
        }
        ocr_system_manager_feed(&g_system);
        int value;
        if (g_light_ready && g_brightness_ready &&
            ocr_light_sensor_read(&g_light, &value) == 0)
            (void)ocr_brightness_update(&g_brightness, value);
        if (g_fan_ready) {
            if (g_temp_ready && ocr_temp_sensor_read(&g_temp, &value) == 0) {
                (void)ocr_fan_ctrl_update(&g_fan, value);
            } else if (g_fan.cur_level != FAN_MAX_LEVEL) {
                LOG_W("温度读取失败；风扇切换为全速故障安全模式");
                (void)ocr_fan_ctrl_set_level(&g_fan, FAN_MAX_LEVEL);
            }
        }
        if (g_init.archive && ocr_storage_mgr_check(&g_storage, &value) == 0 &&
            value > g_config.archive_max_percent) {
            int cleanup_target = g_config.archive_max_percent > 10
                               ? g_config.archive_max_percent - 10 : 0;
            (void)ocr_storage_mgr_cleanup(&g_storage,
                                          cleanup_target);
        }
        sleep(1);
    }
    return atomic_load(&g_system.exit_code);
}

static void cleanup(void)
{
    /* 第一阶段阻止新业务进入：按键线程可能正在执行拍照推理，先等它结束；
     * 随后停采集/IMU，避免节点销毁时仍有生产者入队。此时暂不关闭图像 fd。 */
    int key_stop_failed = g_init.key && ocr_key_event_stop(&g_key) != 0;
    if (g_init.capture) (void)ocr_v4l2_stop(&g_capture);
    if (g_init.imu) (void)ocr_imu_reader_stop(&g_imu);
    /* PWM ownership is independent of pipeline workers. Restore an external
     * channel, or disable and unexport a channel created by this process,
     * before any later cleanup path can return early. */
    if (g_fan_ready || g_fan.exported_by_us || g_fan.original_state_valid) {
        if (ocr_fan_ctrl_destroy(&g_fan) != 0 &&
            ocr_fan_ctrl_destroy(&g_fan) != 0) {
            LOG_E("fan PWM cleanup failed after retry");
        } else {
            g_fan_ready = 0;
        }
    }
    /* join 失败时宁可保留依赖资源到进程退出，也不能销毁尚在使用的模型、
     * 会话或图像映射；下面对管线/显示的失败也采用同样的资源保留原则。 */
    if (key_stop_failed) {
        LOG_E("camera key worker did not stop; preserving session resources until process exit");
        return;
    }
    /* 第二阶段结束所有处理回调并清空队列引用；这之后仍可能有 DRM current
     * 持有的扫描引用，所以不能在 pipeline_destroy 后立刻销毁输出池。 */
    if (g_init.pipeline) {
        ocr_pipeline_dump_stats(&g_pipeline);
        if (ocr_pipeline_destroy(&g_pipeline) != 0) {
            LOG_E("pipeline worker did not stop; preserving dependent resources until process exit");
            return;
        }
        g_init.pipeline = 0;
    }
    /* 第三阶段先撤文字层，再让 sink 解除 primary 扫描并释放 current。
     * 文字层退出需要有效 CRTC，所以先于 sink 关闭；二者均先于 DRM fd 关闭。 */
    if (g_init.overlay) {
        ocr_overlay_destroy(&g_overlay);
        if (g_overlay.resource_active) {
            LOG_E("overlay cleanup failed; preserving dependent DRM resources");
            return;
        }
        g_init.overlay = 0;
    }
    if (g_init.sink) {
        ocr_drm_sink_destroy(&g_sink);
        if (g_sink.initialized) {
            LOG_E("DRM sink cleanup failed; preserving dependent DRM resources");
            return;
        }
        g_init.sink = 0;
    }
    /* 到这里生产者、队列、OCR、显示扫描都应停止使用 B，才能 munmap/close。
     * 采集池 A 则在下面 v4l2_close 中销毁；两个池的 fd 不能混为一组释放。 */
    if (g_init.output_pool && ocr_pool_destroy(&g_output_pool) != 0)
        LOG_E("RGA output pool still has outstanding references");
    if (g_init.overlay_state) ocr_mutex_destroy(&g_overlay_state.lock);
    if (g_init.key) ocr_key_event_destroy(&g_key);
    if (g_init.capture && ocr_v4l2_close(&g_capture) != 0)
        LOG_E("capture close deferred by outstanding references");
    if (g_init.imu) ocr_imu_reader_destroy(&g_imu);
    if (g_init.planes) ocr_drm_planes_destroy(&g_planes);
    if (g_init.drm) ocr_drm_close(&g_drm);
    if (g_init.translator) translator_destroy(&g_translator);
    if (g_init.rec) ocr_rec_destroy(&g_rec);
    if (g_init.det) ocr_det_destroy(&g_det);
    if (g_init.archive) ocr_archiver_destroy(&g_archiver);
    if (g_init.system) ocr_system_manager_destroy(&g_system);
    config_free(&g_config);
    if (g_init.logging) log_deinit();
    memset(&g_init, 0, sizeof(g_init));
}

/** @brief 依次加载配置、初始化系统、运行主循环；通过统一清理路径处理初始化中途失败。 */
int main(int argc, char **argv)
{
    const char *config_path;
    int mode_override, level_override;
    int parsed = parse_args(argc, argv, &config_path,
                            &mode_override, &level_override);
    if (parsed != 0) {
        if (parsed < 0) print_usage(argv[0]);
        return parsed > 0 ? 0 : 2;
    }
    if (config_load(&g_config, config_path) != 0) {
        fprintf(stderr, "failed to load configuration: %s\n", config_path);
        return 1;
    }
    if (mode_override >= 0) g_config.run_mode = mode_override;
    if (init_logging(&g_config, level_override) != 0) {
        fprintf(stderr, "failed to initialize logging: %s\n",
                g_config.log_path[0] ? g_config.log_path : "stderr");
        config_free(&g_config);
        return 1;
    }
    g_init.logging = 1;
    LOG_I("OCR translator starting");
    config_dump(&g_config);

    int result = 1;
    if (ocr_system_manager_init(&g_system) != 0) goto done;
    g_init.system = 1;
    if (ocr_system_manager_install_signals(&g_system) != 0) goto done;
    if (init_hardware(&g_config) != 0) goto done;
    if (init_ai(&g_config) != 0) goto done;
    if (init_archive_and_input(&g_config) != 0) goto done;
    if (init_pipeline(g_config.run_mode == 0) != 0) goto done;
    result = run_application();

done:
    LOG_I("OCR translator stopping with code %d", result);
    cleanup();
    return result;
}
