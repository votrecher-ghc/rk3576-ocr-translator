/**
 * @file archiver.c
 * @brief 归档管理实现
 */
#include "archiver.h"
#include "timestamp.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* 递归创建目录 */
static int mkdir_p(const char *path)
{
    char tmp[ARCHIVE_PATH_LEN];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                LOG_W("mkdir %s 失败: %s", tmp, strerror(errno));
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        LOG_W("mkdir %s 失败: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

int ocr_archiver_init(ocr_archiver_t *arch, const char *root_dir)
{
    if (!arch || !root_dir) return -1;
    memset(arch, 0, sizeof(*arch));
    strncpy(arch->root_dir, root_dir, sizeof(arch->root_dir) - 1);
    mkdir_p(root_dir);
    LOG_I("归档器初始化: %s", root_dir);
    return 0;
}

int ocr_archiver_create(ocr_archiver_t *arch, ocr_archive_entry_t *entry)
{
    if (!arch || !entry) return -1;
    memset(entry, 0, sizeof(*entry));

    uint64_t ts_ms = timestamp_ms();
    entry->timestamp = ts_ms;

    /* 按日期创建子目录：root/YYYYMMDD/HHMMSS_mmm */
    time_t sec = (time_t)(ts_ms / 1000);
    struct tm tm;
    localtime_r(&sec, &tm);

    snprintf(entry->dir, sizeof(entry->dir), "%s/%04d%02d%02d/%02d%02d%02d_%03d",
             arch->root_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts_ms % 1000));

    if (mkdir_p(entry->dir) != 0) return -2;

    snprintf(entry->image_path, sizeof(entry->image_path), "%s/image.jpg", entry->dir);
    snprintf(entry->json_path,  sizeof(entry->json_path),  "%s/result.json", entry->dir);
    snprintf(entry->txt_path,   sizeof(entry->txt_path),   "%s/result.txt", entry->dir);

    arch->count++;
    return 0;
}

int ocr_archiver_save_text(ocr_archive_entry_t *entry, const char *text, const char *trans)
{
    if (!entry || !entry->txt_path[0]) return -1;
    FILE *fp = fopen(entry->txt_path, "w");
    if (!fp) {
        LOG_E("创建 %s 失败: %s", entry->txt_path, strerror(errno));
        return -2;
    }
    fprintf(fp, "=== 原文 ===\n%s\n\n=== 译文 ===\n%s\n", text ? text : "", trans ? trans : "");
    fclose(fp);
    return 0;
}

int ocr_archiver_save_json(ocr_archive_entry_t *entry, const void *boxes,
                           char **texts, char **trans, int count)
{
    if (!entry || !entry->json_path[0]) return -1;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "timestamp", (double)entry->timestamp);
    cJSON_AddStringToObject(root, "src_lang", entry->src_lang);
    cJSON_AddStringToObject(root, "tgt_lang", entry->tgt_lang);

    cJSON *arr = cJSON_AddArrayToObject(root, "results");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        /* TODO: 添加文本框坐标（需访问 ocr_text_box_t，但本模块不应依赖 ai 模块，
         * 通过 boxes 参数以 void* 传入，由调用方保证类型） */
        if (texts && texts[i]) cJSON_AddStringToObject(item, "src", texts[i]);
        if (trans && trans[i]) cJSON_AddStringToObject(item, "tgt", trans[i]);
        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_Print(root);
    FILE *fp = fopen(entry->json_path, "w");
    if (fp) {
        fputs(json_str, fp);
        fclose(fp);
    } else {
        LOG_E("创建 %s 失败: %s", entry->json_path, strerror(errno));
    }
    free(json_str);
    cJSON_Delete(root);
    (void)boxes;
    return 0;
}

int ocr_archiver_save_image(ocr_archive_entry_t *entry, const uint8_t *jpeg_data, size_t size)
{
    if (!entry || !jpeg_data || size == 0) return -1;
    FILE *fp = fopen(entry->image_path, "wb");
    if (!fp) {
        LOG_E("创建 %s 失败: %s", entry->image_path, strerror(errno));
        return -2;
    }
    fwrite(jpeg_data, 1, size, fp);
    fclose(fp);
    return 0;
}

void ocr_archiver_destroy(ocr_archiver_t *arch)
{
    if (!arch) return;
    LOG_I("归档器销毁: 累计归档 %d 次", arch->count);
    memset(arch, 0, sizeof(*arch));
}
