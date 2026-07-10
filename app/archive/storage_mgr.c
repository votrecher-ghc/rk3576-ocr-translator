/**
 * @file storage_mgr.c
 * @brief 存储管理实现
 */
#include "storage_mgr.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

int ocr_storage_mgr_init(ocr_storage_mgr_t *mgr, const char *root_dir, int max_percent)
{
    if (!mgr || !root_dir) return -1;
    memset(mgr, 0, sizeof(*mgr));
    strncpy(mgr->root_dir, root_dir, sizeof(mgr->root_dir) - 1);
    mgr->max_percent = (max_percent > 0 && max_percent < 100) ? max_percent : 90;
    return 0;
}

int ocr_storage_mgr_check(ocr_storage_mgr_t *mgr, int *used_percent)
{
    if (!mgr || !used_percent) return -1;

    struct statvfs stat;
    if (statvfs(mgr->root_dir, &stat) != 0) {
        LOG_E("statvfs 失败: %s", strerror(errno));
        return -2;
    }

    if (stat.f_blocks == 0) return -3;
    uint64_t total = (uint64_t)stat.f_blocks * stat.f_frsize;
    uint64_t avail = (uint64_t)stat.f_bavail * stat.f_frsize;
    uint64_t used = total - avail;
    *used_percent = (int)(used * 100 / total);

    LOG_D("存储: total=%lluMB used=%d%%", (unsigned long long)(total / 1048576), *used_percent);
    return 0;
}

/* 递归删除目录 */
static int remove_dir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                remove_dir_recursive(child);
            } else {
                remove(child);
            }
        }
    }
    closedir(d);
    return rmdir(path);
}

/* 比较函数：按目录名排序（最旧优先） */
static int name_compare(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

int ocr_storage_mgr_cleanup(ocr_storage_mgr_t *mgr, int target_percent)
{
    if (!mgr) return -1;

    int used = 0;
    if (ocr_storage_mgr_check(mgr, &used) != 0) return -2;
    if (used <= target_percent) return 0;

    /* 枚举根目录下的日期子目录 */
    DIR *d = opendir(mgr->root_dir);
    if (!d) {
        LOG_E("打开归档目录失败: %s", strerror(errno));
        return -3;
    }

    char *dirs[256];
    int dir_count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && dir_count < 256) {
        if (ent->d_name[0] == '.') continue;
        dirs[dir_count] = strdup(ent->d_name);
        dir_count++;
    }
    closedir(d);

    /* 排序（最旧优先） */
    qsort(dirs, dir_count, sizeof(char *), name_compare);

    /* 逐个删除直到低于目标 */
    int deleted = 0;
    for (int i = 0; i < dir_count; i++) {
        if (used <= target_percent) break;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", mgr->root_dir, dirs[i]);
        LOG_I("删除旧归档: %s", path);
        if (remove_dir_recursive(path) == 0) {
            deleted++;
            ocr_storage_mgr_check(mgr, &used);
        }
    }

    /* 释放 */
    for (int i = 0; i < dir_count; i++) free(dirs[i]);

    LOG_I("存储清理完成: 删除 %d 个归档，当前使用率 %d%%", deleted, used);
    return deleted;
}
