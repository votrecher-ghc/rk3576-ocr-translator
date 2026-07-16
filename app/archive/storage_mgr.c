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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

int ocr_storage_mgr_init(ocr_storage_mgr_t *mgr, const char *root_dir, int max_percent)
{
    if (!mgr || !root_dir || !*root_dir ||
        strlen(root_dir) >= sizeof(mgr->root_dir)) return -1;
    memset(mgr, 0, sizeof(*mgr));
    strncpy(mgr->root_dir, root_dir, sizeof(mgr->root_dir) - 1);
    mgr->max_percent = (max_percent > 0 && max_percent <= 100) ? max_percent : 90;
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

/*
 * Recursively remove a directory without ever resolving a child outside the
 * already-open parent directory. O_NOFOLLOW closes the lstat/open race for
 * directory entries; unlinkat keeps every mutation anchored to a directory fd.
 */
static int remove_dir_recursive_at(int parent_fd, const char *name)
{
    if (parent_fd < 0 || !name || !*name || strchr(name, '/')) return -1;

    int child_fd = openat(parent_fd, name,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child_fd < 0) return -1;

    DIR *d = fdopendir(child_fd);
    if (!d) {
        int saved_errno = errno;
        close(child_fd);
        errno = saved_errno;
        return -1;
    }

    struct dirent *ent;
    int ret = 0;
    errno = 0;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        struct stat st;
        if (fstatat(dirfd(d), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                errno = 0;
                continue;
            }
            ret = -1;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            if (remove_dir_recursive_at(dirfd(d), ent->d_name) != 0) {
                ret = -1;
                break;
            }
        } else if (unlinkat(dirfd(d), ent->d_name, 0) != 0 && errno != ENOENT) {
            ret = -1;
            break;
        }
        errno = 0;
    }
    if (!ent && errno != 0) ret = -1;

    int saved_errno = errno;
    if (closedir(d) != 0 && ret == 0) {
        ret = -1;
        saved_errno = errno;
    }
    if (ret == 0 && unlinkat(parent_fd, name, AT_REMOVEDIR) != 0) {
        ret = -1;
        saved_errno = errno;
    }
    errno = saved_errno;
    return ret;
}

/* 比较函数：按目录名排序（最旧优先） */
static int name_compare(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static int is_archive_date_dir(const char *name)
{
    if (!name || strlen(name) != 8) return 0;
    for (int i = 0; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') return 0;
    }
    return 1;
}

int ocr_storage_mgr_cleanup(ocr_storage_mgr_t *mgr, int target_percent)
{
    if (!mgr || target_percent < 0 || target_percent >= 100) return -1;

    int used = 0;
    if (ocr_storage_mgr_check(mgr, &used) != 0) return -2;
    if (used <= target_percent) return 0;

    /* Open the trusted anchor once. Never follow a symlink supplied as root. */
    int root_fd = open(mgr->root_dir,
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) {
        LOG_E("安全打开归档根目录失败: %s", strerror(errno));
        return -3;
    }
    DIR *d = fdopendir(root_fd);
    if (!d) {
        int saved_errno = errno;
        close(root_fd);
        errno = saved_errno;
        LOG_E("打开归档目录失败: %s", strerror(errno));
        return -3;
    }

    size_t capacity = 32;
    size_t dir_count = 0;
    char **dirs = calloc(capacity, sizeof(*dirs));
    if (!dirs) {
        closedir(d);
        return -4;
    }
    char today[9] = {0};
    time_t now = time(NULL);
    struct tm local_now;
    if (now != (time_t)-1 && localtime_r(&now, &local_now)) {
        (void)strftime(today, sizeof(today), "%Y%m%d", &local_now);
    }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        /* The capture thread may be writing today's directory concurrently. */
        if (!is_archive_date_dir(ent->d_name) ||
            (today[0] && strcmp(ent->d_name, today) == 0)) continue;
        struct stat st;
        if (fstatat(dirfd(d), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (dir_count == capacity) {
            size_t new_capacity = capacity * 2;
            char **grown = realloc(dirs, new_capacity * sizeof(*dirs));
            if (!grown) break;
            dirs = grown;
            capacity = new_capacity;
        }
        dirs[dir_count] = strdup(ent->d_name);
        if (!dirs[dir_count]) break;
        dir_count++;
    }
    /* 排序（最旧优先） */
    qsort(dirs, dir_count, sizeof(char *), name_compare);

    /* 逐个删除直到低于目标 */
    int deleted = 0;
    for (size_t i = 0; i < dir_count; i++) {
        if (used <= target_percent) break;
        LOG_I("删除旧归档: %s/%s", mgr->root_dir, dirs[i]);
        if (remove_dir_recursive_at(dirfd(d), dirs[i]) == 0) {
            deleted++;
            ocr_storage_mgr_check(mgr, &used);
        } else {
            LOG_W("安全删除归档 %s 失败: %s", dirs[i], strerror(errno));
        }
    }

    /* 释放 */
    for (size_t i = 0; i < dir_count; i++) free(dirs[i]);
    free(dirs);
    closedir(d); /* also closes root_fd */

    LOG_I("存储清理完成: 删除 %d 个归档，当前使用率 %d%%", deleted, used);
    return deleted;
}
