#define _POSIX_C_SOURCE 200809L

/**
 * @file model_loader.c
 * @brief Model registry and on-disk change detection.
 *
 * 模型文件登记与更新检测：记录路径、大小和修改时间，供上层决定何时重新加载。
 * 本模块不创建 RKNN 上下文，查询结果属于 loader，销毁或修改登记后不应继续引用。
 */
#include "model_loader.h"
#include "timestamp.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int valid_model_type(ocr_model_type_t type)
{
    return type >= MODEL_DET && type <= MODEL_TRANSLATE_DEC;
}

static void stat_mtime(const struct stat *st, int64_t *sec, int64_t *nsec)
{
#if defined(_WIN32)
    *sec = (int64_t)st->st_mtime;
    *nsec = 0;
#elif defined(__APPLE__)
    *sec = (int64_t)st->st_mtimespec.tv_sec;
    *nsec = (int64_t)st->st_mtimespec.tv_nsec;
#else
    *sec = (int64_t)st->st_mtim.tv_sec;
    *nsec = (int64_t)st->st_mtim.tv_nsec;
#endif
}

static int validate_regular_file(const char *path, struct stat *st)
{
    FILE *fp;
    unsigned char probe;

    if (!path || path[0] == '\0' || !st) return -EINVAL;
    if (stat(path, st) != 0) return errno ? -errno : -ENOENT;
    if (!S_ISREG(st->st_mode) || st->st_size <= 0) return -EINVAL;

    fp = fopen(path, "rb");
    if (!fp) return errno ? -errno : -EACCES;
    if (fread(&probe, 1, 1, fp) != 1) {
        fclose(fp);
        return -EIO;
    }
    if (fclose(fp) != 0) return -EIO;
    return 0;
}

/** @brief 支持绝对路径或与模型目录组合的相对路径，拒绝路径截断。 */
static int build_model_path(const ocr_model_loader_t *loader,
                            const char *path, char *out, size_t out_size)
{
    int written;

    if (!loader || !path || !out || out_size == 0 || path[0] == '\0') {
        return -EINVAL;
    }

    if (path[0] == '/' || path[0] == '\\' ||
        (((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')) {
        written = snprintf(out, out_size, "%s", path);
    } else if (loader->model_dir[0] != '\0') {
        size_t len = strlen(loader->model_dir);
        const char *sep = (len > 0 &&
                           (loader->model_dir[len - 1] == '/' ||
                            loader->model_dir[len - 1] == '\\')) ? "" : "/";
        written = snprintf(out, out_size, "%s%s%s", loader->model_dir, sep, path);
    } else {
        written = snprintf(out, out_size, "%s", path);
    }

    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
}

int model_loader_init(ocr_model_loader_t *loader, const char *model_dir)
{
    size_t len;

    if (!loader || !model_dir) return -EINVAL;
    len = strlen(model_dir);
    if (len >= sizeof(loader->model_dir)) return -ENAMETOOLONG;

    memset(loader, 0, sizeof(*loader));
    memcpy(loader->model_dir, model_dir, len + 1);
    return 0;
}

/** @brief 验证普通模型文件并记录元数据，后续更新检查比较同一登记项。 */
int model_loader_register(ocr_model_loader_t *loader, ocr_model_type_t type,
                          const char *path, const char *version)
{
    struct stat st;
    char resolved[sizeof(loader->entries[0].path)];
    int ret;

    if (!loader || !path || !valid_model_type(type)) return -EINVAL;
    if (loader->count < 0 || loader->count >= MODEL_MAX) return -ENOSPC;
    if (version && strlen(version) >= sizeof(loader->entries[0].version)) {
        return -ENAMETOOLONG;
    }
    for (int i = 0; i < loader->count; ++i) {
        if (loader->entries[i].type == type) {
            LOG_E("model type %d is already registered", type);
            return -EEXIST;
        }
    }

    ret = build_model_path(loader, path, resolved, sizeof(resolved));
    if (ret != 0) return ret;
    ret = validate_regular_file(resolved, &st);
    if (ret != 0) {
        LOG_E("invalid model file: %s", resolved);
        return ret;
    }

    ocr_model_entry_t *entry = &loader->entries[loader->count];
    memset(entry, 0, sizeof(*entry));
    entry->type = type;
    memcpy(entry->path, resolved, strlen(resolved) + 1);
    if (version) memcpy(entry->version, version, strlen(version) + 1);
    entry->file_size = (uint64_t)st.st_size;
    stat_mtime(&st, &entry->file_mtime_sec, &entry->file_mtime_nsec);
    entry->load_time = timestamp_ms();
    entry->loaded = 0;
    ++loader->count;

    LOG_I("model registered: type=%d path=%s size=%llu version=%s",
          type, entry->path, (unsigned long long)entry->file_size,
          entry->version);
    return 0;
}

const ocr_model_entry_t *model_loader_query(ocr_model_loader_t *loader,
                                            ocr_model_type_t type)
{
    if (!loader || !valid_model_type(type) || loader->count < 0 ||
        loader->count > MODEL_MAX) {
        return NULL;
    }
    for (int i = 0; i < loader->count; ++i) {
        if (loader->entries[i].type == type) return &loader->entries[i];
    }
    return NULL;
}

/** @brief 只报告文件大小或修改时间变化，不在这里替换正在运行的模型。 */
int model_loader_check_update(ocr_model_loader_t *loader,
                              ocr_model_type_t type)
{
    const ocr_model_entry_t *entry = model_loader_query(loader, type);
    struct stat st;
    int64_t sec;
    int64_t nsec;
    int ret;

    if (!entry) return -ENOENT;
    ret = validate_regular_file(entry->path, &st);
    if (ret != 0) return ret;
    stat_mtime(&st, &sec, &nsec);

    return (uint64_t)st.st_size != entry->file_size ||
           sec != entry->file_mtime_sec || nsec != entry->file_mtime_nsec;
}

void model_loader_destroy(ocr_model_loader_t *loader)
{
    if (loader) memset(loader, 0, sizeof(*loader));
}
