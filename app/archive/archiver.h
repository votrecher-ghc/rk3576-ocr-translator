#ifndef OCR_ARCHIVE_ARCHIVER_H
#define OCR_ARCHIVE_ARCHIVER_H
/**
 * @file archiver.h
 * @brief 归档管理：按时间戳创建目录，保存 image.jpg/result.json/result.txt
 *
 * 拍照归档：创建日期/会话目录，保存原图 JPEG、识别/翻译文本以及文本框 JSON。
 * 目录路径保存在归档条目中，后续写入函数复用该条目；错误由调用方决定如何恢复。
 */

#include <stdint.h>
#include <stddef.h>

#define ARCHIVE_PATH_LEN 512
#define ARCHIVE_MAX_TEXT 4096

/** 与 AI 模块解耦的四点文本框序列化格式。 */
typedef struct {
    float x[4]; /**< 归档图像中四个文本框顶点的横坐标，像素，与检测框同顺序。 */
    float y[4]; /**< 归档图像中四个顶点的纵坐标，像素，与同索引 x 配对。 */
    float score; /**< 检测框置信度，写入 result.json；不是识别字符的概率。 */
    int   valid; /**< 文本框有效标记，随检测结果复制到归档元数据。 */
} ocr_archive_box_t;

/** 单次归档结果 */
typedef struct {
    char     dir[ARCHIVE_PATH_LEN];      /* 归档目录 */
    char     image_path[ARCHIVE_PATH_LEN];/* image.jpg 路径 */
    char     json_path[ARCHIVE_PATH_LEN]; /* result.json 路径 */
    char     txt_path[ARCHIVE_PATH_LEN];  /* result.txt 路径 */
    uint64_t timestamp; /**< 创建本次归档时的 CLOCK_REALTIME 毫秒值，不能直接与单调采集纳秒比较。 */
    char     src_lang[16]; /**< 本次归档记录的源语言代码，含 NUL，例如 en；不用于在此处选择模型。 */
    char     tgt_lang[16]; /**< 本次归档记录的目标语言代码，含 NUL，例如 zh。 */
} ocr_archive_entry_t;

/** 归档器上下文 */
typedef struct {
    char root_dir[ARCHIVE_PATH_LEN]; /* 归档根目录 */
    int  count; /**< 本进程本归档器累计创建次数，参与生成唯一目录后缀；不扫描统计磁盘既有归档。 */
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
 * @param[in] boxes    ocr_archive_box_t 数组（可为 NULL）
 * @param[in] texts    原文数组
 * @param[in] trans    译文数组
 * @param[in] count    条目数
 * @return 0=成功，负数=错误
 */
int ocr_archiver_save_json(ocr_archive_entry_t *entry, const ocr_archive_box_t *boxes,
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
