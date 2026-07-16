#ifndef OCR_DISPLAY_DRM_SINK_H
#define OCR_DISPLAY_DRM_SINK_H

/** @file drm_sink.h @brief Pipeline sink that presents DMA-BUF frames. */

#include "drm_fb.h"
#include "drm_plane.h"
#include "node.h"

#define OCR_DRM_SINK_MAX_FBS 32

typedef struct {
    const ocr_buffer_t *source;
    int source_fd;
    size_t source_size;
    uint32_t width, height;
    ocr_pixel_format_t format;
    uint32_t plane_count;
    uint32_t strides[4];
    uint32_t offsets[4];
    ocr_drm_fb_t fb;
    int valid;
} ocr_drm_sink_entry_t;

typedef struct {
    ocr_drm_device_t *dev;
    ocr_drm_planes_t *planes;
    ocr_drm_sink_entry_t entries[OCR_DRM_SINK_MAX_FBS];
    ocr_buffer_t *current;
    ocr_drm_sink_entry_t *current_entry;
    uint64_t current_frame_id;
    unsigned int next_evict;
    int initialized;
} ocr_drm_sink_t;

int ocr_drm_sink_init(ocr_drm_sink_t *sink, ocr_drm_device_t *dev,
                      ocr_drm_planes_t *planes);
int ocr_drm_sink_present(ocr_drm_sink_t *sink, ocr_buffer_t *buffer);
int ocr_drm_sink_process(ocr_pipeline_node_t *node, ocr_buffer_t *buffer);
void ocr_drm_sink_destroy(ocr_drm_sink_t *sink);

#endif /* OCR_DISPLAY_DRM_SINK_H */
