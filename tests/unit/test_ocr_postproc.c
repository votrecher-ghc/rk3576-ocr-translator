#include "ocr_postproc.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void test_oriented_component_box(void)
{
    enum { WIDTH = 48, HEIGHT = 48 };
    float *map = calloc((size_t)WIDTH * HEIGHT, sizeof(*map));
    assert(map != NULL);

    /* A thick +35-degree text-like component. */
    const float angle = 35.0f * 3.14159265358979323846f / 180.0f;
    const float c = cosf(angle), s = sinf(angle);
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            float dx = (float)x + 0.5f - 24.0f;
            float dy = (float)y + 0.5f - 24.0f;
            float u = dx * c + dy * s;
            float v = -dx * s + dy * c;
            if (fabsf(u) <= 14.0f && fabsf(v) <= 3.0f)
                map[(size_t)y * WIDTH + (size_t)x] = 0.95f;
        }
    }

    ocr_text_box_list_t boxes;
    assert(ocr_db_postprocess(map, WIDTH, HEIGHT, 0.5f, 0.8f, 0.0f,
                              &boxes) == 0);
    assert(boxes.count == 1);
    assert(boxes.boxes[0].valid);
    /* Top edge must retain a measurable slope instead of becoming axis-aligned. */
    assert(fabsf(boxes.boxes[0].y[1] - boxes.boxes[0].y[0]) > 2.0f);
    free(map);
}

int main(void)
{
    test_oriented_component_box();
    puts("[PASS] OCR oriented post-processing tests");
    return 0;
}
