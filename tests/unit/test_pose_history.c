#include "pose_history.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static int close_enough(float a, float b)
{
    return fabsf(a - b) < 1.0e-4f;
}

int main(void)
{
    ocr_pose_history_t history;
    quaternion_t identity = {1.0f, 0.0f, 0.0f, 0.0f};
    quaternion_t z_90 = {
        0.7071067812f, 0.0f, 0.0f, 0.7071067812f
    };
    quaternion_t sampled;

    ocr_pose_history_init(&history);
    assert(ocr_pose_history_sample(&history, 1, &sampled) == -1);

    assert(ocr_pose_history_push(&history, 1000000000LL, &identity) == 0);
    assert(ocr_pose_history_push(&history, 2000000000LL, &z_90) == 0);
    assert(ocr_pose_history_push(&history, 1500000000LL, &identity) == -2);

    assert(ocr_pose_history_sample(&history, 500000000LL, &sampled) == 0);
    assert(close_enough(sampled.w, identity.w));
    assert(close_enough(sampled.z, identity.z));

    assert(ocr_pose_history_sample(&history, 2500000000LL, &sampled) == 0);
    assert(close_enough(sampled.w, z_90.w));
    assert(close_enough(sampled.z, z_90.z));

    assert(ocr_pose_history_sample(&history, 1500000000LL, &sampled) == 0);
    assert(close_enough(sampled.w, 0.9238795325f));
    assert(close_enough(sampled.x, 0.0f));
    assert(close_enough(sampled.y, 0.0f));
    assert(close_enough(sampled.z, 0.3826834324f));

    puts("pose history tests passed");
    return 0;
}
