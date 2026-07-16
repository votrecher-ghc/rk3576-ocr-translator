/**
 * @file ocr_postproc.c
 * @brief Dependency-free DB connected-component post-processing.
 */
#include "ocr_postproc.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static float box_center_x(const ocr_text_box_t *box)
{
    return (box->x[0] + box->x[1] + box->x[2] + box->x[3]) * 0.25f;
}

static float box_center_y(const ocr_text_box_t *box)
{
    return (box->y[0] + box->y[1] + box->y[2] + box->y[3]) * 0.25f;
}

static float box_height(const ocr_text_box_t *box)
{
    float left = hypotf(box->x[3] - box->x[0], box->y[3] - box->y[0]);
    float right = hypotf(box->x[2] - box->x[1], box->y[2] - box->y[1]);
    return fmaxf(1.0f, 0.5f * (left + right));
}

static float clamp_coord(float value, float maximum)
{
    return fminf(maximum, fmaxf(0.0f, value));
}

static void keep_best_box(ocr_text_box_list_t *out,
                          const ocr_text_box_t *candidate)
{
    if (out->count < OCR_MAX_TEXT_BOXES) {
        out->boxes[out->count++] = *candidate;
        return;
    }

    int lowest = 0;
    for (int i = 1; i < out->count; ++i) {
        if (out->boxes[i].score < out->boxes[lowest].score) lowest = i;
    }
    if (candidate->score > out->boxes[lowest].score) {
        out->boxes[lowest] = *candidate;
    }
}

static int extract_components(uint8_t *binary, const float *prob_map,
                              int width, int height, float box_thresh,
                              float unclip_ratio, size_t *queue,
                              ocr_text_box_list_t *out)
{
    static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    const size_t pixels = (size_t)width * (size_t)height;

    for (size_t seed = 0; seed < pixels; ++seed) {
        if (!binary[seed]) continue;

        size_t head = 0;
        size_t tail = 0;
        size_t component_pixels = 0;
        double score_sum = 0.0;
        double sum_x = 0.0, sum_y = 0.0;
        double sum_xx = 0.0, sum_xy = 0.0, sum_yy = 0.0;

        binary[seed] = 0;
        queue[tail++] = seed;
        while (head < tail) {
            size_t index = queue[head++];
            int y = (int)(index / (size_t)width);
            int x = (int)(index - (size_t)y * (size_t)width);

            ++component_pixels;
            score_sum += prob_map[index];
            double px = (double)x + 0.5;
            double py = (double)y + 0.5;
            sum_x += px; sum_y += py;
            sum_xx += px * px; sum_xy += px * py; sum_yy += py * py;

            for (int n = 0; n < 8; ++n) {
                int nx = x + dx[n];
                int ny = y + dy[n];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                size_t next = (size_t)ny * (size_t)width + (size_t)nx;
                if (binary[next]) {
                    binary[next] = 0;
                    queue[tail++] = next;
                }
            }
        }

        if (component_pixels < 3) continue;
        float score = (float)(score_sum / (double)component_pixels);
        if (!isfinite(score) || score < box_thresh) continue;

        double mean_x = sum_x / (double)component_pixels;
        double mean_y = sum_y / (double)component_pixels;
        double cov_xx = sum_xx / (double)component_pixels - mean_x * mean_x;
        double cov_xy = sum_xy / (double)component_pixels - mean_x * mean_y;
        double cov_yy = sum_yy / (double)component_pixels - mean_y * mean_y;
        float angle = 0.5f * atan2f((float)(2.0 * cov_xy),
                                    (float)(cov_xx - cov_yy));
        float ux = cosf(angle), uy = sinf(angle);
        if (ux < 0.0f) { ux = -ux; uy = -uy; }
        float vx = -uy, vy = ux;
        float min_u = INFINITY, max_u = -INFINITY;
        float min_v = INFINITY, max_v = -INFINITY;
        for (size_t i = 0; i < tail; ++i) {
            int py = (int)(queue[i] / (size_t)width);
            int px = (int)(queue[i] - (size_t)py * (size_t)width);
            float fx = (float)px + 0.5f;
            float fy = (float)py + 0.5f;
            float pu = fx * ux + fy * uy;
            float pv = fx * vx + fy * vy;
            if (pu < min_u) min_u = pu;
            if (pu > max_u) max_u = pu;
            if (pv < min_v) min_v = pv;
            if (pv > max_v) max_v = pv;
        }
        /* Pixel centers span one pixel less than the component's outer edge. */
        min_u -= 0.5f; max_u += 0.5f;
        min_v -= 0.5f; max_v += 0.5f;
        float box_w = max_u - min_u;
        float box_h = max_v - min_v;
        float perimeter = 2.0f * (box_w + box_h);
        float distance = perimeter > 0.0f
                       ? (box_w * box_h * unclip_ratio) / perimeter
                       : 0.0f;
        min_u -= distance; max_u += distance;
        min_v -= distance; max_v += distance;

        ocr_text_box_t box;
        memset(&box, 0, sizeof(box));
        const float corners_u[4] = {min_u, max_u, max_u, min_u};
        const float corners_v[4] = {min_v, min_v, max_v, max_v};
        for (int point = 0; point < 4; ++point) {
            float px = corners_u[point] * ux + corners_v[point] * vx;
            float py = corners_u[point] * uy + corners_v[point] * vy;
            box.x[point] = clamp_coord(px, (float)width);
            box.y[point] = clamp_coord(py, (float)height);
        }
        box.score = score;
        box.valid = 1;
        keep_best_box(out, &box);
    }
    return 0;
}

static void insertion_sort_y(ocr_text_box_t *boxes, int count)
{
    for (int i = 1; i < count; ++i) {
        ocr_text_box_t item = boxes[i];
        float key = box_center_y(&item);
        int j = i - 1;
        while (j >= 0 && box_center_y(&boxes[j]) > key) {
            boxes[j + 1] = boxes[j];
            --j;
        }
        boxes[j + 1] = item;
    }
}

static void insertion_sort_x(ocr_text_box_t *boxes, int begin, int end)
{
    for (int i = begin + 1; i < end; ++i) {
        ocr_text_box_t item = boxes[i];
        float key = box_center_x(&item);
        int j = i - 1;
        while (j >= begin && box_center_x(&boxes[j]) > key) {
            boxes[j + 1] = boxes[j];
            --j;
        }
        boxes[j + 1] = item;
    }
}

int ocr_sort_boxes(ocr_text_box_list_t *list)
{
    if (!list || list->count < 0 || list->count > OCR_MAX_TEXT_BOXES) {
        return -EINVAL;
    }

    int valid_count = 0;
    for (int i = 0; i < list->count; ++i) {
        if (list->boxes[i].valid) list->boxes[valid_count++] = list->boxes[i];
    }
    list->count = valid_count;
    if (valid_count < 2) return 0;

    insertion_sort_y(list->boxes, valid_count);
    int row_begin = 0;
    while (row_begin < valid_count) {
        int row_end = row_begin + 1;
        float center_sum = box_center_y(&list->boxes[row_begin]);
        float height_sum = box_height(&list->boxes[row_begin]);

        while (row_end < valid_count) {
            int row_count = row_end - row_begin;
            float row_center = center_sum / row_count;
            float row_height = height_sum / row_count;
            float tolerance = fmaxf(3.0f, row_height * 0.5f);
            float next_center = box_center_y(&list->boxes[row_end]);
            if (fabsf(next_center - row_center) > tolerance) break;
            center_sum += next_center;
            height_sum += box_height(&list->boxes[row_end]);
            ++row_end;
        }
        insertion_sort_x(list->boxes, row_begin, row_end);
        row_begin = row_end;
    }
    return 0;
}

static int validate_request(const float *prob_map, int width, int height,
                            float thresh, float box_thresh,
                            float unclip_ratio, ocr_text_box_list_t *out,
                            size_t *pixels)
{
    if (!prob_map || !out || width <= 0 || height <= 0 ||
        !isfinite(thresh) || !isfinite(box_thresh) ||
        !isfinite(unclip_ratio) || thresh < 0.0f || thresh > 1.0f ||
        box_thresh < 0.0f || box_thresh > 1.0f || unclip_ratio < 0.0f) {
        return -EINVAL;
    }
    if ((size_t)width > SIZE_MAX / (size_t)height) return -EOVERFLOW;
    *pixels = (size_t)width * (size_t)height;
    if (*pixels > SIZE_MAX / sizeof(size_t)) return -EOVERFLOW;
    return 0;
}

int ocr_db_postprocess_with_workspace(
    const float *prob_map, int width, int height,
    float thresh, float box_thresh, float unclip_ratio,
    uint8_t *binary, size_t *queue, size_t workspace_pixels,
    ocr_text_box_list_t *out)
{
    size_t pixels;
    int ret = validate_request(prob_map, width, height, thresh, box_thresh,
                               unclip_ratio, out, &pixels);
    if (ret != 0) return ret;
    if (!binary || !queue || workspace_pixels < pixels) return -ENOSPC;

    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < pixels; ++i) {
        binary[i] = isfinite(prob_map[i]) && prob_map[i] >= thresh ? 1u : 0u;
    }
    ret = extract_components(binary, prob_map, width, height, box_thresh,
                             unclip_ratio, queue, out);
    if (ret != 0) {
        memset(out, 0, sizeof(*out));
        return ret;
    }
    return ocr_sort_boxes(out);
}

int ocr_db_postprocess(const float *prob_map, int width, int height,
                       float thresh, float box_thresh, float unclip_ratio,
                       ocr_text_box_list_t *out)
{
    size_t pixels;
    int ret = validate_request(prob_map, width, height, thresh, box_thresh,
                               unclip_ratio, out, &pixels);
    if (ret != 0) return ret;
    uint8_t *binary = malloc(pixels);
    size_t *queue = malloc(pixels * sizeof(*queue));
    if (!binary || !queue) {
        free(binary);
        free(queue);
        return -ENOMEM;
    }
    ret = ocr_db_postprocess_with_workspace(
        prob_map, width, height, thresh, box_thresh, unclip_ratio,
        binary, queue, pixels, out);
    free(queue);
    free(binary);
    return ret;
}
