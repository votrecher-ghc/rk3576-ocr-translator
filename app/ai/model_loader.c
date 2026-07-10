/**
 * @file model_loader.c
 * @brief 模型加载/版本管理实现
 */
#include "model_loader.h"
#include "timestamp.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

int model_loader_init(ocr_model_loader_t *loader, const char *model_dir)
{
    if (!loader || !model_dir) return -1;
    memset(loader, 0, sizeof(*loader));
    strncpy(loader->model_dir, model_dir, sizeof(loader->model_dir) - 1);
    return 0;
}

int model_loader_register(ocr_model_loader_t *loader, ocr_model_type_t type,
                          const char *path, const char *version)
{
    if (!loader || !path) return -1;
    if (loader->count >= MODEL_MAX) {
        LOG_E("模型注册数超过上限");
        return -2;
    }

    /* 检查文件是否存在并获取大小 */
    struct stat st;
    if (stat(path, &st) != 0) {
        LOG_E("模型文件不存在: %s", path);
        return -3;
    }

    ocr_model_entry_t *e = &loader->entries[loader->count++];
    e->type = type;
    strncpy(e->path, path, sizeof(e->path) - 1);
    if (version) strncpy(e->version, version, sizeof(e->version) - 1);
    e->file_size = (uint64_t)st.st_size;
    e->load_time = timestamp_ms();
    e->loaded = 0;

    LOG_I("模型注册: type=%d path=%s size=%llu version=%s",
          type, path, (unsigned long long)e->file_size, e->version);
    return 0;
}

const ocr_model_entry_t *model_loader_query(ocr_model_loader_t *loader,
                                            ocr_model_type_t type)
{
    if (!loader) return NULL;
    for (int i = 0; i < loader->count; i++) {
        if (loader->entries[i].type == type) return &loader->entries[i];
    }
    return NULL;
}

int model_loader_check_update(ocr_model_loader_t *loader, ocr_model_type_t type)
{
    const ocr_model_entry_t *e = model_loader_query(loader, type);
    if (!e) return -1;

    struct stat st;
    if (stat(e->path, &st) != 0) return -2;

    if ((uint64_t)st.st_size != e->file_size) return 1;
    /* TODO: 比较 mtime */
    return 0;
}

void model_loader_destroy(ocr_model_loader_t *loader)
{
    if (!loader) return;
    memset(loader, 0, sizeof(*loader));
}
