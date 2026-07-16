#ifndef OCR_AI_MODEL_LOADER_H
#define OCR_AI_MODEL_LOADER_H

/** @file model_loader.h @brief Model registry and update tracking. */

#include <stdint.h>

#include "rknn_runtime.h"

#define MODEL_MAX 8

typedef enum {
    MODEL_DET = 0,
    MODEL_REC,
    MODEL_TRANSLATE_ENC,
    MODEL_TRANSLATE_DEC,
} ocr_model_type_t;

typedef struct {
    ocr_model_type_t type;
    char             path[256];
    char             version[32];
    uint64_t         file_size;
    int64_t          file_mtime_sec;
    int64_t          file_mtime_nsec;
    uint64_t         load_time;
    int              loaded;
} ocr_model_entry_t;

typedef struct {
    ocr_model_entry_t entries[MODEL_MAX];
    int               count;
    char              model_dir[256];
} ocr_model_loader_t;

int model_loader_init(ocr_model_loader_t *loader, const char *model_dir);

int model_loader_register(ocr_model_loader_t *loader, ocr_model_type_t type,
                          const char *path, const char *version);

const ocr_model_entry_t *model_loader_query(ocr_model_loader_t *loader,
                                            ocr_model_type_t type);

/** Return 1 for a size/mtime change, 0 for no change, or a negative error. */
int model_loader_check_update(ocr_model_loader_t *loader,
                              ocr_model_type_t type);

void model_loader_destroy(ocr_model_loader_t *loader);

#endif /* OCR_AI_MODEL_LOADER_H */
