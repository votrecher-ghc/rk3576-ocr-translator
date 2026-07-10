/**
 * @file main.c
 * @brief OCR 翻译系统入口：参数解析、模式分发、线程启动
 *
 * 基于 RK3576 的端侧 OCR 实时翻译系统。
 * 单进程多线程架构，零拷贝 DMA-BUF 管线。
 */
#include "system_manager.h"
#include "config.h"
#include "log.h"
#include "timestamp.h"

/* 各模块头文件 */
#include "v4l2_capture.h"
#include "imx415_ctrl.h"
#include "rga_proc.h"
#include "rga_stabilizer.h"
#include "drm_device.h"
#include "drm_plane.h"
#include "drm_fb.h"
#include "drm_overlay.h"
#include "ocr_det.h"
#include "ocr_rec.h"
#include "translator.h"
#include "imu_reader.h"
#include "attitude_fusion.h"
#include "motion_compensate.h"
#include "light_sensor.h"
#include "brightness_ctrl.h"
#include "temp_sensor.h"
#include "fan_ctrl.h"
#include "key_event.h"
#include "capture_session.h"
#include "archiver.h"
#include "storage_mgr.h"
#include "pipeline.h"
#include "msgbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

/* 全局配置 */
static ocr_config_t g_config;

/* 各模块上下文 */
static ocr_system_manager_t  g_sys;
static ocr_v4l2_capture_t    g_capture;
static ocr_imx415_t          g_imx415;
static ocr_drm_device_t      g_drm;
static ocr_drm_planes_t      g_planes;
static ocr_drm_overlay_t     g_overlay;
static ocr_det_t             g_ocr_det;
static ocr_rec_t             g_ocr_rec;
static ocr_translator_t      g_translator;
static ocr_imu_reader_t      g_imu;
static ocr_attitude_t        g_attitude;
static ocr_motion_comp_t     g_motion_comp;
static ocr_light_sensor_t    g_light;
static ocr_brightness_t      g_brightness;
static ocr_temp_sensor_t     g_temp;
static ocr_fan_ctrl_t        g_fan;
static ocr_key_event_t_ctx   g_key;
static ocr_capture_session_t g_session;
static ocr_archiver_t        g_archiver;
static ocr_storage_mgr_t     g_storage;
static ocr_pipeline_t        g_pipeline;

/* 打印使用说明 */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "选项:\n"
        "  -c, --config <file>   配置文件路径 (默认 /etc/ocr_translator.json)\n"
        "  -m, --mode <mode>     运行模式: 0=实时翻译, 1=拍照翻译\n"
        "  -l, --log <level>     日志级别 (0=D 1=I 2=W 3=E 4=F)\n"
        "  -h, --help            显示帮助\n",
        prog);
}

/* 解析命令行参数 */
static int parse_args(int argc, char *argv[], const char **config_path, int *mode, int *log_level)
{
    static struct option long_opts[] = {
        {"config", required_argument, 0, 'c'},
        {"mode",   required_argument, 0, 'm'},
        {"log",    required_argument, 0, 'l'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    *config_path = "/etc/ocr_translator.json";
    *mode = -1;
    *log_level = -1;

    while ((opt = getopt_long(argc, argv, "c:m:l:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c': *config_path = optarg; break;
            case 'm': *mode = atoi(optarg); break;
            case 'l': *log_level = atoi(optarg); break;
            case 'h': print_usage(argv[0]); exit(0);
            default:  print_usage(argv[0]); return -1;
        }
    }
    return 0;
}

/* 初始化日志 */
static int init_logging(const ocr_config_t *cfg, int log_level_override)
{
    log_level_t level = (log_level_override >= 0) ? (log_level_t)log_level_override
                                                   : (log_level_t)cfg->log_level;
    int to_file = (cfg->log_path[0] != '\0');
    return log_init("ocr_translator", level, to_file,
                    to_file ? cfg->log_path : NULL);
}

/* 初始化硬件子系统 */
static int init_hardware(const ocr_config_t *cfg)
{
    int ret;

    /* 1. V4L2 采集 */
    LOG_I("初始化 V4L2 采集...");
    ret = ocr_v4l2_open(&g_capture, cfg->v4l2_device,
                        cfg->capture_width, cfg->capture_height,
                        cfg->capture_fps, OCR_FMT_NV12);
    if (ret != 0) return ret;

    ret = ocr_imx415_init(&g_imx415, g_capture.fd);
    if (ret != 0) LOG_W("IMX415 控制初始化失败（非致命）");

    /* 2. DRM 显示 */
    LOG_I("初始化 DRM 显示...");
    ret = ocr_drm_open(&g_drm, cfg->drm_device);
    if (ret != 0) return ret;
    ret = ocr_drm_setup_crtc(&g_drm);
    if (ret != 0) return ret;
    ret = ocr_drm_planes_init(&g_planes, &g_drm);
    if (ret != 0) return ret;

    /* 3. 叠加渲染 */
    LOG_I("初始化叠加渲染...");
    ret = ocr_overlay_init(&g_overlay, &g_drm, &g_planes,
                           cfg->font_path, cfg->font_size,
                           g_drm.width, g_drm.height);
    if (ret != 0) LOG_W("叠加渲染初始化失败（非致命）");

    /* 4. IMU */
    if (cfg->imu_device[0] != '\0') {
        LOG_I("初始化 IMU...");
        ret = ocr_imu_reader_init(&g_imu, cfg->imu_device, 512);
        if (ret != 0) LOG_W("IMU 初始化失败（非致命）");
        else {
            ocr_attitude_init(&g_attitude, (float)cfg->imu_sample_hz, 0.1f);
            ocr_motion_comp_init(&g_motion_comp, 60.0f, 45.0f, cfg->stab_alpha);
        }
    }

    /* 5. 传感器 */
    LOG_I("初始化传感器...");
    ocr_light_sensor_init(&g_light, "/sys/bus/iio/devices/iio:device1");
    ocr_brightness_init(&g_brightness, cfg->backlight_path);
    ocr_temp_sensor_init(&g_temp, "/sys/bus/iio/devices/iio:device2");
    ocr_fan_ctrl_init(&g_fan, cfg->pwm_path);

    return 0;
}

/* 初始化 AI 子系统 */
static int init_ai(const ocr_config_t *cfg)
{
    int ret;
    LOG_I("初始化 AI 模型...");
    ret = ocr_det_init(&g_ocr_det, cfg->det_model);
    if (ret != 0) return ret;
    ret = ocr_rec_init(&g_ocr_rec, cfg->rec_model, cfg->tokenizer_vocab);
    if (ret != 0) return ret;
    ret = translator_init(&g_translator, cfg->trans_model, cfg->trans_model,
                          cfg->tokenizer_vocab, cfg->tokenizer_vocab);
    if (ret != 0) LOG_W("翻译器初始化失败（非致命）");
    return 0;
}

/* 初始化归档 */
static int init_archive(const ocr_config_t *cfg)
{
    LOG_I("初始化归档...");
    if (ocr_archiver_init(&g_archiver, cfg->archive_dir) != 0) return -1;
    ocr_storage_mgr_init(&g_storage, cfg->archive_dir, cfg->archive_max_percent);
    return 0;
}

/* 初始化输入 */
static int init_input(const ocr_config_t *cfg)
{
    (void)cfg;
    LOG_I("初始化输入...");
    /* TODO: input 设备路径可配置 */
    if (ocr_key_event_init(&g_key, "/dev/input/event0",
                           ocr_capture_session_on_key, &g_session) != 0) {
        LOG_W("按键监听初始化失败（非致命）");
    }
    ocr_capture_session_init(&g_session, &g_capture, &g_ocr_det,
                             &g_ocr_rec, &g_translator, &g_archiver);
    return 0;
}

/* 实时翻译模式 */
static int run_realtime_mode(void)
{
    LOG_I("==== 启动实时翻译模式 ====");

    /* 1. 初始化管线 */
    ocr_pipeline_init(&g_pipeline);
    /* TODO: 创建各节点并连接：
     *   capture → rga(stab) → display + ocr_det → ocr_rec → translator → render
     */

    /* 2. 启动采集 */
    if (ocr_v4l2_start(&g_capture) != 0) {
        LOG_E("采集启动失败");
        return -1;
    }

    /* 3. 启动 IMU */
    if (g_imu.dev_fd >= 0) {
        ocr_imu_reader_start(&g_imu);
    }

    /* 4. 启动按键监听 */
    ocr_key_event_start(&g_key);

    /* 5. 启动管线 */
    ocr_pipeline_start(&g_pipeline);

    atomic_store(&g_sys.state, SYS_STATE_RUNNING);
    LOG_I("系统运行中，等待退出信号...");

    /* 6. 主循环：喂狗 + 传感器轮询 */
    while (!ocr_system_manager_should_exit(&g_sys)) {
        ocr_system_manager_feed(&g_sys);

        /* 传感器轮询（~1Hz） */
        int lux, temp;
        if (ocr_light_sensor_read(&g_light, &lux) == 0) {
            ocr_brightness_update(&g_brightness, lux);
        }
        if (ocr_temp_sensor_read(&g_temp, &temp) == 0) {
            ocr_fan_ctrl_update(&g_fan, temp);
        }

        /* 存储检查 */
        int used;
        if (ocr_storage_mgr_check(&g_storage, &used) == 0 && used > g_config.archive_max_percent) {
            ocr_storage_mgr_cleanup(&g_storage, g_config.archive_max_percent - 10);
        }

        sleep(1);
    }

    /* 7. 优雅退出 */
    LOG_I("开始优雅退出...");
    ocr_pipeline_stop(&g_pipeline);
    ocr_pipeline_dump_stats(&g_pipeline);
    ocr_v4l2_stop(&g_capture);
    ocr_imu_reader_stop(&g_imu);
    ocr_key_event_stop(&g_key);

    return 0;
}

/* 拍照翻译模式 */
static int run_capture_mode(void)
{
    LOG_I("==== 启动拍照翻译模式 ====");
    /* 拍照模式与实时模式类似，但管线仅在按键时触发单帧处理 */
    /* TODO: 实现拍照模式的简化管线 */
    return run_realtime_mode();
}

/* 清理资源 */
static void cleanup(void)
{
    LOG_I("清理资源...");
    ocr_key_event_destroy(&g_key);
    ocr_v4l2_close(&g_capture);
    ocr_imu_reader_destroy(&g_imu);
    ocr_overlay_destroy(&g_overlay);
    ocr_drm_planes_destroy(&g_planes);
    ocr_drm_close(&g_drm);
    ocr_det_destroy(&g_ocr_det);
    ocr_rec_destroy(&g_ocr_rec);
    translator_destroy(&g_translator);
    ocr_archiver_destroy(&g_archiver);
    ocr_system_manager_destroy(&g_sys);
    config_free(&g_config);
    log_deinit();
}

int main(int argc, char *argv[])
{
    const char *config_path;
    int mode_override, log_level_override;

    if (parse_args(argc, argv, &config_path, &mode_override, &log_level_override) != 0) {
        return 1;
    }

    /* 1. 加载配置 */
    if (config_load(&g_config, config_path) != 0) {
        fprintf(stderr, "配置加载失败: %s\n", config_path);
        return 1;
    }
    if (mode_override >= 0) g_config.run_mode = mode_override;

    /* 2. 初始化日志 */
    if (init_logging(&g_config, log_level_override) != 0) {
        fprintf(stderr, "日志初始化失败\n");
        return 1;
    }
    LOG_I("========================================");
    LOG_I("  OCR 翻译系统启动 (RK3576)");
    LOG_I("========================================");
    config_dump(&g_config);

    /* 3. 系统管理器 */
    if (ocr_system_manager_init(&g_sys) != 0) {
        LOG_F("系统管理器初始化失败");
        return 1;
    }
    ocr_system_manager_install_signals(&g_sys);
    ocr_system_manager_start_watchdog(&g_sys, 5000);

    /* 4. 初始化各子系统 */
    if (init_hardware(&g_config) != 0) {
        LOG_F("硬件初始化失败");
        cleanup();
        return 1;
    }
    if (init_ai(&g_config) != 0) {
        LOG_F("AI 初始化失败");
        cleanup();
        return 1;
    }
    if (init_archive(&g_config) != 0) {
        LOG_W("归档初始化失败（非致命）");
    }
    init_input(&g_config);

    /* 5. 模式分发 */
    int ret;
    if (g_config.run_mode == 1) {
        ret = run_capture_mode();
    } else {
        ret = run_realtime_mode();
    }

    /* 6. 清理 */
    cleanup();
    LOG_I("系统退出 (code=%d)", ret);
    return ret;
}
