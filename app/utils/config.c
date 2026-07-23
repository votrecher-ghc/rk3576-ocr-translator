/**
 * @file config.c
 * @brief JSON 配置解析实现（基于 cJSON）
 */
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <math.h>

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

/* Path-like fields must never silently name a different object after truncation. */
static int cfg_path(char *dst, size_t dst_size, const cJSON *obj,
                    const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item || !cJSON_IsString(item) || !item->valuestring) return 0;
    size_t length = strlen(item->valuestring);
    if (length >= dst_size) return -1;
    memcpy(dst, item->valuestring, length + 1U);
    return 0;
}

/* 将 cJSON 整型字段写入目标 */
static void cfg_int(int *dst, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item)) {
        *dst = item->valueint;
    }
}

static void cfg_u32(uint32_t *dst, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item) && item->valuedouble >= 0.0 &&
        item->valuedouble <= (double)UINT32_MAX) {
        *dst = (uint32_t)item->valuedouble;
    }
}

static void cfg_float(float *dst, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item) && isfinite(item->valuedouble)) {
        *dst = (float)item->valuedouble;
    }
}

static void cfg_bool(int *dst, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) return;
    if (cJSON_IsBool(item)) *dst = cJSON_IsTrue(item) ? 1 : 0;
    else if (cJSON_IsNumber(item)) *dst = item->valueint != 0;
}

static int cfg_log_level(const cJSON *item, int fallback)
{
    if (!item) return fallback;
    if (cJSON_IsNumber(item)) return item->valueint;
    if (!cJSON_IsString(item) || !item->valuestring) return fallback;
    if (strcasecmp(item->valuestring, "debug") == 0) return 0;
    if (strcasecmp(item->valuestring, "info") == 0)  return 1;
    if (strcasecmp(item->valuestring, "warn") == 0 ||
        strcasecmp(item->valuestring, "warning") == 0) return 2;
    if (strcasecmp(item->valuestring, "error") == 0) return 3;
    if (strcasecmp(item->valuestring, "fatal") == 0) return 4;
    return fallback;
}

static int cfg_run_mode(const cJSON *item, int fallback)
{
    if (!item) return fallback;
    if (cJSON_IsNumber(item)) return item->valueint == 1 ? 1 : 0;
    if (!cJSON_IsString(item) || !item->valuestring) return fallback;
    if (strcasecmp(item->valuestring, "photo") == 0 ||
        strcasecmp(item->valuestring, "capture") == 0) return 1;
    if (strcasecmp(item->valuestring, "live") == 0 ||
        strcasecmp(item->valuestring, "realtime") == 0) return 0;
    return fallback;
}

static void config_defaults(ocr_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->app_name, sizeof(cfg->app_name), "%s", "ocr_translator");
    cfg->run_mode = 0;
    cfg->log_level = 1;
    snprintf(cfg->v4l2_device, sizeof(cfg->v4l2_device), "%s", "/dev/video0");
    cfg->capture_width = 1920;
    cfg->capture_height = 1080;
    cfg->capture_fps = 30;
    snprintf(cfg->capture_format, sizeof(cfg->capture_format), "%s", "NV12");
    cfg->capture_buffer_count = 4;
    snprintf(cfg->drm_device, sizeof(cfg->drm_device), "%s", "/dev/dri/card0");
    cfg->display_width = 1920;
    cfg->display_height = 1080;
    snprintf(cfg->det_model, sizeof(cfg->det_model), "%s",
             "/usr/share/ocr/models/ppocrv4_det_int8.rknn");
    snprintf(cfg->rec_model, sizeof(cfg->rec_model), "%s",
             "/usr/share/ocr/models/ppocrv4_rec_fp16.rknn");
    snprintf(cfg->tokenizer_vocab, sizeof(cfg->tokenizer_vocab), "%s",
             "/usr/share/ocr/models/ppocr_keys_v1.txt");
    snprintf(cfg->rec_vocab, sizeof(cfg->rec_vocab), "%s",
             "/usr/share/ocr/models/ppocr_keys_v1.txt");
    snprintf(cfg->trans_encoder_model, sizeof(cfg->trans_encoder_model), "%s",
             "/usr/share/ocr/models/lite_transformer_encoder_fp16.rknn");
    snprintf(cfg->trans_decoder_model, sizeof(cfg->trans_decoder_model), "%s",
             "/usr/share/ocr/models/lite_transformer_decoder_fp16.rknn");
    snprintf(cfg->trans_manifest, sizeof(cfg->trans_manifest), "%s",
             "/usr/share/ocr/models/translation_manifest.json");
    snprintf(cfg->trans_src_vocab, sizeof(cfg->trans_src_vocab), "%s",
             "/usr/share/ocr/models/translation_src_vocab.txt");
    snprintf(cfg->trans_tgt_vocab, sizeof(cfg->trans_tgt_vocab), "%s",
             "/usr/share/ocr/models/translation_tgt_vocab.txt");
    cfg->det_threshold = 0.3f;
    cfg->det_box_threshold = 0.6f;
    cfg->allow_ocr_only = 0;
    snprintf(cfg->src_lang, sizeof(cfg->src_lang), "%s", "en");
    snprintf(cfg->tgt_lang, sizeof(cfg->tgt_lang), "%s", "zh");
    snprintf(cfg->font_path, sizeof(cfg->font_path), "%s",
             "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc");
    cfg->font_size = 32;
    snprintf(cfg->archive_dir, sizeof(cfg->archive_dir), "%s", "/data/ocr");
    cfg->archive_max_percent = 90;
    cfg->jpeg_quality = 90;
    snprintf(cfg->imu_device, sizeof(cfg->imu_device), "%s", "auto");
    cfg->imu_sample_hz = 1000;
    cfg->stab_alpha = 0.1f;
    cfg->madgwick_beta = 0.1f;
    cfg->stab_crop_ratio = 0.9f;
    snprintf(cfg->light_iio_device, sizeof(cfg->light_iio_device), "%s", "auto");
    snprintf(cfg->temp_iio_device, sizeof(cfg->temp_iio_device), "%s", "auto");
    snprintf(cfg->key_device, sizeof(cfg->key_device), "%s", "auto");
    snprintf(cfg->backlight_path, sizeof(cfg->backlight_path), "%s",
             "/sys/class/backlight/panel-backlight/brightness");
    snprintf(cfg->pwm_path, sizeof(cfg->pwm_path), "%s", "kernel");
}

int config_load(ocr_config_t *cfg, const char *path)
{
    int path_too_long = 0;

    if (!cfg || !path) {
        return -1;
    }
    config_defaults(cfg);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_E("无法打开配置文件: %s", path);
        return -2;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -3;
    }
    long fsize = ftell(fp);
    if (fsize < 0 || fsize > 16 * 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -3;
    }

    char *buf = (char *)malloc(fsize + 1);
    if (!buf) {
        fclose(fp);
        return -4;
    }

    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    if (nread != (size_t)fsize && ferror(fp)) {
        free(buf);
        fclose(fp);
        return -5;
    }
    buf[nread] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        LOG_E("配置文件 JSON 解析失败: %s", path);
        return -6;
    }

    /* 兼容旧版顶层字段。 */
    cfg_strcpy(cfg->app_name, sizeof(cfg->app_name), root, "app_name");
    cfg->run_mode = cfg_run_mode(cJSON_GetObjectItemCaseSensitive(root, "run_mode"),
                                 cfg->run_mode);
    cfg->log_level = cfg_log_level(cJSON_GetObjectItemCaseSensitive(root, "log_level"),
                                   cfg->log_level);
    cfg_strcpy(cfg->log_path, sizeof(cfg->log_path), root, "log_path");

    /* 当前配置文件使用 app 子对象。 */
    const cJSON *app = cJSON_GetObjectItemCaseSensitive(root, "app");
    if (app && cJSON_IsObject(app)) {
        cfg_strcpy(cfg->app_name, sizeof(cfg->app_name), app, "name");
        cfg->run_mode = cfg_run_mode(cJSON_GetObjectItemCaseSensitive(app, "mode"),
                                     cfg->run_mode);
        cfg->log_level = cfg_log_level(cJSON_GetObjectItemCaseSensitive(app, "log_level"),
                                       cfg->log_level);
        cfg_strcpy(cfg->log_path, sizeof(cfg->log_path), app, "log_file");
    }

    /* 采集 */
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(root, "capture");
    if (cap && cJSON_IsObject(cap)) {
        cfg_strcpy(cfg->v4l2_device, sizeof(cfg->v4l2_device), cap, "device");
        cfg_u32(&cfg->capture_width, cap, "width");
        cfg_u32(&cfg->capture_height, cap, "height");
        cfg_u32(&cfg->capture_fps, cap, "fps");
        cfg_strcpy(cfg->capture_format, sizeof(cfg->capture_format), cap, "format");
        cfg_int(&cfg->capture_buffer_count, cap, "buffer_count");
    }

    /* 显示 */
    const cJSON *disp = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (disp && cJSON_IsObject(disp)) {
        cfg_strcpy(cfg->drm_device, sizeof(cfg->drm_device), disp, "device");
        cfg_u32(&cfg->display_width, disp, "display_width");
        cfg_u32(&cfg->display_height, disp, "display_height");
        cfg_u32(&cfg->display_width, disp, "width");
        cfg_u32(&cfg->display_height, disp, "height");
    }

    /* AI 模型 */
    const cJSON *ai = cJSON_GetObjectItemCaseSensitive(root, "ai");
    if (ai && cJSON_IsObject(ai)) {
        cfg_strcpy(cfg->det_model, sizeof(cfg->det_model), ai, "det_model");
        cfg_strcpy(cfg->rec_model, sizeof(cfg->rec_model), ai, "rec_model");
        cfg_strcpy(cfg->trans_model, sizeof(cfg->trans_model), ai, "trans_model");
        cfg_strcpy(cfg->tokenizer_vocab, sizeof(cfg->tokenizer_vocab), ai, "vocab");
        cfg_strcpy(cfg->rec_vocab, sizeof(cfg->rec_vocab), ai, "vocab");
        cfg_strcpy(cfg->rec_vocab, sizeof(cfg->rec_vocab), ai, "rec_vocab");
        cfg_strcpy(cfg->trans_encoder_model, sizeof(cfg->trans_encoder_model),
                   ai, "trans_encoder_model");
        cfg_strcpy(cfg->trans_decoder_model, sizeof(cfg->trans_decoder_model),
                   ai, "trans_decoder_model");
        cfg_strcpy(cfg->trans_manifest, sizeof(cfg->trans_manifest),
                   ai, "trans_manifest");
        cfg_strcpy(cfg->trans_src_vocab, sizeof(cfg->trans_src_vocab), ai, "src_vocab");
        cfg_strcpy(cfg->trans_tgt_vocab, sizeof(cfg->trans_tgt_vocab), ai, "tgt_vocab");
        cfg_float(&cfg->det_threshold, ai, "det_threshold");
        cfg_float(&cfg->det_box_threshold, ai, "det_box_threshold");
        cfg_bool(&cfg->allow_ocr_only, ai, "allow_ocr_only");
        cfg_strcpy(cfg->src_lang, sizeof(cfg->src_lang), ai, "src_lang");
        cfg_strcpy(cfg->tgt_lang, sizeof(cfg->tgt_lang), ai, "tgt_lang");
        cfg_strcpy(cfg->tgt_lang, sizeof(cfg->tgt_lang), ai, "dst_lang");
    }

    /* 渲染字体 */
    const cJSON *rend = cJSON_GetObjectItemCaseSensitive(root, "render");
    if (rend && cJSON_IsObject(rend)) {
        cfg_strcpy(cfg->font_path, sizeof(cfg->font_path), rend, "font_path");
        cfg_int(&cfg->font_size, rend, "font_size");
    }

    /* 归档 */
    const cJSON *arch = cJSON_GetObjectItemCaseSensitive(root, "archive");
    if (arch && cJSON_IsObject(arch)) {
        cfg_strcpy(cfg->archive_dir, sizeof(cfg->archive_dir), arch, "dir");
        cfg_strcpy(cfg->archive_dir, sizeof(cfg->archive_dir), arch, "base_dir");
        cfg_int(&cfg->archive_max_percent, arch, "max_percent");
        cfg_int(&cfg->archive_max_percent, arch, "max_usage_pct");
        cfg_int(&cfg->jpeg_quality, arch, "jpeg_quality");
    }

    /* IMU/防抖 */
    const cJSON *imu = cJSON_GetObjectItemCaseSensitive(root, "imu");
    if (imu && cJSON_IsObject(imu)) {
        cfg_strcpy(cfg->imu_device, sizeof(cfg->imu_device), imu, "device");
        cfg_int(&cfg->imu_sample_hz, imu, "sample_hz");
        cfg_float(&cfg->stab_alpha, imu, "stab_alpha");
        cfg_float(&cfg->madgwick_beta, imu, "madgwick_beta");
        cfg_float(&cfg->stab_crop_ratio, imu, "crop_ratio");
    }

    const cJSON *stab = cJSON_GetObjectItemCaseSensitive(root, "stab");
    if (stab && cJSON_IsObject(stab)) {
        cfg_strcpy(cfg->imu_device, sizeof(cfg->imu_device), stab, "imu_device");
        cfg_int(&cfg->imu_sample_hz, stab, "sample_rate");
        cfg_float(&cfg->stab_alpha, stab, "filter_beta");
        cfg_float(&cfg->madgwick_beta, stab, "madgwick_beta");
        cfg_float(&cfg->stab_crop_ratio, stab, "crop_ratio");
    }

    /* 背光/风扇 */
    const cJSON *hw = cJSON_GetObjectItemCaseSensitive(root, "hardware");
    if (hw && cJSON_IsObject(hw)) {
        if (cfg_path(cfg->backlight_path, sizeof(cfg->backlight_path),
                     hw, "backlight") != 0)
            path_too_long = 1;
        if (cfg_path(cfg->pwm_path, sizeof(cfg->pwm_path), hw, "pwm") != 0)
            path_too_long = 1;
    }

    const cJSON *sensors = cJSON_GetObjectItemCaseSensitive(root, "sensors");
    if (sensors && cJSON_IsObject(sensors)) {
        if (cfg_path(cfg->light_iio_device, sizeof(cfg->light_iio_device),
                     sensors, "ambient_light_device") != 0)
            path_too_long = 1;
        if (cfg_path(cfg->temp_iio_device, sizeof(cfg->temp_iio_device),
                     sensors, "temperature_device") != 0)
            path_too_long = 1;
        if (cfg_path(cfg->backlight_path, sizeof(cfg->backlight_path),
                     sensors, "backlight_path") != 0)
            path_too_long = 1;
        if (cfg_path(cfg->pwm_path, sizeof(cfg->pwm_path),
                     sensors, "pwm_fan_path") != 0)
            path_too_long = 1;
    }

    const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
    if (input && cJSON_IsObject(input)) {
        if (cfg_path(cfg->key_device, sizeof(cfg->key_device), input,
                     "key_device") != 0)
            path_too_long = 1;
    }

    if (path_too_long) {
        cJSON_Delete(root);
        LOG_E("传感器或输入设备路径过长: %s", path);
        return -7;
    }

    if (cfg->capture_width == 0 || cfg->capture_height == 0 || cfg->capture_fps == 0 ||
        cfg->display_width == 0 || cfg->display_height == 0 ||
        (strcasecmp(cfg->capture_format, "NV12") != 0 &&
         strcasecmp(cfg->capture_format, "NV16") != 0 &&
         strcasecmp(cfg->capture_format, "YUYV") != 0) ||
        ((strcasecmp(cfg->capture_format, "NV12") == 0) &&
         ((cfg->capture_width | cfg->capture_height) & 1U)) ||
        ((strcasecmp(cfg->capture_format, "NV16") == 0 ||
          strcasecmp(cfg->capture_format, "YUYV") == 0) &&
         (cfg->capture_width & 1U)) ||
        ((cfg->display_width | cfg->display_height) & 1U) ||
        cfg->capture_buffer_count < 2 || cfg->capture_buffer_count > 8 ||
        cfg->log_level < 0 || cfg->log_level > 4 ||
        cfg->archive_max_percent < 1 || cfg->archive_max_percent > 100 ||
        cfg->jpeg_quality < 1 || cfg->jpeg_quality > 100 ||
        cfg->det_threshold < 0.0f || cfg->det_threshold > 1.0f ||
        cfg->det_box_threshold < 0.0f || cfg->det_box_threshold > 1.0f ||
        cfg->font_size <= 0 || cfg->font_size > 512 ||
        cfg->src_lang[0] == '\0' || cfg->tgt_lang[0] == '\0' ||
        cfg->imu_sample_hz <= 0 || cfg->stab_alpha < 0.0f || cfg->stab_alpha > 1.0f ||
        cfg->madgwick_beta < 0.0f || cfg->madgwick_beta > 10.0f ||
        cfg->stab_crop_ratio <= 0.5f || cfg->stab_crop_ratio > 1.0f) {
        cJSON_Delete(root);
        LOG_E("配置字段超出有效范围: %s", path);
        return -7;
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
    LOG_I("capture=%ux%u@%u %s buffers=%d dev=%s", cfg->capture_width,
          cfg->capture_height, cfg->capture_fps, cfg->capture_format,
          cfg->capture_buffer_count, cfg->v4l2_device);
    LOG_I("display=%ux%u dev=%s", cfg->display_width, cfg->display_height, cfg->drm_device);
    LOG_I("det_model=%s", cfg->det_model);
    LOG_I("rec_model=%s", cfg->rec_model);
    LOG_I("font=%s size=%d", cfg->font_path, cfg->font_size);
    LOG_I("archive=%s max%%=%d", cfg->archive_dir, cfg->archive_max_percent);
    LOG_I("imu=%s hz=%d beta=%.3f crop=%.3f", cfg->imu_device,
          cfg->imu_sample_hz, cfg->madgwick_beta, cfg->stab_crop_ratio);
    LOG_I("light=%s temp=%s key=%s", cfg->light_iio_device,
          cfg->temp_iio_device, cfg->key_device);
    LOG_I("backlight=%s pwm=%s", cfg->backlight_path, cfg->pwm_path);
}
