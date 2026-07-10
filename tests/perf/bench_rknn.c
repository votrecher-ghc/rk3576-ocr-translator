/*
 * bench_rknn.c - NPU 性能基准测试
 *
 * 功能: 测量 RKNN 模型在 RK3576 NPU 上的推理性能
 * 指标: 单次推理延迟, 吞吐量 (FPS), NPU 利用率
 * 模型: ppocrv4_det (INT8), ppocrv4_rec (FP16), lite_transformer (FP16)
 *
 * 编译: 交叉编译, 链接 librknnrt.so
 * 运行: 板端执行
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "rknn_api.h"  /* RKNN 运行时头文件 */

/* --------------------------------------------------------------------
 * 基准配置
 * -------------------------------------------------------------------- */

#define BENCH_ITERATIONS  100   /* 每个模型推理次数 (取平均) */
#define WARMUP_ITERATIONS 5     /* 预热次数 (不计入统计) */

/* 待测模型列表 */
typedef struct {
    const char *name;          /* 模型名称 */
    const char *path;          /* 模型文件路径 */
    int         input_width;   /* 输入宽度 */
    int         input_height;  /* 输入高度 */
    int         input_channels;/* 输入通道数 */
} bench_model_t;

static bench_model_t models[] = {
    {
        .name = "ppocrv4_det (INT8)",
        .path = "/usr/share/ocr/models/ppocrv4_det_int8.rknn",
        .input_width = 960,
        .input_height = 960,
        .input_channels = 3,
    },
    {
        .name = "ppocrv4_rec (FP16)",
        .path = "/usr/share/ocr/models/ppocrv4_rec_fp16.rknn",
        .input_width = 320,
        .input_height = 48,
        .input_channels = 3,
    },
    {
        .name = "lite_transformer (FP16)",
        .path = "/usr/share/ocr/models/lite_transformer_fp16.rknn",
        .input_width = 128,
        .input_height = 1,
        .input_channels = 1,
    },
};

#define NUM_MODELS (sizeof(models) / sizeof(models[0]))

/* --------------------------------------------------------------------
 * 辅助函数
 * -------------------------------------------------------------------- */

/* 获取当前时间 (毫秒) */
static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* 检查文件是否存在 */
static int file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

/* --------------------------------------------------------------------
 * 单模型基准测试
 * -------------------------------------------------------------------- */
static int bench_single_model(const bench_model_t *model)
{
    printf("\n--- 基准测试: %s ---\n", model->name);
    printf("  模型路径: %s\n", model->path);
    printf("  输入尺寸: %dx%dx%d\n",
           model->input_width, model->input_height, model->input_channels);

    /* 检查模型文件 */
    if (!file_exists(model->path)) {
        printf("  [跳过] 模型文件不存在\n");
        return -1;
    }

    /* 初始化 RKNN */
    rknn_context ctx;
    int ret = rknn_init(&ctx, NULL, 0, 0, NULL);
    if (ret < 0) {
        printf("  [错误] rknn_init 失败: %d\n", ret);
        return -1;
    }

    /* 获取输入输出属性 */
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
        printf("  [错误] rknn_query 失败: %d\n", ret);
        rknn_destroy(ctx);
        return -1;
    }
    printf("  输入节点: %d, 输出节点: %d\n", io_num.n_input, io_num.n_output);

    /* 准备输入数据 (随机填充) */
    size_t input_size = model->input_width * model->input_height *
                        model->input_channels;
    void *input_data = malloc(input_size);
    if (!input_data) {
        printf("  [错误] 分配输入内存失败\n");
        rknn_destroy(ctx);
        return -1;
    }
    memset(input_data, 128, input_size);  /* 填充中性值 */

    /* 设置输入 */
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = input_size;
    inputs[0].buf = input_data;
    inputs[0].pass_through = 0;
    inputs[0].fmt = RKNN_TENSOR_NHWC;

    /* 预热 */
    printf("  预热 %d 次...\n", WARMUP_ITERATIONS);
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        rknn_outputs_set(ctx, 0, NULL);  /* 桩调用 */
    }

    /* 正式测试 */
    printf("  正式测试 %d 次...\n", BENCH_ITERATIONS);
    double times[BENCH_ITERATIONS];
    double total_ms = 0;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        double t_start = get_time_ms();

        /* 设置输入并运行推理 */
        rknn_inputs_set(ctx, 1, inputs);
        rknn_output outputs[io_num.n_output];
        memset(outputs, 0, sizeof(outputs));
        for (int j = 0; j < io_num.n_output; j++) {
            outputs[j].want_float = 0;
            outputs[j].is_prealloc = 0;
        }
        rknn_run(ctx, NULL);
        rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);

        double t_end = get_time_ms();
        times[i] = t_end - t_start;
        total_ms += times[i];

        rknn_outputs_release(ctx, io_num.n_output, outputs);
    }

    /* 统计结果 */
    double avg_ms = total_ms / BENCH_ITERATIONS;
    double min_ms = times[0], max_ms = times[0];
    for (int i = 1; i < BENCH_ITERATIONS; i++) {
        if (times[i] < min_ms) min_ms = times[i];
        if (times[i] > max_ms) max_ms = times[i];
    }
    double fps = 1000.0 / avg_ms;

    printf("  结果:\n");
    printf("    平均延迟: %.2f ms\n", avg_ms);
    printf("    最小延迟: %.2f ms\n", min_ms);
    printf("    最大延迟: %.2f ms\n", max_ms);
    printf("    吞吐量:   %.1f FPS\n", fps);

    /* 清理 */
    free(input_data);
    rknn_destroy(ctx);
    return 0;
}

/* --------------------------------------------------------------------
 * 主函数
 * -------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    printf("=== RKNN NPU 性能基准测试 ===\n");
    printf("测试次数: %d (预热 %d)\n", BENCH_ITERATIONS, WARMUP_ITERATIONS);
    printf("模型数量: %zu\n", NUM_MODELS);

    for (size_t i = 0; i < NUM_MODELS; i++) {
        bench_single_model(&models[i]);
    }

    printf("\n=== 基准测试完成 ===\n");
    return 0;
}
