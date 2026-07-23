/** @file main.c @brief RK3576 OCR translator application orchestration. */
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
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define LIVE_OVERLAY_MAX_ITEMS 32

typedef struct {
    int logging;
    int system;
    int capture;
    int drm;
    int planes;
    int overlay;
    int imu;
    int det;
    int rec;
    int translator;
    int archive;
    int key;
    int pipeline;
    int output_pool;
    int sink;
    int overlay_state;
} init_state_t;

typedef struct {
    ocr_mutex_t lock;
    char text[LIVE_OVERLAY_MAX_ITEMS][OCR_MAX_TEXT_LEN];
    int x[LIVE_OVERLAY_MAX_ITEMS];
    int y[LIVE_OVERLAY_MAX_ITEMS];
    int count;
    uint64_t generation;
    uint64_t presented_generation;
} live_overlay_state_t;

typedef struct {
    int count;
    uint64_t generation;
    char text[LIVE_OVERLAY_MAX_ITEMS][OCR_MAX_TEXT_LEN];
    int x[LIVE_OVERLAY_MAX_ITEMS];
    int y[LIVE_OVERLAY_MAX_ITEMS];
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
static ocr_motion_comp_t g_motion;
static ocr_rga_stabilizer_t g_stabilizer;
static int64_t g_last_imu_timestamp;
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
        (void)ocr_motion_comp_init(&g_motion, 60.0f, 45.0f, config->stab_alpha);
        g_motion.max_scale = 1.0f / config->stab_crop_ratio;
        (void)ocr_rga_stab_init(&g_stabilizer, config->stab_alpha);
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
    return ocr_node_push_input((ocr_pipeline_node_t *)user_data, buffer);
}

static int update_stabilizer_from_imu(uint32_t width, uint32_t height)
{
    if (!g_init.imu) return 0;
    imu_sample_t sample;
    int updated = 0;
    while (ocr_imu_reader_get(&g_imu, &sample) == 0) {
        float dt = 0.0f;
        if (g_last_imu_timestamp > 0 && sample.timestamp > g_last_imu_timestamp)
            dt = (float)((sample.timestamp - g_last_imu_timestamp) / 1.0e9);
        g_last_imu_timestamp = sample.timestamp;
        if (ocr_attitude_update(&g_attitude, sample.accel_si, sample.gyro_si, dt) == 0)
            updated = 1;
    }
    if (updated) {
        ocr_stab_params_t params;
        if (ocr_motion_comp_update(&g_motion, &g_attitude, width, height,
                                   &params) == 0)
            return ocr_rga_stab_update(&g_stabilizer, &params);
    }
    return 0;
}

static int rga_process(ocr_pipeline_node_t *node, ocr_buffer_t *input)
{
    ocr_rga_proc_t *proc = node ? node->user_ctx : NULL;
    if (!proc || !input) return -1;
    ocr_buffer_t *output = ocr_pool_acquire(proc->out_pool);
    if (!output) return -2;
    output->timestamp = input->timestamp;
    output->frame_id = input->frame_id;

    int ret;
    (void)update_stabilizer_from_imu(input->width, input->height);
    if (g_init.imu)
        ret = ocr_rga_stab_apply(&g_stabilizer, input, output);
    else
        ret = ocr_rga_resize(input, output);
    if (ret != 0) {
        (void)ocr_pool_release(proc->out_pool, output);
        return ret;
    }
    if (ocr_node_emit(node, output) < 0) {
        (void)ocr_pool_release(proc->out_pool, output);
        return -3;
    }
    return 1;
}

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
    ++g_overlay_state.generation;
    ocr_mutex_unlock(&g_overlay_state.lock);
}

static int ocr_process(ocr_pipeline_node_t *node, ocr_buffer_t *buffer)
{
    (void)node;
    ocr_text_box_list_t boxes;
    memset(&boxes, 0, sizeof(boxes));
    if (buffer->fd >= 0) (void)ocr_zerocopy_sync(buffer->fd, 0);
    if (ocr_det_run(&g_det, buffer, &boxes) != 0) {
        if (buffer->fd >= 0) (void)ocr_zerocopy_sync(buffer->fd, 1);
        return -1;
    }

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
    if (g_init.translator) {
        for (int i = 0; i < boxes.count; ++i) {
            if (translator_translate(&g_translator, texts[i], translated[i],
                                     OCR_MAX_TEXT_LEN) != 0)
                translated[i][0] = '\0';
        }
    }
    publish_overlay(&boxes, texts, translated);
    if (buffer->fd >= 0) (void)ocr_zerocopy_sync(buffer->fd, 1);
    free(text_ptrs);
    free(translated);
    free(texts);
    return 0;
}

static int display_process(ocr_pipeline_node_t *node, ocr_buffer_t *buffer)
{
    (void)node;
    int ret = ocr_drm_sink_present(&g_sink, buffer);
    if (ret != 0 || !g_init.overlay) return ret;

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
    if (snapshot.generation == 0) return 0;

    (void)ocr_overlay_clear(&g_overlay);
    for (int i = 0; i < snapshot.count; ++i)
        (void)ocr_overlay_render_text(&g_overlay, snapshot.text[i],
                                      snapshot.x[i], snapshot.y[i], 0xEFFFFFFFU);
    if (ocr_overlay_commit(&g_overlay) == 0) {
        ocr_mutex_lock(&g_overlay_state.lock);
        if (g_overlay_state.presented_generation < snapshot.generation)
            g_overlay_state.presented_generation = snapshot.generation;
        ocr_mutex_unlock(&g_overlay_state.lock);
    }
    return 0;
}

static int init_pipeline(int live_ocr)
{
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
    if (ocr_node_link(&g_rga_node, &g_display_node) != 0) return -8;

    if (live_ocr) {
        if (ocr_node_init(&g_ocr_node, "ocr", ocr_process, NULL) != 0 ||
            ocr_pipeline_add(&g_pipeline, &g_ocr_node) != 0 ||
            ocr_node_link(&g_rga_node, &g_ocr_node) != 0) return -9;
    }
    return ocr_v4l2_set_frame_callback(&g_capture, capture_frame, &g_rga_node);
}

static int run_application(void)
{
    uint64_t last_frame_id = UINT64_MAX;
    int stalled_seconds = 0;
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
    if (key_stop_failed) {
        LOG_E("camera key worker did not stop; preserving session resources until process exit");
        return;
    }
    if (g_init.pipeline) {
        ocr_pipeline_dump_stats(&g_pipeline);
        if (ocr_pipeline_destroy(&g_pipeline) != 0) {
            LOG_E("pipeline worker did not stop; preserving dependent resources until process exit");
            return;
        }
        g_init.pipeline = 0;
    }
    /* Overlay must leave its plane while the CRTC is still active. */
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
