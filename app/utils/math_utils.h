#ifndef OCR_UTILS_MATH_H
#define OCR_UTILS_MATH_H
/**
 * @file math_utils.h
 * @brief 数学工具：clamp/lerp/区间映射/角度转换
 */

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief 将 value 限制在 [lo, hi] 区间
 */
static inline int ocr_clamp_i(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/** 浮点 clamp */
static inline float ocr_clamp_f(float value, float lo, float hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/**
 * @brief 线性插值
 * @param a 起点值
 * @param b 终点值
 * @param t 插值因子 [0,1]
 * @return 插值结果
 */
static inline float ocr_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

/**
 * @brief 将 value 从 [in_min, in_max] 映射到 [out_min, out_max]
 */
static inline float ocr_map_range(float value, float in_min, float in_max,
                                  float out_min, float out_max)
{
    if (in_max == in_min) return out_min;
    return out_min + (value - in_min) * (out_max - out_min) / (in_max - in_min);
}

/**
 * @brief 低通滤波一阶更新
 * @param prev 上一次输出
 * @param curr 当前输入
 * @param alpha 滤波系数 [0,1]，越大越接近当前输入
 * @return 滤波输出
 */
static inline float ocr_lowpass(float prev, float curr, float alpha)
{
    return prev + alpha * (curr - prev);
}

/** 角度→弧度 */
static inline float ocr_deg2rad(float deg) { return deg * (float)M_PI / 180.0f; }
/** 弧度→角度 */
static inline float ocr_rad2deg(float rad) { return rad * 180.0f / (float)M_PI; }

#endif /* OCR_UTILS_MATH_H */
