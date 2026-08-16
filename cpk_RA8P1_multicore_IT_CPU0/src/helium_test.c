/*
 * helium_test.c
 *
 *  Created on: 2026年4月29日
 *      Author: BEAM
 */
#include <arm_mve.h>  // Helium intrinsics 头文件（开启MVE配置下才能编译）
#include "hal_data.h"
#include <stdint.h>
#include <string.h>

// 浮点 向量乘加：y[i] = a[i] * b[i] + c[i]
// 关闭MVE + -fno-tree-vectorize 后，纯标量执行
void vector_mac_scalar(
    const float *__restrict a,
    const float *__restrict b,
    const float *__restrict c,
    float *__restrict y,
    uint32_t len
) {
    for (uint32_t i = 0; i < len; i++) {
        y[i] = a[i] * b[i] + c[i];
    }
}

// Helium 浮点向量乘加：一次处理 4 个 float32
// 理论性能 = 标量 ×4
void vector_mac_helium(
    const float *__restrict a,
    const float *__restrict b,
    const float *__restrict c,
    float *__restrict y,
    uint32_t len
) {
    uint32_t i = 0;

    // --------------------------
    // Helium 核心：一次处理 4 个 float
    // --------------------------
    for (; i <= len - 4; i += 4) {
        // 加载 4 个 float (128bit 向量)
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        float32x4_t vc = vld1q_f32(c + i);

        // 向量乘法 + 向量加法
        float32x4_t vy = vaddq_f32(vmulq_f32(va, vb), vc);

        // 存储结果
        vst1q_f32(y + i, vy);
    }

    // --------------------------
    // 标量收尾（处理剩余数据）
    // --------------------------
    for (; i < len; i++) {
        y[i] = a[i] * b[i] + c[i];
    }
}
