/**
 * @file config.c
 * @brief JSON 配置解析实现（基于 cJSON）
 */
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

/* 将 cJSON 字符串字段安全拷贝到目标缓冲 */
static void cfg_strcpy(char *dst, size_t dst_size, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

/* 将 cJSON 整型字段写入目标 */
static void cfg_int(int *dst, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item)) {
        *dst = item->valueint;
    }
}

int config_load(ocr_config_t *cfg, const char *path)
{
    if (!cfg || !path) {
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* 默认值 */
    cfg->run_mode = 0;
    cfg->log_level = 1;
    cfg->capture_width = 1920;
    cfg->capture_height = 1080;
    cfg->capture_fps = 30;
    cfg->display_width = 1920;
    cfg->display_height = 1080;
    cfg->font_size = 32;
    cfg->archive_max_percent = 90;
    cfg->imu_sample_hz = 1000;
    cfg->stab_alpha = 0.85f;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_E("无法打开配置文件: %s", path);
        return -2;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = (char *)malloc(fsize + 1);
    if (!buf) {
        fclose(fp);
        return -3;
    }

    size_t nread = fread(buf, 1, fsize, fp);
    buf[nread] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        LOG_E("配置文件 JSON 解析失败: %s", path);
        return -4;
    }

    /* 顶层字段 */
    cfg_strcpy(cfg->app_name, sizeof(cfg->app_name), root, "app_name");
    cfg_int(&cfg->run_mode, root, "run_mode");
    cfg_int(&cfg->log_level, root, "log_level");
    cfg_strcpy(cfg->log_path, sizeof(cfg->log_path), root, "log_path");

    /* 采集 */
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(root, "capture");
    if (cap) {
        cfg_strcpy(cfg->v4l2_device, sizeof(cfg->v4l2_device), cap, "device");
        cfg_int((int *)&cfg->capture_width, cap, "width");
        cfg_int((int *)&cfg->capture_height, cap, "height");
        cfg_int((int *)&cfg->capture_fps, cap, "fps");
    }

    /* 显示 */
    const cJSON *disp = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (disp) {
        cfg_strcpy(cfg->drm_device, sizeof(cfg->drm_device), disp, "device");
        cfg_int((int *)&cfg->display_width, disp, "width");
        cfg_int((int *)&cfg->display_height, disp, "height");
    }

    /* AI 模型 */
    const cJSON *ai = cJSON_GetObjectItemCaseSensitive(root, "ai");
    if (ai) {
        cfg_strcpy(cfg->det_model, sizeof(cfg->det_model), ai, "det_model");
        cfg_strcpy(cfg->rec_model, sizeof(cfg->rec_model), ai, "rec_model");
        cfg_strcpy(cfg->trans_model, sizeof(cfg->trans_model), ai, "trans_model");
        cfg_strcpy(cfg->tokenizer_vocab, sizeof(cfg->tokenizer_vocab), ai, "vocab");
        cfg_strcpy(cfg->src_lang, sizeof(cfg->src_lang), ai, "src_lang");
        cfg_strcpy(cfg->tgt_lang, sizeof(cfg->tgt_lang), ai, "tgt_lang");
    }

    /* 渲染字体 */
    const cJSON *rend = cJSON_GetObjectItemCaseSensitive(root, "render");
    if (rend) {
        cfg_strcpy(cfg->font_path, sizeof(cfg->font_path), rend, "font_path");
        cfg_int(&cfg->font_size, rend, "font_size");
    }

    /* 归档 */
    const cJSON *arch = cJSON_GetObjectItemCaseSensitive(root, "archive");
    if (arch) {
        cfg_strcpy(cfg->archive_dir, sizeof(cfg->archive_dir), arch, "dir");
        cfg_int(&cfg->archive_max_percent, arch, "max_percent");
    }

    /* IMU/防抖 */
    const cJSON *imu = cJSON_GetObjectItemCaseSensitive(root, "imu");
    if (imu) {
        cfg_strcpy(cfg->imu_device, sizeof(cfg->imu_device), imu, "device");
        cfg_int(&cfg->imu_sample_hz, imu, "sample_hz");
        /* TODO: 读取 stab_alpha 浮点值 */
    }

    /* 背光/风扇 */
    const cJSON *hw = cJSON_GetObjectItemCaseSensitive(root, "hardware");
    if (hw) {
        cfg_strcpy(cfg->backlight_path, sizeof(cfg->backlight_path), hw, "backlight");
        cfg_strcpy(cfg->pwm_path, sizeof(cfg->pwm_path), hw, "pwm");
    }

    cJSON_Delete(root);
    return 0;
}

void config_free(ocr_config_t *cfg)
{
    if (cfg) {
        memset(cfg, 0, sizeof(*cfg));
    }
}

void config_dump(const ocr_config_t *cfg)
{
    if (!cfg) return;
    LOG_I("==== 配置 dump ====");
    LOG_I("app_name=%s run_mode=%d", cfg->app_name, cfg->run_mode);
    LOG_I("capture=%ux%u@%u dev=%s", cfg->capture_width, cfg->capture_height,
          cfg->capture_fps, cfg->v4l2_device);
    LOG_I("display=%ux%u dev=%s", cfg->display_width, cfg->display_height, cfg->drm_device);
    LOG_I("det_model=%s", cfg->det_model);
    LOG_I("rec_model=%s", cfg->rec_model);
    LOG_I("font=%s size=%d", cfg->font_path, cfg->font_size);
    LOG_I("archive=%s max%%=%d", cfg->archive_dir, cfg->archive_max_percent);
    LOG_I("imu=%s hz=%d", cfg->imu_device, cfg->imu_sample_hz);
}
