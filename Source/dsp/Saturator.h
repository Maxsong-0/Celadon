/*
  ==============================================================================

    Saturator.h
    波形整形器：Limit 的 Clip 级与 Color 的 Heat 级共用。

    本类只做波形整形，不含过采样——调用方自己负责在过采样域里调用它，
    避免两层过采样叠在一起。只依赖 juce_dsp（Source/dsp/ 的硬规则）。

    三种用法：
      - processSample：逐样本，tanh 用 std::tanh（Limit 的 SoftClip 在用，v0.5 起保持逐位不变）；
      - processTubeBlock：Tube 曲线的整段版本（Color 的 Heat 残差 makeup 标定、测试在用），
        tanh 换成 4 路并行的有理式 fastTanh，快约 6 倍；整条 Tube 曲线与 processSample 差 ≤ −128 dBFS
        （drive 1 时即 fastTanh 自身 4.1e-7 的误差；v0.5 验收 V1 扫描实测 −128.9 dBFS），所以两种用法不逐位相同；
      - processTubeResidualBlock：Tube (x) − x 的整段版本（Color 的 Heat 在 8× 域、驱动量不变的一段上整段调用，
        2026-09-29 T-032 R3-04），与「processTubeBlock 后再逐样本减原样本」逐位相同，只是少一次拷贝与一遍循环。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

namespace celadon
{
    class Saturator
    {
    public:
        enum class Curve
        {
            SoftClip,   // 阈值 × 0.7 以下完全线性，之后 tanh 型软拐点，渐近封顶在阈值（Limit 的 Clip）
            Tube        // y = tanh(d·x + b) − tanh(b)，再除以原点斜率归一（Color 的 Heat）
        };

        void setCurve (Curve newCurve) noexcept;

        // Tube 的驱动量 d（≥ 1）。
        void setDrive (float newDrive) noexcept;

        // SoftClip 的封顶阈值（线性幅度，> 0）。
        void setThreshold (float newThreshold) noexcept;

        // Tube 的不对称量 b（偶次谐波来源；会带来直流偏移，调用方要接高通）。
        void setAsymmetry (float newAsymmetry) noexcept;

        // 一次设好 d 与 b：结果与先 setDrive 再 setAsymmetry 完全相同，只少算一遍 tanh(b)
        // （Heat 平滑期间每 16 个过采样样本更新一次，省下的是一半的更新开销）。
        void setDriveAndAsymmetry (float newDrive, float newAsymmetry) noexcept;

        float processSample (float x) const noexcept
        {
            return curve == Curve::SoftClip ? softClip (x) : tube (x);
        }

        // Tube 曲线的整段版本：data[0 … numSamples) 原地整形，驱动量 / 不对称量取当前设定（与 setCurve 无关，
        // 总是 Tube）。y = (fastTanh (d·x + b) − fastTanh (b)) / (d · (1 − fastTanh (b)²))：y(0) 精确为 0、
        // 原点斜率 1，与 processSample 的 Tube 只差 fastTanh 的逼近误差。任意长度都走同一段 4 路代码
        // （尾巴补齐成 4 个再算），每个样本的结果与它在块里的位置、块长都无关。
        void processTubeBlock (float* data, int numSamples) const noexcept;

        // Tube 残差的整段版本：data[i] = Tube (data[i]) − data[i]（Color 的 Heat 只把非线性残差加回原声，T-032 R3-04）。
        // 与「processTubeBlock 后再逐样本减原样本」逐位相同：4 路里 Tube (x) 先舍入成 float，再减同一个原样本，
        // 是同一个 IEEE 单精度减法、同一对操作数（ADR-013：乘和减各自舍入、不融合）；只是不再要先拷贝一份原样本、再走第二遍循环。
        // 尾巴补 0 的 lane 得 fastTanh (b) − fastTanh (b) = 0，丢弃不用；NaN 进 NaN 出。ColorTests 按位对照。
        void processTubeResidualBlock (float* data, int numSamples) const noexcept;

        // tanh 的 4 路并行有理式（Eigen generic_fast_tanh_float 的 13 / 6 次式，不融合乘加的那组钳位常数）：
        //   |x| < 4e-4 直接返回 x（略去的 x³/3 < 2.2e-11）；|x| > 7.9053 钳位（结果已是 ±1）；其余 p(x) / q(x)。
        //   与 double tanh 的最大绝对误差 4.10e-7（−127.7 dB，x ≈ 5.83；逐个 float 穷举），奇对称逐位成立，|y| ≤ 1，
        //   ±0 保号，NaN 进 NaN 出，±Inf → ±1。不是逐 ulp 单调（相邻 float 最多回落 8 ulp ≈ 4.8e-7，与逼近误差同量级）。
        // 各平台（NEON / SSE2 / 标量回退）是同一串 IEEE 运算，结果逐位相同（ARM 上 NEON 与标量回退逐字节对比过）；
        // 前提是编译器不把乘和加融合成 FMA（GCC 要关 -ffp-contract，ADR-013）。
        static void fastTanhBlock (float* data, int numSamples) noexcept;
        static float fastTanh (float x) noexcept;

    private:
        float softClip (float x) const noexcept
        {
            const auto magnitude = std::abs (x);

            if (magnitude <= kneeStart)
                return x;

            // 拐点处数值与斜率都连续（tanh 在 0 点斜率为 1），之后渐近逼近 threshold，永不超过。
            const auto shaped = kneeStart + kneeWidth * std::tanh ((magnitude - kneeStart) / kneeWidth);
            return std::copysign (shaped, x);
        }

        float tube (float x) const noexcept
        {
            return (std::tanh (drive * x + asymmetry) - tanhOfAsymmetry) * inverseSlopeAtZero;
        }

        void updateSoftClip() noexcept;
        void updateTube() noexcept;

        Curve curve = Curve::SoftClip;

        float threshold = 1.0f;
        float kneeStart = 0.7f;       // = threshold × 0.7
        float kneeWidth = 0.3f;       // = threshold − kneeStart

        float drive = 1.0f;
        float asymmetry = 0.0f;
        float tanhOfAsymmetry = 0.0f;
        float inverseSlopeAtZero = 1.0f;

        // processTubeBlock / processTubeResidualBlock 用：与块里同一个 fastTanh 算出的 tanh(b) 与 1 / 原点斜率——这样 x = 0 时
        // 两项 fastTanh (b) 逐位相同、相减精确为 0。
        float fastTanhOfAsymmetry = 0.0f;
        float fastInverseSlopeAtZero = 1.0f;
    };
}
