/**
 * @file archiver.c
 * @brief 归档管理实现
 */
#include "archiver.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <cjson/cJSON.h>

/* 递归创建目录 */
static int mkdir_p(const char *path)
{
    char tmp[ARCHIVE_PATH_LEN];
    struct stat st;
    if (!path || !*path || strlen(path) >= sizeof(tmp)) return -1;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0) {
                if (errno != EEXIST || stat(tmp, &st) != 0 ||
                    !S_ISDIR(st.st_mode)) {
                    LOG_E("mkdir %s 失败: %s", tmp, strerror(errno));
                    return -1;
                }
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0) {
        if (errno != EEXIST || stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
            LOG_W("mkdir %s 失败: %s", tmp, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int ocr_archiver_init(ocr_archiver_t *arch, const char *root_dir)
{
    if (!arch || !root_dir || !*root_dir) return -1;
    size_t root_len = strlen(root_dir);
    if (root_len >= sizeof(arch->root_dir)) {
        LOG_E("归档根目录过长（最大 %zu 字节）", sizeof(arch->root_dir) - 1U);
        return -ENAMETOOLONG;
    }
    memset(arch, 0, sizeof(*arch));
    memcpy(arch->root_dir, root_dir, root_len + 1U);
    if (mkdir_p(root_dir) != 0) return -2;
    LOG_I("归档器初始化: %s", root_dir);
    return 0;
}

int ocr_archiver_create(ocr_archiver_t *arch, ocr_archive_entry_t *entry)
{
    if (!arch || !entry) return -1;
    memset(entry, 0, sizeof(*entry));

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -2;
    uint64_t ts_ms = (uint64_t)now.tv_sec * 1000ULL +
                     (uint64_t)now.tv_nsec / 1000000ULL;
    entry->timestamp = ts_ms;

    /* 按日期创建子目录：root/YYYYMMDD/HHMMSS_mmm */
    time_t sec = now.tv_sec;
    struct tm tm;
    localtime_r(&sec, &tm);

    int n = snprintf(entry->dir, sizeof(entry->dir),
                     "%s/%04d%02d%02d/%02d%02d%02d_%03d_%ld_%06d",
                     arch->root_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts_ms % 1000),
                     (long)getpid(), arch->count);
    if (n < 0 || (size_t)n >= sizeof(entry->dir)) return -3;

    if (mkdir_p(entry->dir) != 0) return -4;

    if (snprintf(entry->image_path, sizeof(entry->image_path), "%s/image.jpg", entry->dir) >=
            (int)sizeof(entry->image_path) ||
        snprintf(entry->json_path, sizeof(entry->json_path), "%s/result.json", entry->dir) >=
            (int)sizeof(entry->json_path) ||
        snprintf(entry->txt_path, sizeof(entry->txt_path), "%s/result.txt", entry->dir) >=
            (int)sizeof(entry->txt_path)) {
        return -5;
    }

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
    int ret = fprintf(fp, "=== 原文 ===\n%s\n\n=== 译文 ===\n%s\n",
                      text ? text : "", trans ? trans : "") < 0 ? -3 : 0;
    if (fclose(fp) != 0) ret = -3;
    return ret;
}

int ocr_archiver_save_json(ocr_archive_entry_t *entry, const ocr_archive_box_t *boxes,
                           char **texts, char **trans, int count)
{
    if (!entry || !entry->json_path[0] || count < 0) return -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) return -2;
    cJSON_AddNumberToObject(root, "timestamp", (double)entry->timestamp);
    cJSON_AddStringToObject(root, "src_lang", entry->src_lang);
    cJSON_AddStringToObject(root, "tgt_lang", entry->tgt_lang);

    cJSON *arr = cJSON_AddArrayToObject(root, "results");
    if (!arr) {
        cJSON_Delete(root);
        return -2;
    }
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            return -2;
        }
        if (boxes) {
            cJSON *points = cJSON_AddArrayToObject(item, "box");
            for (int p = 0; points && p < 4; ++p) {
                cJSON *point = cJSON_CreateArray();
                if (!point) continue;
                cJSON_AddItemToArray(point, cJSON_CreateNumber(boxes[i].x[p]));
                cJSON_AddItemToArray(point, cJSON_CreateNumber(boxes[i].y[p]));
                cJSON_AddItemToArray(points, point);
            }
            cJSON_AddNumberToObject(item, "score", boxes[i].score);
        }
        if (texts && texts[i]) cJSON_AddStringToObject(item, "src", texts[i]);
        if (trans && trans[i]) cJSON_AddStringToObject(item, "tgt", trans[i]);
        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        return -2;
    }
    FILE *fp = fopen(entry->json_path, "w");
    int ret = 0;
    if (fp) {
        if (fputs(json_str, fp) == EOF) ret = -3;
        if (fclose(fp) != 0) ret = -3;
    } else {
        LOG_E("创建 %s 失败: %s", entry->json_path, strerror(errno));
        ret = -3;
    }
    free(json_str);
    cJSON_Delete(root);
    return ret;
}

int ocr_archiver_save_image(ocr_archive_entry_t *entry, const uint8_t *jpeg_data, size_t size)
{
    if (!entry || !jpeg_data || size == 0) return -1;
    FILE *fp = fopen(entry->image_path, "wb");
    if (!fp) {
        LOG_E("创建 %s 失败: %s", entry->image_path, strerror(errno));
        return -2;
    }
    int ret = fwrite(jpeg_data, 1, size, fp) == size ? 0 : -3;
    if (fclose(fp) != 0) ret = -3;
    return ret;
}

void ocr_archiver_destroy(ocr_archiver_t *arch)
{
    if (!arch) return;
    LOG_I("归档器销毁: 累计归档 %d 次", arch->count);
    memset(arch, 0, sizeof(*arch));
}
