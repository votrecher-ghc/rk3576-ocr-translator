/** @file drm_sink.c @brief Synchronous VSync-aware DRM pipeline sink. */
#include "drm_sink.h"
#include "log.h"

#include <string.h>
static int entry_matches(const ocr_drm_sink_entry_t *entry,
                         const ocr_buffer_t *buffer)
{
    /* Pool buffer objects and their owned fd remain stable until sink destroy. */
    return entry->valid && entry->source == buffer &&
           entry->source_fd == buffer->fd &&
           entry->source_size == buffer->size &&
           entry->width == buffer->width && entry->height == buffer->height &&
           entry->format == buffer->format &&
           entry->plane_count == buffer->plane_count &&
           memcmp(entry->strides, buffer->strides, sizeof(entry->strides)) == 0 &&
           memcmp(entry->offsets, buffer->offsets, sizeof(entry->offsets)) == 0;
}

static ocr_drm_sink_entry_t *find_or_create_fb(ocr_drm_sink_t *sink,
                                                const ocr_buffer_t *buffer)
{
    ocr_drm_sink_entry_t *free_entry = NULL;
    for (int i = 0; i < OCR_DRM_SINK_MAX_FBS; ++i) {
        ocr_drm_sink_entry_t *entry = &sink->entries[i];
        if (entry_matches(entry, buffer)) return entry;
        if (!entry->valid && !free_entry) free_entry = entry;
    }

    /* 缓存满时只淘汰非 scanout 条目；当前 framebuffer 永不被提前销毁。 */
    if (!free_entry) {
        for (int n = 0; n < OCR_DRM_SINK_MAX_FBS; ++n) {
            unsigned int idx = (sink->next_evict + (unsigned int)n) %
                               OCR_DRM_SINK_MAX_FBS;
            ocr_drm_sink_entry_t *entry = &sink->entries[idx];
            if (entry == sink->current_entry) continue;
            if (ocr_drm_fb_destroy(sink->dev, &entry->fb) == 0) {
                memset(entry, 0, sizeof(*entry));
                free_entry = entry;
                sink->next_evict = (idx + 1U) % OCR_DRM_SINK_MAX_FBS;
                break;
            }
        }
    }
    if (!free_entry) return NULL;

    const uint32_t *strides = buffer->plane_count ? buffer->strides : NULL;
    const uint32_t *offsets = buffer->plane_count ? buffer->offsets : NULL;
    if (ocr_drm_fb_create_with_layout(sink->dev, buffer, strides, offsets,
                                       &free_entry->fb) != 0) {
        return NULL;
    }
    free_entry->source = buffer;
    free_entry->source_fd = buffer->fd;
    free_entry->source_size = buffer->size;
    free_entry->width = buffer->width;
    free_entry->height = buffer->height;
    free_entry->format = buffer->format;
    free_entry->plane_count = buffer->plane_count;
    memcpy(free_entry->strides, buffer->strides, sizeof(free_entry->strides));
    memcpy(free_entry->offsets, buffer->offsets, sizeof(free_entry->offsets));
    free_entry->valid = 1;
    return free_entry;
}

int ocr_drm_sink_init(ocr_drm_sink_t *sink, ocr_drm_device_t *dev,
                      ocr_drm_planes_t *planes)
{
    if (!sink || !dev || !dev->opened || !planes || !planes->primary ||
        dev->fd < 0 || !dev->connector || !dev->crtc) return -1;
    memset(sink, 0, sizeof(*sink));
    sink->dev = dev;
    sink->planes = planes;
    sink->initialized = 1;
    return 0;
}

int ocr_drm_sink_present(ocr_drm_sink_t *sink, ocr_buffer_t *buffer)
{
    if (!buffer) return -1;
    if (!sink || !sink->initialized || buffer->fd < 0) return -2;
    if (sink->current == buffer && sink->current_frame_id == buffer->frame_id)
        return 0;

    ocr_drm_sink_entry_t *entry = find_or_create_fb(sink, buffer);
    if (!entry) return -3;
    if (ocr_buffer_ref(buffer) < 0) return -4;

    int present_ret;
    if (!sink->dev->mode_set) {
        present_ret = ocr_drm_plane_set_fb_ex(
            sink->planes, sink->planes->primary, entry->fb.fb_id,
            0, 0, entry->fb.width, entry->fb.height,
            0, 0, sink->dev->width, sink->dev->height);
    } else {
        present_ret = ocr_drm_fb_pageflip(sink->dev, &entry->fb, NULL, NULL);
        if (present_ret == 0 && sink->planes->primary) {
            sink->planes->primary->fb_id = entry->fb.fb_id;
            sink->planes->primary->in_use = 1;
        }
    }
    if (present_ret != 0) {
        (void)ocr_buffer_unref(buffer);
        return -5;
    }

    ocr_buffer_t *previous = sink->current;
    sink->current = buffer;
    sink->current_entry = entry;
    sink->current_frame_id = buffer->frame_id;
    if (previous) (void)ocr_buffer_unref(previous);
    return 0;
}

int ocr_drm_sink_process(ocr_pipeline_node_t *node, ocr_buffer_t *buffer)
{
    if (!node) return -1;
    return ocr_drm_sink_present((ocr_drm_sink_t *)node->user_ctx, buffer);
}

void ocr_drm_sink_destroy(ocr_drm_sink_t *sink)
{
    if (!sink || !sink->initialized) return;
    if (sink->planes && sink->planes->primary)
        if (ocr_drm_plane_disable(sink->planes,
                                  sink->planes->primary) != 0) {
            LOG_E("cannot release DRM sink while primary plane is active");
            return;
        }

    if (sink->current) {
        (void)ocr_buffer_unref(sink->current);
        sink->current = NULL;
    }
    sink->current_entry = NULL;
    int cleanup_failed = 0;
    if (sink->dev && sink->dev->fd >= 0) {
        for (int i = 0; i < OCR_DRM_SINK_MAX_FBS; ++i) {
            if (sink->entries[i].valid) {
                if (ocr_drm_fb_destroy(sink->dev,
                                       &sink->entries[i].fb) == 0)
                    sink->entries[i].valid = 0;
                else
                    cleanup_failed = 1;
            }
        }
    }
    if (cleanup_failed) return;
    memset(sink, 0, sizeof(*sink));
}
