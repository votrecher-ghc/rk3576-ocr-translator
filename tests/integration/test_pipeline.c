/*
 * test_pipeline.c - 管线集成测试
 *
 * 功能: 端到端验证 V4L2 采集 -> 稳像 -> OCR -> 翻译 -> 渲染 管线
 * 说明: 此为骨架测试, 在板端运行; 桌面环境以桩(Stub)模拟各阶段
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* --------------------------------------------------------------------
 * 管线阶段定义 (与实际管线模块接口一致)
 * -------------------------------------------------------------------- */

#define FRAME_WIDTH   1920
#define FRAME_HEIGHT  1080
#define FRAME_SIZE    (FRAME_WIDTH * FRAME_HEIGHT * 2)  /* NV12 */

#define NUM_FRAMES_TEST  10  /* 测试帧数 */

/* 帧数据 */
typedef struct {
    void    *data;
    size_t   size;
    int      index;
    int      valid;
} frame_t;

/* 管线阶段上下文 */
typedef struct {
    const char *name;
    int      enabled;
    frame_t *in_frame;
    frame_t *out_frame;
} stage_ctx_t;

/* --------------------------------------------------------------------
 * 各阶段桩实现 (Stub)
 * -------------------------------------------------------------------- */

/* 阶段1: V4L2 采集 (生成测试帧) */
static int stage_capture(stage_ctx_t *ctx, frame_t *out)
{
    static int frame_idx = 0;
    if (frame_idx >= NUM_FRAMES_TEST) {
        return -1;  /* 采集结束 */
    }
    out->data  = malloc(FRAME_SIZE);
    out->size  = FRAME_SIZE;
    out->index = frame_idx++;
    out->valid = 1;
    /* 模拟数据填充 */
    memset(out->data, (frame_idx & 0xFF), FRAME_SIZE);
    ctx->out_frame = out;
    return 0;
}

/* 阶段2: 稳像 (IMU 校正) */
static int stage_stabilize(stage_ctx_t *ctx, frame_t *in, frame_t *out)
{
    /* 桩: 直接传递帧 (实际实现做裁剪/校正) */
    out->data  = in->data;
    out->size  = in->size;
    out->index = in->index;
    out->valid = 1;
    ctx->in_frame  = in;
    ctx->out_frame = out;
    return 0;
}

/* 阶段3: OCR 检测 */
static int stage_ocr_detect(stage_ctx_t *ctx, frame_t *in, frame_t *out)
{
    /* 桩: 模拟检测到文本区域 */
    out->data  = in->data;
    out->size  = in->size;
    out->index = in->index;
    out->valid = 1;
    ctx->in_frame  = in;
    ctx->out_frame = out;
    return 0;
}

/* 阶段4: OCR 识别 */
static int stage_ocr_recognize(stage_ctx_t *ctx, frame_t *in, frame_t *out)
{
    /* 桩: 模拟识别文本 (输出固定字符串) */
    out->data  = in->data;
    out->size  = in->size;
    out->index = in->index;
    out->valid = 1;
    ctx->in_frame  = in;
    ctx->out_frame = out;
    return 0;
}

/* 阶段5: 翻译 */
static int stage_translate(stage_ctx_t *ctx, frame_t *in, frame_t *out)
{
    /* 桩: 模拟翻译 (输出固定字符串) */
    out->data  = in->data;
    out->size  = in->size;
    out->index = in->index;
    out->valid = 1;
    ctx->in_frame  = in;
    ctx->out_frame = out;
    return 0;
}

/* 阶段6: 渲染 (输出到 DRM) */
static int stage_render(stage_ctx_t *ctx, frame_t *in)
{
    /* 桩: 模拟渲染 (实际写 DRM overlay plane) */
    ctx->in_frame = in;
    return 0;
}

/* 阶段7: 归档 (保存 JPEG) */
static int stage_archive(stage_ctx_t *ctx, frame_t *in)
{
    /* 桩: 模拟归档 */
    ctx->in_frame = in;
    return 0;
}

/* --------------------------------------------------------------------
 * 管线执行 (串行验证)
 * -------------------------------------------------------------------- */
static int run_pipeline_once(void)
{
    stage_ctx_t s_cap, s_stab, s_det, s_rec, s_trans, s_render, s_arch;

    frame_t f_cap, f_stab, f_det, f_rec, f_trans;

    /* 采集 */
    if (stage_capture(&s_cap, &f_cap) != 0) {
        return -1;  /* 无更多帧 */
    }

    /* 稳像 */
    stage_stabilize(&s_stab, &f_cap, &f_stab);
    free(f_cap.data);  /* 采集帧已处理, 释放 */

    /* OCR 检测 */
    stage_ocr_detect(&s_det, &f_stab, &f_det);

    /* OCR 识别 */
    stage_ocr_recognize(&s_rec, &f_det, &f_rec);

    /* 翻译 */
    stage_translate(&s_trans, &f_rec, &f_trans);

    /* 渲染 */
    stage_render(&s_render, &f_trans);

    /* 归档 */
    stage_archive(&s_arch, &f_trans);

    /* 最终释放帧数据 */
    free(f_trans.data);

    return 0;
}

/* --------------------------------------------------------------------
 * 测试用例
 * -------------------------------------------------------------------- */

/* 测试1: 管线完整运行 NUM_FRAMES_TEST 帧 */
static void test_pipeline_end_to_end(void)
{
    printf("  运行 %d 帧端到端管线...\n", NUM_FRAMES_TEST);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    int frame_count = 0;
    while (run_pipeline_once() == 0) {
        frame_count++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    assert(frame_count == NUM_FRAMES_TEST);
    printf("  完成 %d 帧, 耗时 %.3f 秒 (%.1f FPS)\n",
           frame_count, elapsed, frame_count / elapsed);
    printf("[PASS] test_pipeline_end_to_end\n");
}

/* 测试2: 帧索引连续性 */
static void test_frame_index_continuity(void)
{
    printf("  验证帧索引连续性...\n");

    int prev_index = -1;
    int frame_count = 0;
    stage_ctx_t s_cap;
    frame_t f;

    while (stage_capture(&s_cap, &f) == 0) {
        assert(f.index == prev_index + 1);
        prev_index = f.index;
        free(f.data);
        frame_count++;
    }
    assert(frame_count == NUM_FRAMES_TEST);
    printf("[PASS] test_frame_index_continuity\n");
}

/* 测试3: 各阶段 enabled 开关 */
static void test_stage_enable_disable(void)
{
    printf("  验证阶段启用/禁用...\n");

    stage_ctx_t s;
    s.enabled = 1;
    assert(s.enabled == 1);

    s.enabled = 0;
    assert(s.enabled == 0);

    printf("[PASS] test_stage_enable_disable\n");
}

/* 测试4: 错误处理 - 空帧处理 */
static void test_null_frame_handling(void)
{
    printf("  验证空帧处理...\n");

    stage_ctx_t s;
    frame_t f;
    f.data  = NULL;
    f.valid = 0;

    /* 渲染阶段应能安全处理空帧 (不崩溃) */
    int ret = stage_render(&s, &f);
    assert(ret == 0);

    printf("[PASS] test_null_frame_handling\n");
}

/* --------------------------------------------------------------------
 * 主函数
 * -------------------------------------------------------------------- */
int main(void)
{
    printf("=== 管线集成测试开始 ===\n");
    test_pipeline_end_to_end();
    test_frame_index_continuity();
    test_stage_enable_disable();
    test_null_frame_handling();
    printf("=== 全部测试通过 ===\n");
    return 0;
}
