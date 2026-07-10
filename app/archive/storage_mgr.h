#ifndef OCR_ARCHIVE_STORAGE_MGR_H
#define OCR_ARCHIVE_STORAGE_MGR_H
/**
 * @file storage_mgr.h
 * @brief 存储管理：statfs 检查容量，>阈值按最旧优先删除
 */

#include <stdint.h>

/** 存储管理器上下文 */
typedef struct {
    char root_dir[512];  /* 归档根目录 */
    int  max_percent;    /* 容量告警阈值（百分比，如 90） */
} ocr_storage_mgr_t;

/**
 * @brief 初始化存储管理器
 * @param[in] mgr        存储管理器
 * @param[in] root_dir   归档根目录
 * @param[in] max_percent 容量告警阈值
 * @return 0=成功，负数=错误
 */
int ocr_storage_mgr_init(ocr_storage_mgr_t *mgr, const char *root_dir, int max_percent);

/**
 * @brief 检查存储容量使用率
 * @param[out] used_percent 已用百分比
 * @return 0=成功，负数=错误
 */
int ocr_storage_mgr_check(ocr_storage_mgr_t *mgr, int *used_percent);

/**
 * @brief 清理旧归档（当使用率超过阈值时，按最旧优先删除）
 * @param[in] mgr 存储管理器
 * @param[in] target_percent 清理目标百分比
 * @return 删除的归档数，负数=错误
 */
int ocr_storage_mgr_cleanup(ocr_storage_mgr_t *mgr, int target_percent);

#endif /* OCR_ARCHIVE_STORAGE_MGR_H */
