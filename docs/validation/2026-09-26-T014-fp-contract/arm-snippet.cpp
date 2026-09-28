// T-014：aarch64 上 clang 默认（-ffp-contract=on）与 fast 的对比。写法与 HalfBand / Saturator 的 NEON 封装相同：
// 乘、加各在一个强制内联的小函数里；另一个是 TruePeak 非 NEON 分支 multiplyAdd 的两条语句写法。
#include <arm_neon.h>

static inline __attribute__ ((always_inline)) float32x4_t vAdd (float32x4_t a, float32x4_t b) { return vaddq_f32 (a, b); }
static inline __attribute__ ((always_inline)) float32x4_t vMul (float32x4_t a, float32x4_t b) { return vmulq_f32 (a, b); }

float32x4_t vectorPattern (float32x4_t acc, float32x4_t x, float32x4_t c) { return vAdd (acc, vMul (x, c)); }

float scalarTwoStatements (float sum, float tap, float x)
{
    const auto product = tap * x;
    return sum + product;
}

float scalarOneExpression (float sum, float tap, float x) { return sum + tap * x; }
