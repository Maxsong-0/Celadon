/*
  ==============================================================================

    ColorModule.cpp

  ==============================================================================
*/

#include "ColorModule.h"

#include "AtomicHelpers.h"

#include <array>
#include <cstdint>
#include <cstring>

// MSVC：VS2022（v143）的 /fp:precise 默认不融合乘加；更早的工具集（v142）配 /arch:AVX2 时 /fp:precise 下仍可能融合，
// 这里写死关掉（ADR-013：各路径逐位相同的前提）。clang-cl 不认这个 pragma，由 /clang:-ffp-contract=off 与源码守卫管。
#if defined (_MSC_VER) && ! defined (__clang__)
 #pragma fp_contract (off)
#endif

// ARM 上 Tone 搁架与 Glue 侧链高通两声道打包成 float64x2 一起算（v0.5 研究 P3 R4）。其他平台保留标量写法
// （结果本来就与 v0.4 逐位相同）：标量 processBiquad 里 b0·x + s1 会不会被编译器合并成融合乘加取决于目标与编译选项，
// 只有在 ARM 上能确定它是融合的（ColorTests「立体声 L = R 与单声道逐位相同」验证），打包写法才能照抄出逐位相同的结果。
#if ! defined (CELADON_FORCE_SCALAR_SIMD) && defined (__ARM_NEON)
 #define CELADON_COLOR_NEON 1
 #include <arm_neon.h>
#endif

namespace celadon
{
    namespace
    {
        constexpr double smoothingSeconds          = 0.02;   // 参数平滑 20 ms（设计规格-总体「参数」）
        constexpr int    coefficientUpdateInterval = 16;     // Tone 系数 / Heat 驱动量每 16 个样本按平滑值重算一次

        // Tone：跷跷板式搁架（设计规格-Color「各级定义」）。规格写 Q 0.707，即 1/√2（Butterworth）：
        // 搁架斜率 S = 1、无过冲；Glue 的侧链高通也用这个 Q。
        constexpr double toneLowShelfHz  = 100.0;
        constexpr double toneHighShelfHz = 8000.0;
        constexpr double butterworthQ    = 0.70710678118654752;

        // Glue：前馈总线压缩。
        constexpr double glueSidechainHz          = 100.0;   // 侧链高通：低音不触发泵
        constexpr double glueThresholdMaxDb       = -24.0;   // Glue = 100 时的阈值（初版映射，试听后调）
        constexpr double glueRatio                = 2.0;
        constexpr double glueSlope                = 1.0 - 1.0 / glueRatio;   // 高于阈值的部分被压掉的比例
        constexpr double glueKneeDb               = 6.0;     // 软拐点总宽度
        constexpr double glueAttackMs             = 20.0;
        constexpr double glueReleaseMinMs         = 50.0;
        constexpr double glueReleaseMaxMs         = 500.0;
        constexpr double glueAverageWindowMs      = 500.0;   // 「最近 500 ms 的平均压缩量」
        constexpr double glueDeepReductionDb      = 6.0;     // 平均压缩量到这么深时释放取上限：2:1 下 6 dB 已是重压（初版，试听后调）
        constexpr int    glueAdaptIntervalSamples = 32;      // 释放时间每 32 样本重算一次（同 Limit 慢段）
        constexpr double glueSnapDb               = 1.0e-6;  // 压缩量低于 1e-6 dB（增益差 1.2e-7）直接归零
        constexpr double dbToNaturalLog           = 0.11512925464970229;   // ln(10) / 20：dB → exp 的指数

        // 低频原声保留，150 Hz 二阶高通只喂非线性支路；其残差加回延迟后的原声。
        // Mid 保留温和偶次谐波；Side 奇对称且较轻，不把反相信号的偶次谐波变成虚假的中心声像。
        constexpr float  heatDriveRange        = 1.0f;
        constexpr float  heatMaxWetMix         = 0.25f;
        constexpr float  heatMaxAsymmetry      = 0.04f;
        constexpr float  heatSideDriveRange    = 0.5f;
        constexpr float  heatSideResidualGain  = 0.75f;
        constexpr double heatHighpassHz        = 150.0;
        constexpr double heatDcBlockerHz       = 5.0;
        constexpr float  heatCalibrationDb     = -18.0f;   // 旧曲线标定仅用作轻微残差makeup
        constexpr int    heatTableSize         = 101;      // h = 0, 1, …, 100
        constexpr int    heatCalibrationPoints = 1024;     // 标定正弦一个周期采多少点：与采样率无关，谐波远在 512 次以内衰尽

        // Heat 湿路空闲跳过：重开时用最近这么多个 Heat 输入预热过采样器。三级半带 FIR 的记忆是 133 个 1× 样本
        // （HalfBandOversampler::getMemoryInSamples()，实测 ≥ 120 就逐位相同），取 256 留足余量。
        constexpr int heatPrimeLength = 256;

        // 滤波器状态低于此值就归零：静音里的指数衰减不会一路掉进 denormal（double 下要几秒才自然到 0）。
        constexpr double stateSnapThreshold = 1.0e-30;

        // 时间常数 → 一阶系数：每样本 y = target + (y − target) · coeff。
        double timeConstantCoeff (double fs, double milliseconds)
        {
            return std::exp (-1.0 / (milliseconds * 0.001 * fs));
        }

        // Tone 空闲时整段跳过的前提之一：本块每个样本都是有限值、且不是 −0。直通双二阶（系数 1, c, d, c, d、状态 0）
        // 对这样的样本输出恰为输入、状态恰为 +0；但它把 −0 算成 +0（1·(−0) + (+0) = +0），遇到 NaN / Inf 状态会变成 NaN
        // （之后一直输出 NaN，直到处理器的 NaN 保护清空整条链）——含这两种样本的块（实际几乎不会有）照常逐样本处理，
        // 跳过与不跳过就在任何输入下都逐位相同。按位检查、整数「或」累积，编译器能直接向量化，每样本不到 0.1 ns。
        bool allSamplesPassThroughExactly (const float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept
        {
            std::uint32_t special = 0;

            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                const auto* data = channels[ch] + startSample;

                for (int i = 0; i < numSamples; ++i)
                {
                    std::uint32_t bits;
                    std::memcpy (&bits, data + i, sizeof (bits));
                    special |= (std::uint32_t) (bits == 0x80000000u)                       // −0
                             | (std::uint32_t) ((bits & 0x7f800000u) == 0x7f800000u);      // ±Inf、NaN（指数全 1）
                }
            }

            return special == 0;
        }

        // Mix → Output → Bypass 交叉淡化的一个样本。逐样本循环与「三个平滑器都静止」的常数循环共用这一个函数，
        // 两边编出来是同一串运算，结果逐位相同。
        inline float mixOutputBypass (float wet, float dry, float mix, float outputGain, float bypassMix) noexcept
        {
            // Mix：m = 1 逐位取湿、m = 0 逐位取干，只在中间值时真正混合。
            float mixed;
            if (mix >= 1.0f)       mixed = wet;
            else if (mix <= 0.0f)  mixed = dry;
            else                   mixed = dry + (wet - dry) * mix;

            // Output：0 dB 时增益精确为 1.0，乘法逐位不变。
            const auto processed = mixed * outputGain;

            // Bypass：完全旁通时逐位等于延迟后的输入。
            if (bypassMix <= 0.0f)       return processed;
            if (bypassMix >= 1.0f)       return dry;
            return processed + (dry - processed) * bypassMix;
        }

       #if CELADON_COLOR_NEON
        // 两声道共用一组双二阶系数、状态打包成 float64x2。每一步与标量 ColorModule::processBiquad 在 ARM 上的
        // 编译结果相同：y = b0·x + s1 是一条融合乘加（编译器默认在同一表达式内合并），其余乘积各自舍入后再加减。
        struct StereoBiquad
        {
            float64x2_t b0, b1, b2, a1, a2;

            void set (double cb0, double cb1, double cb2, double ca1, double ca2) noexcept
            {
                b0 = vdupq_n_f64 (cb0);
                b1 = vdupq_n_f64 (cb1);
                b2 = vdupq_n_f64 (cb2);
                a1 = vdupq_n_f64 (ca1);
                a2 = vdupq_n_f64 (ca2);
            }

            float64x2_t process (float64x2_t x, float64x2_t& s1, float64x2_t& s2) const noexcept
            {
                const auto y   = vfmaq_f64 (s1, b0, x);
                const auto bx1 = vmulq_f64 (b1, x);
                const auto ay1 = vmulq_f64 (a1, y);
                s1 = vaddq_f64 (vsubq_f64 (bx1, ay1), s2);

                const auto bx2 = vmulq_f64 (b2, x);
                const auto ay2 = vmulq_f64 (a2, y);
                s2 = vsubq_f64 (bx2, ay2);
                return y;
            }
        };

        inline float64x2_t packPair (double left, double right) noexcept
        {
            return vsetq_lane_f64 (right, vdupq_n_f64 (left), 1);
        }

        // 两个 float 样本 → double（精确）；double → float 与标量的 (float) 同为就近舍入。
        inline float64x2_t loadStereo (float left, float right) noexcept
        {
            return vcvt_f64_f32 (vset_lane_f32 (right, vdup_n_f32 (left), 1));
        }

        inline void storeStereo (float64x2_t y, float& left, float& right) noexcept
        {
            const auto narrowed = vcvt_f32_f64 (y);
            left  = vget_lane_f32 (narrowed, 0);
            right = vget_lane_f32 (narrowed, 1);
        }
       #endif
    }

    //==============================================================================
    void ColorModule::BlockDelay::prepare (int delaySamples, int maxBlock)
    {
        delay = juce::jmax (0, delaySamples);
        buffer.assign ((size_t) (delay + juce::jmax (1, maxBlock)), 0.0f);
    }

    void ColorModule::BlockDelay::fill (float value) noexcept
    {
        std::fill (buffer.begin(), buffer.end(), value);
    }

    void ColorModule::BlockDelay::process (const float* input, float* output, int numSamples) noexcept
    {
        // 本块接在历史后面，输出取整段的前 numSamples 个（= delay 个样本之前的输入），再把最后 delay 个留作历史。
        // 纯搬运，与逐样本推入推出逐位相同；numSamples 不超过 prepare 时的块长。
        auto* data = buffer.data();
        std::memcpy (data + delay, input, sizeof (float) * (size_t) numSamples);
        std::memcpy (output, data, sizeof (float) * (size_t) numSamples);
        std::memmove (data, data + numSamples, sizeof (float) * (size_t) delay);
    }

    //==============================================================================
    double ColorModule::processBiquad (double x, const BiquadCoefficients& c, BiquadState& s) noexcept
    {
        // 转置直接 II 型。乘积先落到具名变量再相减：编译器默认只在同一表达式内做乘加融合，
        // 这样 b1·x − a1·y 在 b1 == a1、y == x 时是两个逐位相同的乘积之差 = 精确的 0，
        // Tone = 0 的直通滤波器状态才能恒为 0（推导见 updateToneCoefficients）。
        const auto y = c.b0 * x + s.s1;

        const auto bx1 = c.b1 * x;
        const auto ay1 = c.a1 * y;
        s.s1 = (bx1 - ay1) + s.s2;

        const auto bx2 = c.b2 * x;
        const auto ay2 = c.a2 * y;
        s.s2 = bx2 - ay2;

        return y;
    }

    void ColorModule::snapToZero (double& value) noexcept
    {
        if (std::abs (value) < stateSnapThreshold)
            value = 0.0;
    }

    ColorModule::BiquadCoefficients ColorModule::designShelf (double fs, double frequency, double gainDb, double q, bool isHighShelf)
    {
        // RBJ Audio EQ Cookbook 的搁架公式。A = 1（gainDb = 0）时逐项可见 b 与 a 完全相同：
        //   (A − 1) = 0 → 带 (A − 1) 的项精确为 0，剩下的 (A + 1) = 2、2√A·α = 2α 两边一样，
        //   乘以 A = 1、2A = 2 都是精确运算，所以 b0 == a0、b1 == a1、b2 == a2 逐位相等，
        //   归一后 b0 = 1、b1 = a1、b2 = a2 —— 滤波器逐位直通，状态从 0 出发恒为 0。
        const auto a       = std::pow (10.0, gainDb / 40.0);
        const auto w0      = juce::MathConstants<double>::twoPi * juce::jmin (frequency, 0.45 * fs) / fs;
        const auto cosW0   = std::cos (w0);
        const auto alpha   = std::sin (w0) / (2.0 * q);
        const auto aPlus1  = a + 1.0;
        const auto aMinus1 = a - 1.0;
        const auto twoSqrtAAlpha = 2.0 * std::sqrt (a) * alpha;

        double b0, b1, b2, a0, a1, a2;

        if (isHighShelf)
        {
            b0 =        a * (aPlus1 + aMinus1 * cosW0 + twoSqrtAAlpha);
            b1 = -2.0 * a * (aMinus1 + aPlus1 * cosW0);
            b2 =        a * (aPlus1 + aMinus1 * cosW0 - twoSqrtAAlpha);
            a0 =            (aPlus1 - aMinus1 * cosW0 + twoSqrtAAlpha);
            a1 =      2.0 * (aMinus1 - aPlus1 * cosW0);
            a2 =            (aPlus1 - aMinus1 * cosW0 - twoSqrtAAlpha);
        }
        else
        {
            b0 =        a * (aPlus1 - aMinus1 * cosW0 + twoSqrtAAlpha);
            b1 =  2.0 * a * (aMinus1 - aPlus1 * cosW0);
            b2 =        a * (aPlus1 - aMinus1 * cosW0 - twoSqrtAAlpha);
            a0 =            (aPlus1 + aMinus1 * cosW0 + twoSqrtAAlpha);
            a1 =     -2.0 * (aMinus1 + aPlus1 * cosW0);
            a2 =            (aPlus1 + aMinus1 * cosW0 - twoSqrtAAlpha);
        }

        BiquadCoefficients c;
        c.b0 = b0 / a0;   // b0 == a0 时精确等于 1.0
        c.b1 = b1 / a0;
        c.b2 = b2 / a0;
        c.a1 = a1 / a0;
        c.a2 = a2 / a0;
        return c;
    }

    ColorModule::BiquadCoefficients ColorModule::designHighpass (double fs, double frequency, double q)
    {
        // RBJ 二阶高通。
        const auto w0    = juce::MathConstants<double>::twoPi * juce::jmin (frequency, 0.45 * fs) / fs;
        const auto cosW0 = std::cos (w0);
        const auto alpha = std::sin (w0) / (2.0 * q);
        const auto a0    = 1.0 + alpha;

        BiquadCoefficients c;
        c.b0 = (1.0 + cosW0) * 0.5 / a0;
        c.b1 = -(1.0 + cosW0) / a0;
        c.b2 = c.b0;
        c.a1 = -2.0 * cosW0 / a0;
        c.a2 = (1.0 - alpha) / a0;
        return c;
    }

    void ColorModule::configureTube (Saturator& saturator, float heat) noexcept
    {
        const auto h = heat / 100.0f;
        saturator.setDriveAndAsymmetry (1.0f + h * heatDriveRange, h * heatMaxAsymmetry);
    }

    const float* ColorModule::heatCompensationTable()
    {
        // 对每个整数 Heat 值，用 −18 dBFS 正弦算 RMS(输入) / RMS(Tube 输出)。现在这个轻微系数只乘残差，
        // 保持高频染色强度接近原并联映射，不补偿或抬高原声。正弦在一个周期内均匀采样，
        // 与采样率无关；用交流有效值（去掉均值）——不对称带来的直流会被 5 Hz 高通滤掉，不该算进响度。
        // 标定走与音频路径同一个整段 Tube（processTubeBlock）：补偿系数对应的正是实际在用的曲线。
        // 表只取决于常数，进程内第一次 prepare 时标定一次（C++ 保证函数内静态变量线程安全地只初始化一次），
        // 之后的 prepare 直接取用（v0.5 研究 P5 R8：原先每次 prepare 都重标定，占 Color prepare 的大半）。
        static const auto table = []
        {
            std::array<float, (size_t) heatTableSize> result {};

            const auto amplitude = juce::Decibels::decibelsToGain (heatCalibrationDb);
            const auto points = (double) heatCalibrationPoints;

            std::vector<float> sine ((size_t) heatCalibrationPoints);
            double inputMeanSquare = 0.0;

            for (int n = 0; n < heatCalibrationPoints; ++n)
            {
                sine[(size_t) n] = amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * n / points);
                const auto x = (double) sine[(size_t) n];
                inputMeanSquare += x * x;
            }

            inputMeanSquare /= points;

            Saturator calibrator;
            calibrator.setCurve (Saturator::Curve::Tube);
            std::vector<float> shaped ((size_t) heatCalibrationPoints);

            for (int h = 0; h < heatTableSize; ++h)
            {
                configureTube (calibrator, (float) h);
                shaped = sine;
                calibrator.processTubeBlock (shaped.data(), heatCalibrationPoints);

                double sum = 0.0, sumOfSquares = 0.0;
                for (auto value : shaped)
                {
                    const auto y = (double) value;
                    sum += y;
                    sumOfSquares += y * y;
                }

                const auto mean = sum / points;
                const auto acMeanSquare = juce::jmax (1.0e-30, sumOfSquares / points - mean * mean);
                result[(size_t) h] = (float) std::sqrt (inputMeanSquare / acMeanSquare);
            }

            return result;
        }();

        return table.data();
    }

    //==============================================================================
    void ColorModule::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate   = spec.sampleRate;
        maxBlockSize = juce::jmax (1, (int) spec.maximumBlockSize);
        numChannels  = (int) juce::jmax ((juce::uint32) 1, spec.numChannels);

        // ---- Tone ----
        lowShelfStates.assign ((size_t) numChannels, BiquadState {});
        highShelfStates.assign ((size_t) numChannels, BiquadState {});

        // ---- Glue ----
        sidechainHighpass = designHighpass (sampleRate, glueSidechainHz, butterworthQ);
        sidechainStates.assign ((size_t) numChannels, BiquadState {});
        sidechainPeaks.assign ((size_t) maxBlockSize, 0.0);
        attackCoeff  = timeConstantCoeff (sampleRate, glueAttackMs);
        averageCoeff = timeConstantCoeff (sampleRate / glueAdaptIntervalSamples, glueAverageWindowMs);

        // ---- Heat ----
        // 8×（3 级半带）FIR 等纹波，线性相位。
        // 8× 沿用原有抗混叠设计：旧 Heat 驱动 10 时，4× 的折返实测 −56.7 dB；8× 约 −107 dB。
        // 现映射最大驱动 2、湿路 25%，仍保留同一过采样器、延迟、预热与空闲跳过行为。
        // 前两级与 Limit 的 Clip 级相同；第 3 级参数（上 0.10 / −65 dB、下 0.15 / −55 dB）是为了让三级延迟
        // 加起来正好是整数（64 样本）：湿路才能和整数延迟的干路逐样本对齐（ADR-004），不需要小数延迟补偿。
        // FIR 阶数只由归一化过渡带决定，所以这个延迟与采样率无关。
        // v0.5 起用 HalfBandOversampler（与 juce::dsp::Oversampling 同系数、同延迟，快约 10 倍）。
        heatOversampler = std::make_unique<HalfBandOversampler> ((size_t) numChannels);
        heatOversampler->addOversamplingStage (0.05f, -90.0f, 0.06f, -75.0f);
        heatOversampler->addOversamplingStage (0.10f, -80.0f, 0.12f, -70.0f);
        heatOversampler->addOversamplingStage (0.10f, -65.0f, 0.15f, -55.0f);
        heatOversampler->initProcessing ((size_t) maxBlockSize);
        oversamplingFactor = (int) heatOversampler->getOversamplingFactor();

        const auto exactHeatLatency = heatOversampler->getLatencyInSamples();
        jassert (exactHeatLatency == std::floor (exactHeatLatency));   // 必须是整数，否则干湿对不齐
        heatLatency = juce::roundToInt (exactHeatLatency);

        // 空闲跳过后的预热长度必须盖住过采样器的全部记忆，预热后的状态才与一直在跑逐位相同。
        jassert (heatPrimeLength >= heatOversampler->getMemoryInSamples());

        tube.setCurve (Saturator::Curve::Tube);
        sideTube.setCurve (Saturator::Curve::Tube);
        proxyTube.setCurve (Saturator::Curve::Tube);
        configureTube (proxyTube, 0.0f);
        heatCompensation = heatCompensationTable();
        heatHighpass = designHighpass (sampleRate, heatHighpassHz, butterworthQ);
        heatHighpassStates.assign ((size_t) numChannels, BiquadState {});
        heatGuardLow.assign ((size_t) numChannels, 0.0);
        heatGuardLowCoeff = std::exp (-juce::MathConstants<double>::twoPi * 4000.0 / sampleRate);
        heatGuardPowerCoeff = timeConstantCoeff (sampleRate, 30.0);
        heatGuardAttackCoeff = timeConstantCoeff (sampleRate, 15.0);
        heatGuardReleaseCoeff = timeConstantCoeff (sampleRate, 250.0);
        heatInputScratch.setSize (numChannels, maxBlockSize);
        heatResidualScratch.assign ((size_t) maxBlockSize * (size_t) oversamplingFactor, 0.0f);

        heatDryDelays.resize ((size_t) numChannels);
        for (auto& d : heatDryDelays)   d.prepare (heatLatency, maxBlockSize);

        // 残差makeup要用「驱动这个样本时」的 Heat 值：湿路比输入晚 heatLatency 个样本，Heat 值也延迟同样长度。
        heatValueDelay.prepare (heatLatency, maxBlockSize);
        heatDryScratch.setSize (numChannels, maxBlockSize);
        heatScratch.assign ((size_t) maxBlockSize, 0.0f);
        heatDelayedScratch.assign ((size_t) maxBlockSize, 0.0f);

        heatHistory.setSize (numChannels, heatPrimeLength);
        heatWorkScratch.setSize (numChannels, maxBlockSize);

        // 空闲时的近似残差：Heat 过采样的第 1 级（同参数）来回，延迟 49；补 15 个样本与 64 对齐。
        heatProxyOversampler = std::make_unique<HalfBandOversampler> ((size_t) numChannels);
        heatProxyOversampler->addOversamplingStage (0.05f, -90.0f, 0.06f, -75.0f);
        heatProxyOversampler->initProcessing ((size_t) maxBlockSize);

        const auto exactProxyLatency = heatProxyOversampler->getLatencyInSamples();
        jassert (exactProxyLatency == std::floor (exactProxyLatency) && exactProxyLatency <= exactHeatLatency);
        const auto proxyAlignment = heatLatency - juce::roundToInt (exactProxyLatency);

        heatProxyAlignDelays.resize ((size_t) numChannels);
        for (auto& d : heatProxyAlignDelays)   d.prepare (proxyAlignment, maxBlockSize);

        // 第 2、3 级的直流增益（理想为 1，等纹波设计差约 1e-3）：近似里少了这两级，直流阻断的状态就会差这么多倍的低频成分。
        heatProxyUpGain   = (float) (heatOversampler->getUpsamplingDcGain()   / heatProxyOversampler->getUpsamplingDcGain());
        heatProxyDownGain = (float) (heatOversampler->getDownsamplingDcGain() / heatProxyOversampler->getDownsamplingDcGain());

        // 5 Hz 一阶高通（双线性变换）：直流增益 0，奈奎斯特处增益精确为 1。
        {
            const auto k = std::tan (juce::MathConstants<double>::pi * heatDcBlockerHz / sampleRate);
            dcBlockerB0 = 1.0 / (1.0 + k);
            dcBlockerB1 = -dcBlockerB0;
            dcBlockerA1 = (k - 1.0) / (k + 1.0);
        }
        dcBlockerStates.assign ((size_t) numChannels, 0.0);

        // ---- 干路（Mix 与 Bypass 共用）----
        inputDelays.resize ((size_t) numChannels);
        for (auto& d : inputDelays)     d.prepare (heatLatency, maxBlockSize);
        inputDelayedScratch.setSize (numChannels, maxBlockSize);

        // ---- 平滑器 ----
        toneSmoothed.reset (sampleRate, smoothingSeconds);
        glueSmoothed.reset (sampleRate, smoothingSeconds);
        glueOnSmoothed.reset (sampleRate, smoothingSeconds);
        heatSmoothed.reset (sampleRate, smoothingSeconds);
        heatMixSmoothed.reset (sampleRate, smoothingSeconds);
        mixSmoothed.reset (sampleRate, smoothingSeconds);
        outputGainSmoothed.reset (sampleRate, smoothingSeconds);
        bypassMixSmoothed.reset (sampleRate, smoothingSeconds);

        reset();
    }

    void ColorModule::reset()
    {
        if (sampleRate <= 0.0)
            return;

        for (auto& s : lowShelfStates)    s = BiquadState {};
        for (auto& s : highShelfStates)   s = BiquadState {};
        for (auto& s : sidechainStates)   s = BiquadState {};
        for (auto& s : dcBlockerStates)   s = 0.0;
        for (auto& s : heatHighpassStates) s = BiquadState {};
        std::fill (heatGuardLow.begin(), heatGuardLow.end(), 0.0);
        heatGuardDryPower = heatGuardHighPower = 0.0;
        heatGuardGain = heatGuardTarget = 1.0;
        heatGuardCounter = 0;

        heatOversampler->reset();
        for (auto& d : heatDryDelays)   d.fill (0.0f);
        heatValueDelay.fill (juce::jlimit (0.0f, 100.0f, params.heat));
        for (auto& d : inputDelays)     d.fill (0.0f);

        // 清空后的过采样器状态 = 「输入一直是 0」的状态（半带 FIR 进 0 出 0，Heat 0 的 Tube 把 0 映成 0），
        // 而历史环形缓冲也清成 0：此时立即进入空闲、将来按环形缓冲预热，与一直在跑逐位相同，所以计数直接记满。
        heatHistory.clear();
        heatHistoryWrite = 0;
        heatIdleSamples = heatPrimeLength;
        heatWetIdle = false;
        heatProxyOversampler->reset();
        for (auto& d : heatProxyAlignDelays)   d.fill (0.0f);
        heatProxyRunning = false;   // 下一块若 Heat 目标为 0，先从（已清零的）环形缓冲预热再跑

        gainReductionDb = 0.0;
        averageReductionDb = 0.0;
        adaptCounter = 0;
        releaseCoeff = timeConstantCoeff (sampleRate, glueReleaseMinMs);
        toneUpdateCounter = 0;
        heatUpdateCounter = 0;

        // 平滑器直接落到当前目标，不从旧值滑过来；系数 / 阈值 / 驱动量按落定后的值重算。
        applyParamTargets (true);
        updateToneCoefficients (toneSmoothed.getCurrentValue());
        updateGlueMapping (glueSmoothed.getCurrentValue());
        updateHeatDrive (heatSmoothed.getCurrentValue());

        lastIdleState = {};
        maxGainReductionDb.store (0.0f);

        stats = {};
        blockStats = {};
    }

    //==============================================================================
    void ColorModule::setParams (const Params& newParams)
    {
        // 范围与设计规格-Color「参数」表一致。补偿表索引、阈值映射、干湿的逐位分支都依赖范围，不信任外部值。
        params.toneDb     = juce::jlimit (-3.0f, 3.0f, newParams.toneDb);
        params.glue       = juce::jlimit (0.0f, 100.0f, newParams.glue);
        params.heat       = juce::jlimit (0.0f, 100.0f, newParams.heat);
        params.mixPercent = juce::jlimit (0.0f, 100.0f, newParams.mixPercent);
        params.outputDb   = juce::jlimit (-6.0f, 6.0f, newParams.outputDb);
        params.bypass     = newParams.bypass;

        if (sampleRate <= 0.0)
            return;

        applyParamTargets (false);
    }

    void ColorModule::applyParamTargets (bool immediately)
    {
        auto set = [immediately] (auto& smoother, float value)
        {
            if (immediately)
                smoother.setCurrentAndTargetValue (value);
            else
                smoother.setTargetValue (value);
        };

        set (toneSmoothed, params.toneDb);
        // 关掉 Glue（目标 0）时不把阈值 / 补偿往 0 拉，保持关掉前的映射，只由下面的 glueOnSmoothed 整体淡出：
        // 否则补偿跟着 Glue 20 ms 内掉完、压缩量却还在慢慢释放，淡出途中会先往下陷一点（实测 −0.47 dB）。
        // 重置（immediately）时照常落到目标——那时 glueOn 也直接是 0，压缩器不起作用。
        //
        // 从「完全关掉」（淡出已走完）重新打开：压缩器此刻对输出毫无作用，就按「从没开过」起步——映射直接落到
        // 新值、压缩量包络清零，再由 glueOn 淡入。关着时检测器仍按关掉前的映射在跑、包络停在旧的压缩量上，
        // 若映射从旧值滑过去、包络从旧值释放，重开的头几百毫秒会先陷下去（GLAZE 100 → 0 → 0.1 实测多陷 1.0 dB、
        // 0.3 s 才回来；2026-09-25 评审 v0.4 R1-03）。淡出还没走完就又打开时压缩器仍在起作用，照常平滑，不跳。
        const bool glueFullyOff = glueOnSmoothed.getCurrentValue() <= 0.0f && glueOnSmoothed.getTargetValue() <= 0.0f;

        if (params.glue > 0.0f && ! immediately && glueFullyOff)
            restartGlueFromOff();
        else if (params.glue > 0.0f || immediately)
            set (glueSmoothed, params.glue);

        // Glue 拧到 0 时整个压缩器（压缩量与补偿一起）在 20 ms 里淡出，和 Heat 的开关手感一致；
        // 不这样的话补偿 20 ms 内归零、压缩量却按 50～500 ms 慢慢释放，输出会先掉一个约 6 dB 的坑、
        // 1～2.5 s 才爬回（2026-09-24 夜间评审 R1-02 实测）。
        set (glueOnSmoothed, params.glue > 0.0f ? 1.0f : 0.0f);
        set (heatSmoothed, params.heat);

        // 非线性残差从0到25%渐进加入原声。Heat0逐位取64样本干路；参数仍20ms平滑。
        set (heatMixSmoothed, heatMaxWetMix * (params.heat / 100.0f));

        // 除以 100（不是乘 0.01f）：100 / 100 精确等于 1.0f，Mix = 100% 时才能逐位取湿路。
        set (mixSmoothed, params.mixPercent / 100.0f);
        set (outputGainSmoothed, juce::Decibels::decibelsToGain (params.outputDb));
        set (bypassMixSmoothed, params.bypass ? 1.0f : 0.0f);
    }

    void ColorModule::restartGlueFromOff() noexcept
    {
        // 与 reset() 里 Glue 那部分相同：映射落到当前目标、压缩量包络与释放自适应清零。侧链高通状态保留（一直在跑）。
        glueSmoothed.setCurrentAndTargetValue (params.glue);
        updateGlueMapping (params.glue);

        gainReductionDb    = 0.0;
        averageReductionDb = 0.0;
        adaptCounter       = 0;
        releaseCoeff       = timeConstantCoeff (sampleRate, glueReleaseMinMs);
    }

    void ColorModule::updateToneCoefficients (float toneDb)
    {
        // 一个旋钮做跷跷板：高搁架 +Tone、低搁架 −Tone。Tone = 0 → 两个搁架 A = 1 → 逐位直通（见 designShelf）。
        lowShelf  = designShelf (sampleRate, toneLowShelfHz,  -(double) toneDb, butterworthQ, false);
        highShelf = designShelf (sampleRate, toneHighShelfHz,  (double) toneDb, butterworthQ, true);
        toneForCoefficients = toneDb;
    }

    void ColorModule::updateGlueMapping (float glue) noexcept
    {
        // 阈值：0 → 0 dBFS，100 → −24 dBFS；自动补偿 = (0 − 阈值)·(1 − 1/比率)·0.5。
        // Glue = 0 时两者都精确为 0——但那时压缩器整个关掉（见 processGlue），不靠这个映射。
        thresholdDb = glueThresholdMaxDb * (double) glue / 100.0;
        makeupDb    = -thresholdDb * glueSlope * 0.5;

        // 软拐点从 阈值 − W/2 开始；侧链峰值低于它时压缩量必为 0，先在线性域比较，省掉大多数样本的 log10。
        kneeStartLinear = std::pow (10.0, (thresholdDb - glueKneeDb * 0.5) / 20.0);
        glueForMapping = glue;
    }

    void ColorModule::updateHeatDrive (float heat) noexcept
    {
        configureTube (tube, heat);
        sideTube.setDriveAndAsymmetry (1.0f + (heat / 100.0f) * heatSideDriveRange, 0.0f);
        heatForTube = heat;
    }

    int ColorModule::getLatencySamples() const noexcept
    {
        // 固定 = Heat 过采样滤波器延迟（Tone、Glue 零延迟），与参数无关：Heat = 0 走的干路也是这个长度。
        return heatLatency;
    }

    float ColorModule::getAndResetGainReductionDb() noexcept
    {
        return maxGainReductionDb.exchange (0.0f);
    }

    void ColorModule::setIdleSkippingEnabled (bool shouldSkip) noexcept
    {
        // 关掉时若 Heat 湿路正在跳过，下一块 processHeat 看到「空闲条件不成立」会先预热再照常处理。
        idleSkippingEnabled = shouldSkip;
    }

    void ColorModule::setHeatDcProxyEnabled (bool shouldUseProxy) noexcept
    {
        // 关掉时：下一块起近似链停跑、直流阻断吃真实8×残差；湿路若正在跳过，同上先预热再照常处理。
        // 重新打开时：Heat 目标为 0 的那一块先从环形缓冲预热近似链（与一直在跑逐位相同）。
        heatDcProxyEnabled = shouldUseProxy;
    }

    ColorModule::IdleState ColorModule::getLastIdleState() const noexcept
    {
        return lastIdleState;
    }

    float ColorModule::heatCompensationFor (float heat) const noexcept
    {
        // 整数 Heat 值之间线性插值。
        const auto position = juce::jlimit (0.0f, (float) (heatTableSize - 1), heat);
        const auto index = (int) position;

        if (index >= heatTableSize - 1)
            return heatCompensation[heatTableSize - 1];

        const auto fraction = position - (float) index;
        const auto lower = heatCompensation[index];
        const auto upper = heatCompensation[index + 1];
        return lower + (upper - lower) * fraction;
    }

    //==============================================================================
    void ColorModule::process (juce::AudioBuffer<float>& io)
    {
        stats = {};

        if (sampleRate <= 0.0 || io.getNumSamples() == 0)
        {
            blockStats = {};
            return;
        }

        for (int start = 0; start < io.getNumSamples(); start += maxBlockSize)
            processChunk (io, start, juce::jmin (maxBlockSize, io.getNumSamples() - start));

        // v0.6 每块统计：按整个调用的样本数平均（空闲跳过的段没有累加，即按 0 计）。
        const auto numSamples = (double) io.getNumSamples();
        blockStats.glueGrMeanDb = (float) (stats.glueReductionSumDb / numSamples);
        blockStats.glueGrMaxDb  = (float) stats.glueReductionMaxDb;
        blockStats.heatDrive    = (float) (stats.heatDriveSum / numSamples);
    }

    void ColorModule::processChunk (juce::AudioBuffer<float>& io, int startSample, int numSamples)
    {
        const auto numChannelsToUse = juce::jmin (numChannels, io.getNumChannels());

        if (numChannelsToUse <= 0)
            return;

        auto* const* channels = io.getArrayOfWritePointers();

        // 1. 干路：原始输入走与整条 Color 等长（= Heat 过采样延迟）的延迟。Mix 的干路和 Bypass 的干路
        //    都是这个信号，所以一条延迟线两用。
        for (int ch = 0; ch < numChannelsToUse; ++ch)
            inputDelays[(size_t) ch].process (channels[ch] + startSample, inputDelayedScratch.getWritePointer (ch), numSamples);

        // 2. Tone → 3. Glue → 4. Heat（各自在 io 上原地处理）→ 5. Mix → Output → Bypass 交叉淡化。
        processTone (io, startSample, numSamples, numChannelsToUse);
        processGlue (io, startSample, numSamples, numChannelsToUse);
        processHeat (io, startSample, numSamples, numChannelsToUse);
        processOutput (io, startSample, numSamples, numChannelsToUse);
    }

    void ColorModule::processOutput (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse)
    {
        auto* const* channels = io.getArrayOfWritePointers();

        // 三个平滑器都静止时（绝大多数时候），每个样本拿到的值就是目标值：SmoothedValue 不在平滑时 getNextValue
        // 原样返回目标、不改状态。于是按常数处理，与逐样本调用逐位相同；常见的两种情形直接整段搬运或什么都不做。
        if (idleSkippingEnabled && ! mixSmoothed.isSmoothing() && ! outputGainSmoothed.isSmoothing()
             && ! bypassMixSmoothed.isSmoothing())
        {
            lastIdleState.output = true;

            const auto mix        = mixSmoothed.getTargetValue();
            const auto outputGain = outputGainSmoothed.getTargetValue();
            const auto bypassMix  = bypassMixSmoothed.getTargetValue();

            if (bypassMix >= 1.0f)
            {
                // 完全旁通：输出 = 延迟后的输入。
                for (int ch = 0; ch < numChannelsToUse; ++ch)
                    std::memcpy (channels[ch] + startSample, inputDelayedScratch.getReadPointer (ch), sizeof (float) * (size_t) numSamples);

                return;
            }

            if (bypassMix <= 0.0f && mix >= 1.0f && outputGain == 1.0f)
                return;   // 全湿、Output 0 dB、不旁通：湿路原样就是输出

            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                auto* out = channels[ch] + startSample;
                const auto* dry = inputDelayedScratch.getReadPointer (ch);

                for (int i = 0; i < numSamples; ++i)
                    out[i] = mixOutputBypass (out[i], dry[i], mix, outputGain, bypassMix);
            }

            return;
        }

        lastIdleState.output = false;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto mix        = mixSmoothed.getNextValue();
            const auto outputGain = outputGainSmoothed.getNextValue();
            const auto bypassMix  = bypassMixSmoothed.getNextValue();

            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                auto& out = channels[ch][startSample + i];
                out = mixOutputBypass (out, inputDelayedScratch.getReadPointer (ch)[i], mix, outputGain, bypassMix);
            }
        }
    }

    //==============================================================================
    bool ColorModule::isToneIdle (int numChannelsToUse) const noexcept
    {
        // Tone 平滑走完且目标为 0、当前系数就是 Tone = 0 的直通系数、全部状态精确为 0（±0 都算）：此时对有限且非 −0 的
        // 样本 y = 1·x + 0 = x、状态落到 +0（见 designShelf / processBiquad 的推导），整段跳过与逐样本算逐位相同；
        // 块里有 −0 或非有限值时不跳过（见 allSamplesPassThroughExactly），由 processTone 判断。
        // 拧回 0 以后，状态一旦小于信号的半个 ulp，两个样本内就精确落到 0，随即进入跳过。
        if (toneSmoothed.isSmoothing() || toneSmoothed.getTargetValue() != 0.0f || toneForCoefficients != 0.0f)
            return false;

        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            const auto& low = lowShelfStates[(size_t) ch];
            const auto& high = highShelfStates[(size_t) ch];

            if (low.s1 != 0.0 || low.s2 != 0.0 || high.s1 != 0.0 || high.s2 != 0.0)
                return false;
        }

        return true;
    }

    void ColorModule::processTone (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse)
    {
        auto* const* channels = io.getArrayOfWritePointers();

        if (idleSkippingEnabled && isToneIdle (numChannelsToUse)
             && allSamplesPassThroughExactly (channels, startSample, numSamples, numChannelsToUse))
        {
            // 系数更新计数器照常推进，重算仍落在固定的绝对样本位置上。
            toneUpdateCounter = (toneUpdateCounter + numSamples) % coefficientUpdateInterval;

            // 直通滤波器处理过哪怕一个有限、非 −0 的样本后，状态恰为 +0（此前可能是 −0）：照样落到 +0，逐位一致。
            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                lowShelfStates[(size_t) ch]  = BiquadState {};
                highShelfStates[(size_t) ch] = BiquadState {};
            }

            lastIdleState.tone = true;
            return;
        }

        lastIdleState.tone = false;

       #if CELADON_COLOR_NEON
        if (numChannelsToUse == 2)
        {
            // 两声道打包：每个样本的运算与下面的标量写法逐位相同（见 StereoBiquad）。
            StereoBiquad low, high;
            low.set  (lowShelf.b0,  lowShelf.b1,  lowShelf.b2,  lowShelf.a1,  lowShelf.a2);
            high.set (highShelf.b0, highShelf.b1, highShelf.b2, highShelf.a1, highShelf.a2);

            auto lowS1  = packPair (lowShelfStates[0].s1,  lowShelfStates[1].s1);
            auto lowS2  = packPair (lowShelfStates[0].s2,  lowShelfStates[1].s2);
            auto highS1 = packPair (highShelfStates[0].s1, highShelfStates[1].s1);
            auto highS2 = packPair (highShelfStates[0].s2, highShelfStates[1].s2);

            auto* left  = channels[0] + startSample;
            auto* right = channels[1] + startSample;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto tone = toneSmoothed.getNextValue();

                if (toneUpdateCounter == 0 && tone != toneForCoefficients)
                {
                    updateToneCoefficients (tone);
                    low.set  (lowShelf.b0,  lowShelf.b1,  lowShelf.b2,  lowShelf.a1,  lowShelf.a2);
                    high.set (highShelf.b0, highShelf.b1, highShelf.b2, highShelf.a1, highShelf.a2);
                }

                if (++toneUpdateCounter == coefficientUpdateInterval)
                    toneUpdateCounter = 0;

                auto y = low.process (loadStereo (left[i], right[i]), lowS1, lowS2);
                y = high.process (y, highS1, highS2);
                storeStereo (y, left[i], right[i]);
            }

            lowShelfStates[0]  = { vgetq_lane_f64 (lowS1, 0),  vgetq_lane_f64 (lowS2, 0) };
            lowShelfStates[1]  = { vgetq_lane_f64 (lowS1, 1),  vgetq_lane_f64 (lowS2, 1) };
            highShelfStates[0] = { vgetq_lane_f64 (highS1, 0), vgetq_lane_f64 (highS2, 0) };
            highShelfStates[1] = { vgetq_lane_f64 (highS1, 1), vgetq_lane_f64 (highS2, 1) };
        }
        else
       #endif
        {
            for (int i = 0; i < numSamples; ++i)
            {
                // 系数每 16 样本按平滑值重算（三角函数不必每样本算）；值没变就不算。计数器跨块延续，
                // 重算落在固定的绝对样本位置上，输出与块大小无关。平滑结束时 SmoothedValue 精确落在目标值，
                // Tone = 0 的最后一次重算得到逐位直通的系数。
                const auto tone = toneSmoothed.getNextValue();

                if (toneUpdateCounter == 0 && tone != toneForCoefficients)
                    updateToneCoefficients (tone);

                if (++toneUpdateCounter == coefficientUpdateInterval)
                    toneUpdateCounter = 0;

                for (int ch = 0; ch < numChannelsToUse; ++ch)
                {
                    auto& sample = channels[ch][startSample + i];
                    auto y = processBiquad ((double) sample, lowShelf, lowShelfStates[(size_t) ch]);
                    y = processBiquad (y, highShelf, highShelfStates[(size_t) ch]);
                    sample = (float) y;
                }
            }
        }

        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            auto& low = lowShelfStates[(size_t) ch];
            auto& high = highShelfStates[(size_t) ch];
            snapToZero (low.s1);  snapToZero (low.s2);
            snapToZero (high.s1); snapToZero (high.s2);
        }
    }

    //==============================================================================
    void ColorModule::computeSidechainPeaks (const float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept
    {
        // 侧链：Tone 之后的信号经 100 Hz 高通，每样本左右取绝对值最大（联动，两声道同一增益）。
        // 高通一直在跑（Glue 关着也跑）：Glue 从 0 打开时侧链里已经是连续的历史。
        // 侧链只看 Glue 施加增益之前的输入，所以整块先算完峰值、再逐样本算增益，与逐样本交替算逐位相同。
       #if CELADON_COLOR_NEON
        if (numChannelsToUse == 2)
        {
            StereoBiquad highpass;
            highpass.set (sidechainHighpass.b0, sidechainHighpass.b1, sidechainHighpass.b2, sidechainHighpass.a1, sidechainHighpass.a2);

            auto s1 = packPair (sidechainStates[0].s1, sidechainStates[1].s1);
            auto s2 = packPair (sidechainStates[0].s2, sidechainStates[1].s2);

            const auto* left  = channels[0] + startSample;
            const auto* right = channels[1] + startSample;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto filtered = highpass.process (loadStereo (left[i], right[i]), s1, s2);

                // 与标量写法同一顺序：peak 从 0 起，先左后右取 jmax（NaN 不会进峰值，同 v0.4）。
                auto peak = juce::jmax (0.0, std::abs (vgetq_lane_f64 (filtered, 0)));
                peak = juce::jmax (peak, std::abs (vgetq_lane_f64 (filtered, 1)));
                sidechainPeaks[(size_t) i] = peak;
            }

            sidechainStates[0] = { vgetq_lane_f64 (s1, 0), vgetq_lane_f64 (s2, 0) };
            sidechainStates[1] = { vgetq_lane_f64 (s1, 1), vgetq_lane_f64 (s2, 1) };
        }
        else
       #endif
        {
            for (int i = 0; i < numSamples; ++i)
            {
                double peak = 0.0;

                for (int ch = 0; ch < numChannelsToUse; ++ch)
                {
                    const auto filtered = processBiquad ((double) channels[ch][startSample + i],
                                                         sidechainHighpass, sidechainStates[(size_t) ch]);
                    peak = juce::jmax (peak, std::abs (filtered));
                }

                sidechainPeaks[(size_t) i] = peak;
            }
        }

        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            auto& state = sidechainStates[(size_t) ch];
            snapToZero (state.s1);
            snapToZero (state.s2);
        }
    }

    void ColorModule::processGlue (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse)
    {
        auto* const* channels = io.getArrayOfWritePointers();

        computeSidechainPeaks (channels, startSample, numSamples, numChannelsToUse);

        // 完全关掉（开关淡出已走完）且 Glue 值不在平滑：增益恒为精确的 1.0、表不报数；压缩量包络与释放自适应
        // 只在这时被推进，而从完全关掉重开时 restartGlueFromOff 会把它们整体清零——所以跳过它们与照常推进逐位相同。
        // 只有侧链高通必须一直跑（上面已跑）：重开时它的状态要是连续的历史。
        if (idleSkippingEnabled && ! glueOnSmoothed.isSmoothing() && glueOnSmoothed.getTargetValue() <= 0.0f
             && ! glueSmoothed.isSmoothing())
        {
            lastIdleState.glue = true;
            return;
        }

        lastIdleState.glue = false;
        double chunkMaxReductionDb = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto peak = sidechainPeaks[(size_t) i];

            // 增益计算：阈值 / 补偿跟着平滑后的 Glue 走。Glue = 0（平滑结束后）彻底关闭——压缩量目标恒为 0、
            // 补偿为 0，增益精确回到 1.0（修订 1：阈值 0 dBFS 加 6 dB 软拐点会轻压 −3 dBFS 以上的信号）。
            const auto glue = glueSmoothed.getNextValue();

            if (glue != glueForMapping)
                updateGlueMapping (glue);

            double target = 0.0;

            if (glue > 0.0f && peak > kneeStartLinear)
            {
                // 峰值封顶在 1e30：输入是 Inf 时 log10 也是 Inf，包络会算出 Inf − Inf = NaN 并永远卡住。
                const auto over = 20.0 * std::log10 (juce::jmin (peak, 1.0e30)) - thresholdDb;   // 高于阈值多少 dB（> −W/2）

                // 2:1、6 dB 软拐点：拐点内按二次曲线过渡，拐点外压掉超出部分的 (1 − 1/比率)。
                target = (2.0 * over >= glueKneeDb)
                           ? over * glueSlope
                           : glueSlope * juce::square (over + glueKneeDb * 0.5) / (2.0 * glueKneeDb);
            }

            // 压缩量在 dB 域平滑：攻击 20 ms、释放按最近 500 ms 平均压缩量自适应。
            const auto reductionDb = nextGainReductionDb (target);

            // 施加：(补偿 − 压缩量) × 开关淡化。关掉（glueOn 落到 0）时 gainDb 是精确的 ±0.0，
            // 不经过 exp，增益是精确的 1.0；打开时 20 ms 内从 1.0 过渡到压缩后的增益。
            const auto glueOn = (double) glueOnSmoothed.getNextValue();
            const auto gainDb = (makeupDb - reductionDb) * glueOn;
            const auto gain = gainDb == 0.0 ? 1.0f : (float) std::exp (gainDb * dbToNaturalLog);

            // 表只报真正作用到输出上的压缩量（开关淡化中按比例）。
            chunkMaxReductionDb = juce::jmax (chunkMaxReductionDb, reductionDb * glueOn);

            // v0.6 统计：同一个量逐样本累加（只读，dB 域，不再取对数）。
            stats.glueReductionSumDb += reductionDb * glueOn;

            for (int ch = 0; ch < numChannelsToUse; ++ch)
                channels[ch][startSample + i] *= gain;
        }

        // 旁通时压缩并没有作用到输出上（输出是干路），表不报数（评审 R1-03）。
        if (chunkMaxReductionDb > 0.0 && ! params.bypass)
            atomicMax (maxGainReductionDb, (float) chunkMaxReductionDb);

        stats.glueReductionMaxDb = juce::jmax (stats.glueReductionMaxDb, chunkMaxReductionDb);   // 统计照报（旁通与否，见 BlockStats）
    }

    double ColorModule::nextGainReductionDb (double target) noexcept
    {
        // 向更深压缩走用攻击系数，向回走用释放系数（都是 dB 域一阶）。
        if (target > gainReductionDb)
            gainReductionDb = target + (gainReductionDb - target) * attackCoeff;
        else
            gainReductionDb = target + (gainReductionDb - target) * releaseCoeff;

        // 释放尾巴是指数逼近，永远到不了精确的 0；低于 1e-6 dB 直接归零：Glue 关掉、信号落回阈值以下后
        // 增益能回到精确的 1.0（补偿为 0 时），也不会在静音里衰减成 denormal。
        if (gainReductionDb < glueSnapDb)
            gainReductionDb = 0.0;

        // 释放自适应（参照 LimiterModule::nextReleaseEnvelope 的慢段）：每 32 样本把当前压缩量记进
        // 时间常数 500 ms 的平均，平均越深释放越慢——持续压着的段落别一泵一泵地回来。
        if (++adaptCounter >= glueAdaptIntervalSamples)
        {
            adaptCounter = 0;
            averageReductionDb = gainReductionDb + (averageReductionDb - gainReductionDb) * averageCoeff;
            snapToZero (averageReductionDb);

            const auto depth = juce::jlimit (0.0, 1.0, averageReductionDb / glueDeepReductionDb);
            releaseCoeff = timeConstantCoeff (sampleRate, juce::jmap (depth, glueReleaseMinMs, glueReleaseMaxMs));
        }

        return gainReductionDb;
    }

    //==============================================================================
    double ColorModule::dcBlock (double x, double& state) const noexcept
    {
        // 5 Hz 一阶高通（转置直接 II 型）的一步：湿路与空闲时的近似驱动走同一段代码。
        const auto y = dcBlockerB0 * x + state;
        state = dcBlockerB1 * x - dcBlockerA1 * y;
        return y;
    }

    bool ColorModule::isHeatWetIdleCondition() const noexcept
    {
        // 湿路对输出毫无作用、且驱动量已停在 Heat 0：Heat 与湿路淡化都静止在 0，驱动量已按 Heat 0 设好。
        return ! heatSmoothed.isSmoothing() && heatSmoothed.getTargetValue() == 0.0f
            && ! heatMixSmoothed.isSmoothing() && heatMixSmoothed.getTargetValue() <= 0.0f
            && heatForTube == 0.0f;
    }

    void ColorModule::prepareHeatInput (const float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept
    {
        // 高通在1×始终运行，先编码M/S再滤波；不改变原声，重开Heat时IIR已经拥有完整历史。
        for (int i = 0; i < numSamples; ++i)
        {
            if (numChannelsToUse == 2)
            {
                const auto left = channels[0][startSample + i];
                const auto right = channels[1][startSample + i];
                const auto mid = left * 0.5f + right * 0.5f; // 先减半，有限大输入不因L+R先溢出
                const auto side = left * 0.5f - right * 0.5f;
                heatInputScratch.setSample (0, i, (float) processBiquad ((double) mid, heatHighpass, heatHighpassStates[0]));
                heatInputScratch.setSample (1, i, (float) processBiquad ((double) side, heatHighpass, heatHighpassStates[1]));
            }
            else
            {
                for (int ch = 0; ch < numChannelsToUse; ++ch)
                    heatInputScratch.setSample (ch, i, (float) processBiquad ((double) channels[ch][startSample + i],
                                                                             heatHighpass, heatHighpassStates[(size_t) ch]));
            }
        }
        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            snapToZero (heatHighpassStates[(size_t) ch].s1);
            snapToZero (heatHighpassStates[(size_t) ch].s2);
        }
    }

    void ColorModule::processResidualBlock (float* data, int numSamples, const Saturator& shaper) noexcept
    {
        // 同一8×链内相减，不把滤波器本身的线性频响误当染色，也不另开一条过采样干路。
        std::memcpy (heatResidualScratch.data(), data, sizeof (float) * (size_t) numSamples);
        shaper.processTubeBlock (data, numSamples);
        for (int i = 0; i < numSamples; ++i)
            data[i] -= heatResidualScratch[(size_t) i];
    }

    void ColorModule::recordHeatHistory (const float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept
    {
        // 本块高通后的M/S输入写进环形缓冲；IIR不重跑，预热只复原有限记忆的半带FIR。
        const auto count = juce::jmin (numSamples, heatPrimeLength);
        const auto skipped = numSamples - count;
        const auto position = (heatHistoryWrite + skipped) % heatPrimeLength;
        const auto firstPart = juce::jmin (count, heatPrimeLength - position);

        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            const auto* source = channels[ch] + startSample + skipped;
            auto* ring = heatHistory.getWritePointer (ch);

            std::memcpy (ring + position, source, sizeof (float) * (size_t) firstPart);
            std::memcpy (ring, source + firstPart, sizeof (float) * (size_t) (count - firstPart));
        }

        heatHistoryWrite = (heatHistoryWrite + numSamples) % heatPrimeLength;
    }

    void ColorModule::copyHeatHistory (int readPosition, int length, int numChannelsToUse) noexcept
    {
        // 从环形缓冲的 readPosition 起按时间顺序取 length 个样本到 heatWorkScratch（length ≤ 块长）。
        const auto firstPart = juce::jmin (length, heatPrimeLength - readPosition);

        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            const auto* ring = heatHistory.getReadPointer (ch);
            auto* work = heatWorkScratch.getWritePointer (ch);

            std::memcpy (work, ring + readPosition, sizeof (float) * (size_t) firstPart);
            std::memcpy (work + firstPart, ring, sizeof (float) * (size_t) (length - firstPart));
        }
    }

    void ColorModule::primeHeatOversampler (int numChannelsToUse) noexcept
    {
        // 湿路从空闲回到工作：清空过采样器，把本块之前的 heatPrimeLength 个高通M/S输入按原顺序、用 Heat0 残差
        // 重跑一遍（输出丢掉）。半带 FIR 是有限记忆（133 个 1× 样本 < 256），中间的 Tube 无记忆、这段里驱动量
        // 一直是 Heat 0（进入空闲的条件保证了这 256 个样本都在空闲条件下），所以预热后的状态与一直在跑逐位相同。
        jassert (heatForTube == 0.0f);

        // 延迟后的 Heat 值（残差makeup用）空闲时没有推：一直在跑时它此刻整条都是 0（Heat 静止在 0 已 ≥ 256 个样本，
        // 比延迟 64 长），直接填 0 就与一直在跑逐位相同。
        heatValueDelay.fill (0.0f);

        heatOversampler->reset();

        auto remaining = heatPrimeLength;
        auto readPosition = heatHistoryWrite;   // 环形缓冲里最旧的样本在写指针处

        while (remaining > 0)
        {
            const auto length = juce::jmin (remaining, maxBlockSize);
            copyHeatHistory (readPosition, length, numChannelsToUse);

            juce::dsp::AudioBlock<float> block (heatWorkScratch);
            auto subBlock = block.getSubsetChannelBlock (0, (size_t) numChannelsToUse).getSubBlock (0, (size_t) length);

            auto upsampled = heatOversampler->processSamplesUp (subBlock);

            for (size_t ch = 0; ch < upsampled.getNumChannels(); ++ch)
                processResidualBlock (upsampled.getChannelPointer (ch), (int) upsampled.getNumSamples(),
                                      upsampled.getNumChannels() == 2 && ch == 1 ? sideTube : tube);

            heatOversampler->processSamplesDown (subBlock);

            readPosition = (readPosition + length) % heatPrimeLength;
            remaining -= length;
        }
    }

    void ColorModule::runHeatProxy (int numSamples, int numChannelsToUse) noexcept
    {
        // heatWorkScratch里的高通M/S输入原地换成近似残差：已有第1级（2×）来回 + Heat0 的 Tube(x)−x，
        // 直流增益补齐第 2、3 级，再延迟到与 Heat 的 64 样本对齐。
        juce::dsp::AudioBlock<float> block (heatWorkScratch);
        auto subBlock = block.getSubsetChannelBlock (0, (size_t) numChannelsToUse).getSubBlock (0, (size_t) numSamples);

        auto upsampled = heatProxyOversampler->processSamplesUp (subBlock);

        for (size_t ch = 0; ch < upsampled.getNumChannels(); ++ch)
        {
            auto* data = upsampled.getChannelPointer (ch);
            const auto count = (int) upsampled.getNumSamples();

            for (int j = 0; j < count; ++j)
                data[j] *= heatProxyUpGain;

            processResidualBlock (data, count, proxyTube);
        }

        heatProxyOversampler->processSamplesDown (subBlock);

        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            auto* work = heatWorkScratch.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
                work[i] *= heatProxyDownGain;

            heatProxyAlignDelays[(size_t) ch].process (work, work, numSamples);
        }
    }

    void ColorModule::primeHeatProxy (int numChannelsToUse) noexcept
    {
        // 近似链开始跑：清空后用本块之前的 heatPrimeLength 个 Heat 输入预热（它的记忆 100 个样本、对齐延迟 15，都 < 256），
        // 之后它的状态与「一直在跑」逐位相同——近似值只取决于输入本身，与它从哪一块开始跑无关。
        heatProxyOversampler->reset();
        for (auto& d : heatProxyAlignDelays)   d.fill (0.0f);

        auto remaining = heatPrimeLength;
        auto readPosition = heatHistoryWrite;

        while (remaining > 0)
        {
            const auto length = juce::jmin (remaining, maxBlockSize);
            copyHeatHistory (readPosition, length, numChannelsToUse);
            runHeatProxy (length, numChannelsToUse);

            readPosition = (readPosition + length) % heatPrimeLength;
            remaining -= length;
        }
    }

    void ColorModule::processHeatWhileIdle (float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept
    {
        // 驱动量更新计数器照常推进（与一直在跑时更新点的位置一致；这段里 Heat 静止在 0，本来也不会更新）。
        // 延迟后的 Heat 值不推，重开时整条填 0（见 primeHeatOversampler）。
        heatUpdateCounter = (int) (((long long) heatUpdateCounter + (long long) numSamples * oversamplingFactor)
                                    % coefficientUpdateInterval);

        // 这时每个样本的直流阻断输入本来就是近似残差（见 processHeat），湿路淡化为 0、输出逐位是干路：
        // 与不跳过时逐位相同。
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numChannelsToUse; ++ch)
                channels[ch][startSample + i] = (float) dcBlock ((double) heatWorkScratch.getReadPointer (ch)[i],
                                                                dcBlockerStates[(size_t) ch]) * heatCompensationFor (0.0f);
            // Keep the same detector and filter history as the non-skipping
            // Heat0 path. Neither detector state is audible while mix is zero.
            protectHeatResidual (channels, startSample + i, i, numChannelsToUse, 0.0f);
            for (int ch = 0; ch < numChannelsToUse; ++ch)
                channels[ch][startSample + i] = heatDryScratch.getReadPointer (ch)[i];
        }
        for (auto& state : dcBlockerStates) snapToZero (state);
    }

    void ColorModule::processTubeSegments (juce::dsp::AudioBlock<float>& upsampled) noexcept
    {
        const auto numUpsampled = (int) upsampled.getNumSamples();
        int j = 0;

        while (j < numUpsampled)
        {
            // 驱动量 / 不对称量每 16 个过采样样本按平滑值更新一次（值没变就不算）。计数器跨块延续，
            // 更新落在固定的绝对位置上，输出与块大小无关——更新点与取值都与 v0.4 的逐样本写法相同。
            if (heatUpdateCounter == 0)
            {
                const auto heat = heatScratch[(size_t) (j / oversamplingFactor)];

                if (heat != heatForTube)
                    updateHeatDrive (heat);
            }

            // 到下一个更新点之前驱动量不变；之后的更新点若 Heat 值没变（不会更新），就并进同一段。
            // Heat 静止时整块是一段。
            auto length = juce::jmin (coefficientUpdateInterval - heatUpdateCounter, numUpsampled - j);

            while (j + length < numUpsampled
                   && heatScratch[(size_t) ((j + length) / oversamplingFactor)] == heatForTube)
                length = juce::jmin (length + coefficientUpdateInterval, numUpsampled - j);

            for (size_t ch = 0; ch < upsampled.getNumChannels(); ++ch)
                processResidualBlock (upsampled.getChannelPointer (ch) + j, length,
                                      upsampled.getNumChannels() == 2 && ch == 1 ? sideTube : tube);

            heatUpdateCounter = (heatUpdateCounter + length) % coefficientUpdateInterval;
            j += length;
        }
    }

    void ColorModule::processHeat (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse)
    {
        auto* const* channels = io.getArrayOfWritePointers();

        // 干路：Heat 的输入（Glue 之后）经等长（= 过采样滤波器延迟）延迟，Heat = 0 时就走它。
        for (int ch = 0; ch < numChannelsToUse; ++ch)
            heatDryDelays[(size_t) ch].process (channels[ch] + startSample, heatDryScratch.getWritePointer (ch), numSamples);

        // 5 Hz直流阻断记忆无限长：Heat0时两种执行模式都改吃已有2×链的近似残差，
        // 才能在跳过8×后精确接回同一状态。近似链与8×链共用高通后的M/S输入历史；
        // HP始终推进，FIR按256个历史样本预热。代理只在Heat目标为0时运行，不增加第二条工作8×链。
        const bool proxyWanted = heatDcProxyEnabled && heatSmoothed.getTargetValue() == 0.0f;

        if (proxyWanted && ! heatProxyRunning)
            primeHeatProxy (numChannelsToUse);

        heatProxyRunning = proxyWanted;

        // ---- 湿路空闲跳过（v0.5 研究 P3 R3d）----
        // 「Heat 与湿路淡化静止在 0、驱动量已是 Heat 0」连续成立 ≥ heatPrimeLength 个样本后，过采样 + Tube 整段不跑：
        // 输出本来就逐位是干路，直流阻断吃的本来就是近似残差。条件一旦不成立（Heat 打开、或关掉跳过），
        // 先用环形缓冲里本块之前的 heatPrimeLength 个输入预热过采样器（有限记忆，逐位复原），再照常处理本块。
        if (idleSkippingEnabled && heatDcProxyEnabled && isHeatWetIdleCondition())
        {
            if (! heatWetIdle)
            {
                // 只按此前已处理的Heat0历史决定是否跳过本块；当前块尚未运行，不能预支其样本数。
                // 例如333样本块的淡出刚结束时，延迟64样本的Heat尾巴可能仍非0，必须先让真实残差/DC跑完。
                heatWetIdle = heatIdleSamples >= heatPrimeLength;
                heatIdleSamples = juce::jmin (heatIdleSamples + numSamples, heatPrimeLength);
            }
        }
        else
        {
            heatIdleSamples = 0;

            if (heatWetIdle)
            {
                primeHeatOversampler (numChannelsToUse);
                heatWetIdle = false;
            }
        }

        // 先按旧历史预热，然后推进本块1×高通并记录其输出；不能用短历史重新初始化无限记忆的HP。
        prepareHeatInput (channels, startSample, numSamples, numChannelsToUse);
        recordHeatHistory (heatInputScratch.getArrayOfReadPointers(), 0, numSamples, numChannelsToUse);

        // 本块的近似残差（Heat 目标为 0 时）。
        if (heatProxyRunning)
        {
            for (int ch = 0; ch < numChannelsToUse; ++ch)
                std::memcpy (heatWorkScratch.getWritePointer (ch), heatInputScratch.getReadPointer (ch), sizeof (float) * (size_t) numSamples);

            runHeatProxy (numSamples, numChannelsToUse);
        }

        lastIdleState.heatWet = heatWetIdle;

        if (heatWetIdle)
        {
            processHeatWhileIdle (channels, startSample, numSamples, numChannelsToUse);
            return;
        }

        // Heat 量按 1× 采样率平滑，每样本存一个值：过采样环里更新驱动量用它；下采样后查补偿表用它经
        // heatLatency 延迟后的值（与湿路对齐）。不在平滑时每个样本就是目标值。
        if (heatSmoothed.isSmoothing())
        {
            for (int i = 0; i < numSamples; ++i)
                heatScratch[(size_t) i] = heatSmoothed.getNextValue();
        }
        else
        {
            std::fill (heatScratch.begin(), heatScratch.begin() + numSamples, heatSmoothed.getTargetValue());
        }

        heatValueDelay.process (heatScratch.data(), heatDelayedScratch.data(), numSamples);

        // 湿路：过采样器一直在跑（空闲时靠预热接上），Heat 从 0 打开时滤波器里已经是连续的历史，不会爆音。
        juce::dsp::AudioBlock<float> block (io);
        auto subBlock = block.getSubsetChannelBlock (0, (size_t) numChannelsToUse)
                             .getSubBlock ((size_t) startSample, (size_t) numSamples);

        juce::dsp::AudioBlock<float> heatInput (heatInputScratch);
        const auto filtered = heatInput.getSubsetChannelBlock (0, (size_t) numChannelsToUse).getSubBlock (0, (size_t) numSamples);
        auto upsampled = heatOversampler->processSamplesUp (filtered);
        processTubeSegments (upsampled);
        heatOversampler->processSamplesDown (subBlock);

        // 下采样后仅给非线性残差去直流并轻微makeup；M/S解码后加入原声，低频不经过残差高通。
        for (int i = 0; i < numSamples; ++i)
        {
            const auto compensation = heatCompensationFor (heatDelayedScratch[(size_t) i]);
            const auto mix = heatMixSmoothed.getNextValue();

            // v0.6 统计：有效 Heat = 驱动这个湿路样本的 Heat 值（已与湿路对齐）× 湿路淡化（只读）。
            stats.heatDriveSum += (double) heatDelayedScratch[(size_t) i] * (double) mix;

            // 湿路淡化为0、驱动这个残差样本的Heat也是0：直流阻断吃近似残差。
            const bool useProxy = heatProxyRunning && mix <= 0.0f && heatDelayedScratch[(size_t) i] == 0.0f;

            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                auto& sample = channels[ch][startSample + i];
                const auto dcInput = useProxy ? heatWorkScratch.getReadPointer (ch)[i] : sample;
                const auto y = dcBlock ((double) dcInput, dcBlockerStates[(size_t) ch]);

                sample = (float) y * compensation; // 此时仍是M/S残差，所有声道DC先处理完再解码
            }
            protectHeatResidual (channels, startSample + i, i, numChannelsToUse, mix);
            if (numChannelsToUse == 2)
            {
                const auto mid = channels[0][startSample + i];
                const auto side = channels[1][startSample + i] * heatSideResidualGain;
                channels[0][startSample + i] = mid + side;
                channels[1][startSample + i] = mid - side;
            }
            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                auto& sample = channels[ch][startSample + i];
                const auto dry = heatDryScratch.getReadPointer (ch)[i];
                sample = mix <= 0.0f ? dry : dry + sample * mix;
            }
        }

        for (int ch = 0; ch < numChannelsToUse; ++ch)
            snapToZero (dcBlockerStates[(size_t) ch]);
    }

    void ColorModule::protectHeatResidual (float* const* channels, int index, int dryIndex,
                                           int channelCount, float mix) noexcept
    {
        // A complementary one-pole split of ONLY the generated M/S residual.
        // The linked detector budgets high residual RMS against dry RMS; it
        // neither EQs the original mix nor independently moves the stereo image.
        double dryPower = 0.0, highPower = 0.0;
        for (int ch = 0; ch < channelCount; ++ch)
        {
            const auto x = (double) channels[ch][index];
            auto& low = heatGuardLow[(size_t) ch];
            low = x + heatGuardLowCoeff * (low - x);
            const auto high = (x - low) * (ch == 1 && channelCount == 2 ? heatSideResidualGain : 1.0f) * mix;
            highPower += high * high;
            const auto dry = (double) heatDryScratch.getReadPointer (ch)[dryIndex];
            dryPower += dry * dry;
        }
        dryPower /= channelCount;
        heatGuardDryPower = dryPower + heatGuardPowerCoeff * (heatGuardDryPower - dryPower);
        heatGuardHighPower = highPower + heatGuardPowerCoeff * (heatGuardHighPower - highPower);
        if (++heatGuardCounter == 32)
        {
            heatGuardCounter = 0;
            for (auto& low : heatGuardLow)
                if (std::abs (low) < 1.0e-30) low = 0.0;
            if (heatGuardDryPower < 1.0e-30) heatGuardDryPower = 0.0;
            if (heatGuardHighPower < 1.0e-30) heatGuardHighPower = 0.0;
            constexpr double budget = 0.035 * 0.035;
            const auto allowed = juce::jmax (1.0e-10, heatGuardDryPower * budget);
            heatGuardTarget = heatGuardEnabled && heatGuardHighPower > allowed
                ? juce::jmax (0.35, std::sqrt (allowed / heatGuardHighPower)) : 1.0;
        }
        const auto coeff = heatGuardTarget < heatGuardGain ? heatGuardAttackCoeff : heatGuardReleaseCoeff;
        heatGuardGain = heatGuardTarget + coeff * (heatGuardGain - heatGuardTarget);
        if (heatGuardTarget == 1.0 && heatGuardGain > 1.0 - 1.0e-9) heatGuardGain = 1.0;
        if (heatGuardEnabled && heatGuardGain < 1.0)
            for (int ch = 0; ch < channelCount; ++ch)
            {
                const auto low = heatGuardLow[(size_t) ch];
                channels[ch][index] = (float) (low + heatGuardGain * ((double) channels[ch][index] - low));
            }
    }
}
