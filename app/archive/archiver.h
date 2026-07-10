#ifndef OCR_ARCHIVE_ARCHIVER_H
#define OCR_ARCHIVE_ARCHIVER_H
/**
 * @file archiver.h
 * @brief 归档管理：按时间戳创建目录，保存 image.jpg/result.json/result.txt
 */

#include <stdint.h>
#include <cjson/cJSON.h>

#define ARCHIVE_PATH_LEN 512
#define ARCHIVE_MAX_TEXT 4096

/** 单次归档结果 */
typedef struct {
    char     dir[ARCHIVE_PATH_LEN];      /* 归档目录 */
    char     image_path[ARCHIVE_PATH_LEN];/* image.jpg 路径 */
    char     json_path[ARCHIVE_PATH_LEN]; /* result.json 路径 */
    char     txt_path[ARCHIVE_PATH_LEN];  /* result.txt 路径 */
    uint64_t timestamp;                   /* 归档时间戳 */
    char     src_lang[16];                /* 源语言 */
    char     tgt_lang[16];                /* 目标语言 */
} ocr_archive_entry_t;

/** 归档器上下文 */
typedef struct {
    char root_dir[ARCHIVE_PATH_LEN]; /* 归档根目录 */
    int  count;                      /* 累计归档数 */
} ocr_archiver_t;

/**
 * @brief 初始化归档器
 * @param[in] arch     归档器
 * @param[in] root_dir 归档根目录
 * @return 0=成功，负数=错误
 */
int ocr_archiver_init(ocr_archiver_t *arch, const char *root_dir);

/**
 * @brief 创建一次归档（按时间戳创建子目录）
 * @param[out] entry 归档条目（含生成的目录路径）
 * @return 0=成功，负数=错误
 */
int ocr_archiver_create(ocr_archiver_t *arch, ocr_archive_entry_t *entry);

/**
 * @brief 保存文本结果（result.txt）
 * @param[in] entry 归档条目
 * @param[in] text  原文文本
 * @param[in] trans 翻译文本
 * @return 0=成功，负数=错误
 */
int ocr_archiver_save_text(ocr_archive_entry_t *entry, const char *text, const char *trans);

/**
 * @brief 保存 JSON 结果（result.json，含框坐标+原文+译文）
 * @param[in] entry    归档条目
 * @param[in] boxes    文本框列表
 * @param[in] texts    原文数组
 * @param[in] trans    译文数组
 * @param[in] count    条目数
 * @return 0=成功，负数=错误
 */
int ocr_archiver_save_json(ocr_archive_entry_t *entry, const void *boxes,
                           char **texts, char **trans, int count);

/**
 * @brief 保存图像（image.jpg，从已编码的 JPEG 数据写入）
 */
int ocr_archiver_save_image(ocr_archive_entry_t *entry, const uint8_t *jpeg_data, size_t size);

/**
 * @brief 销毁归档器
 */
void ocr_archiver_destroy(ocr_archiver_t *arch);

#endif /* OCR_ARCHIVE_ARCHIVER_H */
