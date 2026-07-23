/* Unit tests for parsing the checked-in application configuration. */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef OCR_TEST_CONFIG_PATH
#error "OCR_TEST_CONFIG_PATH must point to config/ocr_translator.json"
#endif

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

static void check_rejects_long_device_path(const char *section,
                                           const char *key)
{
    char file_name[] = "/tmp/ocr-config-path-XXXXXX";
    ocr_config_t cfg;
    int fd = mkstemp(file_name);
    CHECK(fd >= 0);
    FILE *file = fdopen(fd, "w");
    CHECK(file != NULL);
    CHECK(fprintf(file, "{\"%s\":{\"%s\":\"", section, key) > 0);
    for (int i = 0; i < 256; ++i) CHECK(fputc('x', file) != EOF);
    CHECK(fputs("\"}}\n", file) != EOF);
    CHECK(fclose(file) == 0);
    CHECK(config_load(&cfg, file_name) < 0);
    CHECK(unlink(file_name) == 0);
}

int main(void)
{
    ocr_config_t cfg;
    CHECK(config_load(&cfg, OCR_TEST_CONFIG_PATH) == 0);

    CHECK(strcmp(cfg.app_name, "ocr_translator") == 0);
    CHECK(cfg.run_mode == 0);
    CHECK(cfg.log_level == 1);
    CHECK(strcmp(cfg.log_path, "/var/log/ocr_translator.log") == 0);

    CHECK(strcmp(cfg.v4l2_device, "/dev/video0") == 0);
    CHECK(cfg.capture_width == 1920);
    CHECK(cfg.capture_height == 1080);
    CHECK(cfg.capture_fps == 30);
    CHECK(strcmp(cfg.capture_format, "NV12") == 0);
    CHECK(cfg.capture_buffer_count == 4);

    CHECK(strcmp(cfg.drm_device, "/dev/dri/card0") == 0);
    CHECK(cfg.display_width == 1920);
    CHECK(cfg.display_height == 1080);

    CHECK(strcmp(cfg.det_model,
                 "/usr/share/ocr/models/ppocrv4_det_int8.rknn") == 0);
    CHECK(strcmp(cfg.rec_model,
                 "/usr/share/ocr/models/ppocrv4_rec_fp16.rknn") == 0);
    CHECK(strcmp(cfg.rec_vocab,
                 "/usr/share/ocr/models/ppocr_keys_v1.txt") == 0);
    CHECK(strcmp(cfg.trans_encoder_model,
                 "/usr/share/ocr/models/lite_transformer_encoder_fp16.rknn") == 0);
    CHECK(strcmp(cfg.trans_decoder_model,
                 "/usr/share/ocr/models/lite_transformer_decoder_fp16.rknn") == 0);
    CHECK(strcmp(cfg.trans_manifest,
                 "/usr/share/ocr/models/translation_manifest.json") == 0);
    CHECK(strcmp(cfg.trans_src_vocab,
                 "/usr/share/ocr/models/translation_src_vocab.txt") == 0);
    CHECK(strcmp(cfg.trans_tgt_vocab,
                 "/usr/share/ocr/models/translation_tgt_vocab.txt") == 0);
    CHECK(cfg.det_threshold > 0.299f && cfg.det_threshold < 0.301f);
    CHECK(cfg.det_box_threshold > 0.599f && cfg.det_box_threshold < 0.601f);
    CHECK(cfg.allow_ocr_only == 0);
    CHECK(strcmp(cfg.src_lang, "en") == 0);
    CHECK(strcmp(cfg.tgt_lang, "zh") == 0);

    CHECK(strcmp(cfg.archive_dir, "/data/ocr") == 0);
    CHECK(cfg.archive_max_percent == 90);
    CHECK(cfg.jpeg_quality == 90);

    CHECK(strcmp(cfg.imu_device, "auto") == 0);
    CHECK(cfg.imu_sample_hz == 1000);
    CHECK(cfg.stab_alpha > 0.099f && cfg.stab_alpha < 0.101f);
    CHECK(cfg.madgwick_beta > 0.099f && cfg.madgwick_beta < 0.101f);
    CHECK(cfg.stab_crop_ratio > 0.899f && cfg.stab_crop_ratio < 0.901f);

    CHECK(strcmp(cfg.light_iio_device, "auto") == 0);
    CHECK(strcmp(cfg.temp_iio_device, "auto") == 0);
    CHECK(strcmp(cfg.key_device, "auto") == 0);

    CHECK(strcmp(cfg.backlight_path,
                 "/sys/class/backlight/panel-backlight/brightness") == 0);
    CHECK(strcmp(cfg.pwm_path, "kernel") == 0);

    config_free(&cfg);
    check_rejects_long_device_path("sensors", "ambient_light_device");
    check_rejects_long_device_path("sensors", "temperature_device");
    check_rejects_long_device_path("input", "key_device");
    check_rejects_long_device_path("sensors", "pwm_fan_path");
    puts("[PASS] checked-in configuration parsing tests");
    return EXIT_SUCCESS;
}
