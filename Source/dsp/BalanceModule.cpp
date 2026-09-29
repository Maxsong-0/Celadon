/*
  ==============================================================================

    BalanceModule.cpp

  ==============================================================================
*/

#include "BalanceModule.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

// MSVC：VS2022（v143）的 /fp:precise 默认不融合乘加；更早的工具集（v142）配 /arch:AVX2 时 /fp:precise 下仍可能融合，
// 这里写死关掉（ADR-013：各路径逐位相同的前提）。clang-cl 不认这个 pragma，由 /clang:-ffp-contract=off 与源码守卫管。
#if defined (_MSC_VER) && ! defined (__clang__)
 #pragma fp_contract (off)
#endif

// 两声道打包滤波：AArch64 用 float64x2_t（vfmaq_f64 与标量的 std::fma 是同一个融合乘加）；x64（Intel Mac、Windows）
// 用 SSE2 的 __m128d（乘、加各自舍入，与 x64 上不合并的标量写法相同——ADR-013 保证 x86 上不合并乘加；目标带 FMA
// 又没关合并时不开，免得标量被合并而打包没有）。其他平台（32 位 x86 可能用 x87、32 位 ARM）全部走标量路径。
// 输出与打包路径逐位相同的前提见 processBiquad 的注释。CELADON_FORCE_SCALAR_SIMD 只给验证用。
#if ! defined (CELADON_FORCE_SCALAR_SIMD) && defined (__ARM_NEON) && defined (__aarch64__)
 #include <arm_neon.h>
 #define CELADON_BALANCE_PACKED_NEON 1
 #define CELADON_BALANCE_PACKED_SSE2 0
#elif ! defined (CELADON_FORCE_SCALAR_SIMD) && (defined (__x86_64__) || defined (_M_X64)) && ! defined (_M_ARM64EC) \
       && ! (defined (__FMA__) && ! defined (CELADON_FP_CONTRACT_OFF))
 #include <emmintrin.h>
 #define CELADON_BALANCE_PACKED_NEON 0
 #define CELADON_BALANCE_PACKED_SSE2 1
#else
 #define CELADON_BALANCE_PACKED_NEON 0
 #define CELADON_BALANCE_PACKED_SSE2 0
#endif

namespace celadon
{
    namespace
    {
        constexpr double smoothingSeconds          = 0.02;    // 旁通交叉淡化 20 ms（设计规格-总体「参数」）
        constexpr int    coefficientUpdateInterval = 64;      // 增益每 64 样本重算一次系数（设计规格-Balance「执行」）

        // 分析窗随采样率（计划「细节」表）：48 kHz 以下 2048 点，采样率每翻一倍点数翻一倍，窗长约 43 ms 不变；
        // hop = 点数 / 4（≈ 10.7 ms）。点数与 hop 都是 64 的倍数，hop 边界必然落在系数重算的网格上。
        constexpr int    baseFftOrder   = 11;      // 2048 点
        constexpr double baseSampleRate = 48000.0;
        constexpr int    hopDivisor     = 4;

        // 27 个 1/3 倍频程段：第 14 段正好 1 kHz，精确中心 1000 × 2^((b − 14) / 3)、段边界 = 中心 × 2^(±1/6)
        // （相邻段边界严丝合缝，不漏 bin 也不重复）。显示给界面的是 IEC 61260 的标称值（40, 50, 63 …），
        // 与精确值最多差 1.6 %（12500 对 12699），肉眼看不出；分析与滤波器一律用精确值。
        constexpr int   referenceBand = 14;
        constexpr double referenceHz  = 1000.0;
        constexpr float nominalCentresHz[] = { 40.0f, 50.0f, 63.0f, 80.0f, 100.0f, 125.0f, 160.0f, 200.0f, 250.0f,
                                               315.0f, 400.0f, 500.0f, 630.0f, 800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f,
                                               2500.0f, 3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f, 16000.0f };
        static_assert (sizeof (nominalCentresHz) / sizeof (nominalCentresHz[0]) == (size_t) BalanceModule::numBands,
                       "27 band labels");

        // 时间常数（设计规格-Balance「分析」3、4）：下标 = Speed（Slow / Mid / Fast）。
        constexpr double attackMs[]     = { 200.0, 80.0, 30.0 };
        constexpr double releaseMs[]    = { 800.0, 400.0, 150.0 };
        constexpr double referenceSec[] = { 4.0, 2.0, 1.0 };

        // 每段电平估计的目标统计自由度（v0.3 实现修订，见 BalanceModule.cpp 头部说明与 REPORT-B1）：
        // 单帧 FFT 里 40 ～ 100 Hz 段只有一个 bin，能量服从 2 自由度的 χ²，逐帧读数标准差约 5.6 dB，
        // 远超 ±1 dB 死区；规格验收 2「粉噪下 |G| < 0.5 dB」照字面参数过不了。补救：段能量在功率域做
        // Welch 式时间平均，时间常数与该段的有效带宽成反比（τ = 自由度 / (4 · 带宽)），让每段的估计
        // 都有同样的统计可靠度（相对标准差 ≈ 1 / √(自由度 / 2)：200 → 0.1 → ±0.43 dB）。
        // 宽段（≥ 2 kHz）一帧本来就够，τ 短于一个 hop，几乎不加延迟；40 Hz 段则要 1.4 s（Mid）——
        // 这是物理极限：9 Hz 宽的频段每秒只提供约 70 个自由度，测不快。Slow / Fast 相应放宽 / 收紧。
        constexpr double measurementDegreesOfFreedom[] = { 300.0, 200.0, 100.0 };
        constexpr double hannBinEquivalentBandwidth   = 1.5;   // Hann 窗单个 bin 的等效噪声带宽（bin 间距的倍数）

        // 参考轮廓：频率方向 ±3 段加权 [1,2,3,4,3,2,1]/16，越界与静音门下的段丢掉权重后重新归一（计划「细节」表）。
        constexpr double kernel[] = { 1.0, 2.0, 3.0, 4.0, 3.0, 2.0, 1.0 };
        constexpr int    kernelRadius = 3;

        // Brighten 独立于动态修正：1 kHz 为支点，满刻度在 100 Hz / 10 kHz 的段目标为 −3 / +3 dB。
        // 这是各滤波器的目标；相邻峰值滤波器响应会重叠，不等于整条链的精确频响。
        constexpr double tiltDbPerDecade = 3.0;

        // 判断（设计规格-Balance「判断」）。
        constexpr double deadZoneDb      = 1.0;
        constexpr double maxCorrectionDb = 6.0;
        constexpr double maxTameCorrectionDb = 8.0;
        constexpr double maxStepPerHopDb = 1.0;
        constexpr double gateDb          = -60.0;
        constexpr double gateFadeDb      = 6.0;   // 门上 6 dB 内修正量与参考权重线性淡入：门口来回穿越时不跳变

        // 自动 Recover 只能填有两侧内容的持续缺口，不能把歌曲本身的高/低频滚降填平。
        // 150 ms 内不提升，随后渐入，500 ms 满额；与原来的每 hop 限速/增益插值一起平滑。
        constexpr double automaticRecoverStartSeconds = 0.15;
        constexpr double automaticRecoverFullSeconds  = 0.50;
        constexpr double automaticRecoverSupportDb    = 1.0;

        // 自动 Tame 仅处理有两侧宽带内容的持续局部峰。起音时重新等待，短脉冲不会积累成长期共振。
        // 判定全在固定 hop 网格上，复用原始功率，不增加 FFT、音频延迟或分配。
        constexpr double automaticTameStartSeconds = 0.18;
        constexpr double automaticTameFullSeconds  = 0.45;
        constexpr double automaticTameOnsetHoldSeconds = 0.06;
        constexpr double automaticTameOnsetPowerRatio  = 4.0;  // 相邻原始帧上升 6 dB
        constexpr double automaticTamePresentRatio = 0.5;
        constexpr double automaticTameSupportRatio = 1.0e-4;   // 两侧各至少两段，在本段之下 40 dB 内且高于门限
        constexpr double automaticTameMaxExtraDb   = 2.5;

        // 上行掩蔽转移（Recover 与 Tame 同时 > 0 才启用）。斜率按 1/3 倍频程：低频掩蔽更高的邻段，
        // 反向不在这里补。间隙大于 16 dB 的当作泄漏或滚降，不抬。端点与 6.3 kHz 以上不接收新的提升。
        constexpr int    maskingMaxDistance    = 4;
        constexpr int    maskingLowBand        = 2;
        constexpr int    maskingHighBand       = 22;
        constexpr double maskingFloorOffsetDb  = 2.0;
        constexpr double maskingUpwardSlopeDb  = 4.0;
        constexpr double maskingMinBuriedDb    = 0.70;
        constexpr double maskingBoostSeconds   = 0.18;
        constexpr double maskingMinCutDb       = 0.80;
        constexpr double maskingMaxGapDb       = 16.0;
        constexpr double maskingTransfer       = 0.35;
        constexpr double maskingMaxBoostDb     = 1.25;
        constexpr double maskingAnchorMinBoost = 0.12;
        constexpr double maskingAnchorLimitDb  = 0.45;

        // A 计权功率权重，1 kHz 附近为 1。只用来把掩蔽转移的净增益拉回 0 附近，不单独做均衡。
        double maskingLoudnessWeight (int band)
        {
            const auto hz = 1000.0 * std::pow (2.0, ((double) band - 14.0) / 3.0);
            const auto f2 = hz * hz;
            const auto f4 = f2 * f2;
            constexpr double c1 = 12194.217 * 12194.217;
            const auto numerator = c1 * f4;
            const auto denominator = (f2 + 424.36) * std::sqrt ((f2 + 11599.29) * (f2 + 544496.41)) * (f2 + c1);
            const auto db = 20.0 * std::log10 (numerator / denominator) + 2.0;
            return std::pow (10.0, juce::jmax (-14.0, db) / 10.0);
        }

        // 不把单条持久谱线直接等同于问题。仅当同一声道的清晰谱峰构成 1:2:3 关系时保护这三段；
        // 低于六个 FFT bin 的基音分辨率不足，或泛音比基音弱超过约 17 dB，都不据此推断。
        // 保留 25% 自动许可，因此谐波相关也不是完全豁免；手动 Tame 始终按原曲线执行。
        constexpr double harmonicMinimumRootBin = 6.0;
        constexpr double harmonicMaximumRootHz = 2000.0;
        constexpr double harmonicPeakContrast = 12.0;
        constexpr double harmonicMinimumRelativePower = 0.02;
        constexpr double harmonicMaximumRelativePower = 4.0;
        constexpr double harmonicProtectionAmount = 0.75;
        constexpr double harmonicProtectionAttackSeconds = 0.06;
        constexpr double harmonicProtectionReleaseSeconds = 0.30;

        constexpr int    referenceLockHops  = 8;         // 段复活后参考锁在偏差 0 的 hop 数（≈ 85 ms：一整窗 + Mid 攻击时间）

        // 共同电平 C 的输入：所有段同一个 60 ms 对称功率平均（不分 Speed、不过攻击 / 释放）。C 只需要知道
        // 「各段一起动了多少」，若用各段自己的测量（40 Hz 段 τ 1.4 s、16 kHz 段十几毫秒），整体电平一变，
        // Huber 估计会落在快慢两群之间、被慢段拖着爬，全段先被宽带 Tame / Recover 2 ～ 5 dB、持续 0.4 ～ 3 s
        // （2026-09-25 评审 R1-01 实测）。单段 60 ms 的估计噪声大，27 段的稳健位置估计把它再压约 √27；
        // 稳态下参考 R = S + C 与 C 的输入无关（S 追的是「抹平后的 Lfast − C」），只改变瞬态。
        constexpr double commonLevelSeconds = 0.06;

        constexpr double powerFloor         = 1.0e-30;   // 10·log10 → −300 dB，数字静音也有个有限的电平

        // 滤波器状态的归零门槛（每次系数重算时检查，从 reset 起算的固定网格上，归零时刻与块大小无关）。
        //  - 平时 1e-30：静音里状态衰减到这里就归零，不会衰减成 denormal。
        //  - 本次插值增益精确为 0 的段 1e-10（约 −200 dBFS）：这时系数就是直通，状态只是开过修正留下的余振，
        //    按极点自然衰减；但有信号时每个样本的舍入误差会被贴着单位圆的极点反复放大，状态停在约 1e-13 上下，
        //    永远到不了 1e-30——v0.4 实测 Recover / Tame（或 GLAZE）拧回 0 之后 mix 素材 26 s 还有 5 段在跑、
        //    输出也不再逐位等于输入（2026-09-25 研究 P4 R4）。1e-10 比这层舍入噪声高 3 个数量级，又比任何有意义的
        //    余振低得多：归零那一刻改变的量 ≤ 1e-10，之后该段精确直通、不再计算。拧回 0 后约 1 s 内所有段都停下。
        constexpr double stateSnapThreshold         = 1.0e-30;
        constexpr double zeroGainStateSnapThreshold = 1.0e-10;

        // 时间常数 → 一阶系数：每步 y = target + (y − target) · coeff（步长 = 一个 hop）。
        double timeConstantCoeff (double stepSeconds, double tauSeconds)
        {
            return tauSeconds <= 0.0 ? 0.0 : std::exp (-stepSeconds / tauSeconds);
        }

        // 全段共同电平的稳健估计：Huber 型位置估计（软中位数）。每轮 c += Σ w·clip(x − c, ±k) / Σ w：
        // 离 c 不到 k 的值按加权平均算，更远的只算 ±k —— 少数离群的段（贝斯进来、扫频里的纯音）对 c 的影响
        // 封顶在 k / n；与真正的中位数不同，它对每个值、每个权重都连续，段淡入淡出或值穿过中间都不会让 c 跳。
        // 从上一 hop 的 c 出发迭代 8 轮：每轮最多走 k，整体跳 10 dB 一个 hop 内就跟上。没有段就保持不变。
        constexpr double commonLevelClipDb    = 2.0;
        constexpr int    commonLevelIterations = 8;

        double robustLocation (const double* values, const double* weights, int count, double start) noexcept
        {
            auto c = start;

            for (int iteration = 0; iteration < commonLevelIterations; ++iteration)
            {
                double numerator = 0.0, denominator = 0.0;

                for (int i = 0; i < count; ++i)
                {
                    numerator   += weights[i] * juce::jlimit (-commonLevelClipDb, commonLevelClipDb, values[i] - c);
                    denominator += weights[i];
                }

                if (denominator <= 0.0)
                    return start;

                c += numerator / denominator;
            }

            return c;
        }
    }

    //==============================================================================
    double BalanceModule::processBiquad (double x, const BiquadCoefficients& c, BiquadState& s) noexcept
    {
        // 转置直接 II 型。乘积先落到具名变量再相减（同 ColorModule）：G = 0 时 b1 == a1、b2 == a2，
        // y == x，两个逐位相同的乘积之差精确为 0，状态从 0 出发恒为 0 —— 逐位直通。
        //
        // y = b0·x + s1 是整个双二阶里唯一一处乘加融合：编译器默认 -ffp-contract=on，v0.4 这一行在 AArch64 上
        // -O0 ～ -O3 编出来都是一条 fmadd（2026-09-25 反汇编核对），其余各步都是具名中间量、各自舍入。AArch64 上这里
        // 写成显式 std::fma——还是那一条 fmadd，结果与 v0.4 逐位相同，而且不再依赖编译选项；打包路径的
        // vfmaq_f64 是同一个融合乘加，所以两声道打包与标量逐位相同。其他平台保持原写法：x64 上这一行不合并（ADR-013），
        // SSE2 打包路径照样先乘后加、各自舍入，两者逐位相同；再其他平台单声道与多声道都调这一个函数，永远与自己一致。
       #if CELADON_BALANCE_PACKED_NEON
        const auto y = std::fma (c.b0, x, s.s1);
       #else
        const auto y = c.b0 * x + s.s1;
       #endif

        const auto bx1 = c.b1 * x;
        const auto ay1 = c.a1 * y;
        s.s1 = (bx1 - ay1) + s.s2;

        const auto bx2 = c.b2 * x;
        const auto ay2 = c.a2 * y;
        s.s2 = bx2 - ay2;

        return y;
    }

    void BalanceModule::snapToZero (double& value, double threshold) noexcept
    {
        if (std::abs (value) < threshold)
            value = 0.0;
    }

    double BalanceModule::exactBandCentreHz (int band) noexcept
    {
        return referenceHz * std::pow (2.0, (double) (band - referenceBand) / 3.0);
    }

    float BalanceModule::getBandCentreHz (int band) noexcept
    {
        return nominalCentresHz[(size_t) juce::jlimit (0, numBands - 1, band)];
    }

    //==============================================================================
    void BalanceModule::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate   = spec.sampleRate;
        maxBlockSize = juce::jmax (1, (int) spec.maximumBlockSize);
        numChannels  = (int) juce::jmax ((juce::uint32) 1, spec.numChannels);

        // ---- FFT 点数 = 2048 × ceil(fs / 48000) 取 2 的幂：44.1 / 48 k → 2048，96 k → 4096，192 k → 8192 ----
        int order = baseFftOrder;
        while ((double) (1 << (order - baseFftOrder)) * baseSampleRate < sampleRate)
            ++order;

        fftSize    = 1 << order;
        hopSize    = fftSize / hopDivisor;
        hopSeconds = (double) hopSize / sampleRate;
        fft        = std::make_unique<juce::dsp::FFT> (order);

        // ---- Hann 窗与功率谱归一 ----
        // 单个 bin 的 |X|² 乘 2 / (N · Σw²) 就是「该 bin 一格频率（fs / N）内的均方功率」：白噪 σ² 时
        // 每个 bin 的期望正好是 σ² · 2 / N，正弦主瓣三个 bin 相加正好是正弦的均方值。这样段电平就是
        // 「该段内信号的均方值（dBFS）」，静音门的 −60 dBFS 才有绝对意义。
        window.assign ((size_t) fftSize, 0.0f);
        double sumOfSquaredWindow = 0.0;

        for (int n = 0; n < fftSize; ++n)
        {
            const auto w = 0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * (double) n / (double) fftSize));
            window[(size_t) n] = (float) w;
            sumOfSquaredWindow += w * w;
        }

        spectrumScale = 2.0 / ((double) fftSize * sumOfSquaredWindow);

        analysisRing.assign ((size_t) numChannels * (size_t) fftSize, 0.0f);
        frameBuffers.assign ((size_t) numChannels * 2 * (size_t) fftSize, 0.0f);
        powerSpectrum.assign ((size_t) (fftSize / 2 + 1), 0.0);
        frameRing.assign ((size_t) frameRingCapacity, FrameSnapshot {});

        // ---- 段 → bin、每段的时间平均、倾斜、滤波器常数 ----
        const auto binHz          = sampleRate / (double) fftSize;
        const auto halfBandRatio  = std::pow (2.0, 1.0 / 6.0);
        const auto thirdOctaveRatio = std::pow (2.0, 1.0 / 3.0);
        const auto filterQ        = std::sqrt (thirdOctaveRatio) / (thirdOctaveRatio - 1.0);   // ≈ 4.318
        const auto highestBin     = fftSize / 2;

        binWeights.clear();
        binWeights.reserve ((size_t) highestBin + 2 * (size_t) numBands);

        for (int band = 0; band < numBands; ++band)
        {
            const auto b      = (size_t) band;
            const auto centre = exactBandCentreHz (band);
            const auto low    = centre / halfBandRatio;
            const auto high   = centre * halfBandRatio;

            // 每个 bin 的功率看成均匀铺在它的频率格 [f_k − Δ/2, f_k + Δ/2] 里，段能量 = 段范围 [low, high) 内
            // 各格功率的积分：整格权重 1，边界格按重叠宽度 / Δ 计权，比 bin 还窄的段（48 k / 2048 点时 40 ～ 100 Hz）
            // 就是盖住它的那一格的功率 × 段宽 / Δ。这样白噪 / 粉噪下段电平 = 功率谱密度 × 段带宽（不随 bin 落点
            // 忽高忽低），正弦落在段内时能量也一个不漏（主瓣三个 bin 都在段内整格里）。直流 bin 永不参与。
            const auto first = juce::jmax (1, (int) std::floor (low / binHz - 0.5) + 1);
            const auto last  = juce::jmin (highestBin, (int) std::ceil (high / binHz + 0.5) - 1);

            bandFirstBin[b]     = first;
            bandNumBins[b]      = juce::jmax (0, last - first + 1);
            bandWeightOffset[b] = (int) binWeights.size();

            for (int k = first; k <= last; ++k)
            {
                const auto cellLow  = ((double) k - 0.5) * binHz;
                const auto cellHigh = ((double) k + 0.5) * binHz;
                const auto overlap  = juce::jmax (0.0, juce::jmin (high, cellHigh) - juce::jmax (low, cellLow));
                binWeights.push_back (spectrumScale * overlap / binHz);
            }

            // 有效带宽：段本身的带宽，但不小于 Hann 窗一个 bin 的等效噪声带宽（单 bin 段实际测到的是这么宽）。
            const auto bandwidthHz = high - low;
            const auto effectiveBandwidthHz = juce::jmax (bandwidthHz, hannBinEquivalentBandwidth * binHz);

            for (int speed = 0; speed < numSpeeds; ++speed)
            {
                const auto tau = measurementDegreesOfFreedom[speed] / (4.0 * effectiveBandwidthHz);
                preAverageCoeff[(size_t) speed][b] = timeConstantCoeff (hopSeconds, tau);
            }

            tiltPerBrighten[b] = tiltDbPerDecade * std::log10 (centre / referenceHz);

            // RBJ 峰值滤波器的与增益无关的部分；中心钳在 0.45 fs 以下（16 kHz 段在 44.1 kHz 下也够）。
            const auto w0 = juce::MathConstants<double>::twoPi * juce::jmin (centre, 0.45 * sampleRate) / sampleRate;
            cosW0[b] = std::cos (w0);
            alpha[b] = std::sin (w0) / (2.0 * filterQ);
        }

        // ---- 跟踪时间常数（按 hop 一次）----
        for (int speed = 0; speed < numSpeeds; ++speed)
        {
            const auto s = (size_t) speed;
            attackCoeff[s]    = timeConstantCoeff (hopSeconds, attackMs[s] * 0.001);
            releaseCoeff[s]   = timeConstantCoeff (hopSeconds, releaseMs[s] * 0.001);
            referenceCoeff[s] = timeConstantCoeff (hopSeconds, referenceSec[s]);
        }

        quickCoeff = timeConstantCoeff (hopSeconds, commonLevelSeconds);

        // 状态按「段 × 声道」排：同一段相邻两个声道的 {s1, s2} 挨在一起，打包路径一条 vld2q_f64 读出两个声道。
        static_assert (std::is_standard_layout<BiquadState>::value && sizeof (BiquadState) == 2 * sizeof (double),
                       "BiquadState 必须正好是两个紧挨着的 double（打包路径按 {s1, s2, s1, s2} 读写）");
        filterStates.assign ((size_t) numBands * (size_t) numChannels, BiquadState {});
        dryScratch.setSize (numChannels, maxBlockSize);

        bypassMixSmoothed.reset (sampleRate, smoothingSeconds);

        reset();
    }

    void BalanceModule::reset()
    {
        if (sampleRate <= 0.0)
            return;

        std::fill (analysisRing.begin(), analysisRing.end(), 0.0f);
        std::fill (powerSpectrum.begin(), powerSpectrum.end(), 0.0);
        ringWriteIndex   = 0;
        analysedChannels = numChannels;

        // 第一次分析等 FIFO 填满一整窗（否则前 3/4 窗是零，电平严重偏低，参考线要慢慢爬 2 s）。
        samplesUntilAnalysis          = fftSize;
        samplesUntilCoefficientUpdate = 0;
        samplesSinceHop               = 0;

        // 没有做到一半的分析帧。
        nextAnalysisSlice     = -1;
        samplesUntilNextSlice = 0;
        analysisSliceSpacing  = 0;
        frameChannels         = numChannels;
        frameParams           = params;
        frameAppliedAmount    = 1.0f;

        // 每帧快照的序号与时间轴从零开始（与两块表一起 reset，时间戳对齐）。
        processedSamples = 0;
        framesWritten    = 0;
        frameEndSample   = 0;

        const auto floorDb = 10.0 * std::log10 (powerFloor);

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            bandPower[b]        = 0.0;
            bandCross[b]        = 0.0;
            averagedPower[b]    = 0.0;
            hopsAlive[b]        = 0;
            bandAlive[b]        = false;
            bandRevived[b]      = false;
            hopsSinceRevival[b] = 0;
            levelDb[b]          = floorDb;
            fastLevelDb[b]      = floorDb;
            referenceShapeDb[b] = 0.0;
            referencePower[b]   = 0.0;
            referenceDb[b]      = floorDb;
            smoothedLevelDb[b]  = floorDb;
            quickPower[b]       = 0.0;
            quickLevelDb[b]     = floorDb;
            laggedCommonPower[b] = 1.0;         // 10^(0 / 10)：与 commonLevelDb = 0 对应
            laggedCommonDb[b]    = 0.0;
            locationValues[b]   = 0.0;
            locationWeights[b]  = 0.0;
            gateWeight[b]       = 0.0;
            targetGainDb[b]     = 0.0;
            automaticDeficitSeconds[b] = 0.0;
            automaticPeakSeconds[b] = 0.0;
            automaticOnsetHoldSeconds[b] = 0.0;
            harshEnvelopeDb[b] = masteringTargetDb[b] = masteringPreviousPower[b] = masteringOnsetHold[b] = 0.0;
            publishedTargetDb[b].store (0.0f, std::memory_order_relaxed);
            maskingBoostDb[b] = 0.0;
            harmonicProtection[b] = 0.0;
            previousBandPower[b] = 0.0;
            hopStartGainDb[b]   = 0.0;
            gainForCoefficients[b] = 0.0;
            activeBands[b]      = 0;
            designPeak ((int) b, 0.0);
            publishedGainsDb[b].store (0.0f, std::memory_order_relaxed);
        }

        masteringHasTail = false;
        publishedHarshDb.store (0.0f, std::memory_order_relaxed);
        numActiveBands = 0;
        commonLevelDb = 0.0;

        for (auto& s : filterStates)
            s = BiquadState {};

        applyBypassTarget (true);
    }

    //==============================================================================
    void BalanceModule::setParams (const Params& newParams)
    {
        // 范围与设计规格-Balance「参数」表一致；封顶、倾斜与 Speed 下标都依赖范围，不信任外部值。
        params.recover  = std::isfinite (newParams.recover) ? juce::jlimit (0.0f, 100.0f, newParams.recover) : 0.0f;
        params.automaticRecover = std::isfinite (newParams.automaticRecover)
                                    ? juce::jlimit (0.0f, params.recover, newParams.automaticRecover) : 0.0f;
        params.tame     = std::isfinite (newParams.tame) ? juce::jlimit (0.0f, 100.0f, newParams.tame) : 0.0f;
        params.automaticTame = std::isfinite (newParams.automaticTame)
                                 ? juce::jlimit (0.0f, params.tame, newParams.automaticTame) : 0.0f;
        params.brighten = std::isfinite (newParams.brighten) ? juce::jlimit (-100.0f, 100.0f, newParams.brighten) : 0.0f;
        params.speed    = static_cast<Speed> (juce::jlimit (0, numSpeeds - 1, static_cast<int> (newParams.speed)));
        params.bypass   = newParams.bypass;
        const auto unit = [] (float value) { return std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : 0.0f; };
        params.harshAmount = unit (newParams.harshAmount);
        params.transientPreserve = unit (newParams.transientPreserve);
        params.targetAmount = unit (newParams.targetAmount);
        params.targetSlopeDbPerOct = std::isfinite (newParams.targetSlopeDbPerOct)
                                      ? juce::jlimit (-3.0f, 1.0f, newParams.targetSlopeDbPerOct) : 0.0f;
        params.hasTargetReference = newParams.hasTargetReference;
        for (size_t b = 0; b < params.targetReferenceDb.size(); ++b)
            params.targetReferenceDb[b] = std::isfinite (newParams.targetReferenceDb[b])
                                            ? juce::jlimit (-120.0f, 30.0f, newParams.targetReferenceDb[b]) : -120.0f;

        if (sampleRate <= 0.0)
            return;

        applyBypassTarget (false);
    }

    void BalanceModule::applyBypassTarget (bool immediately)
    {
        const auto target = params.bypass ? 1.0f : 0.0f;

        if (immediately)
            bypassMixSmoothed.setCurrentAndTargetValue (target);
        else
            bypassMixSmoothed.setTargetValue (target);
    }

    void BalanceModule::getBandGainsDb (BandGains& out) const noexcept
    {
        for (size_t b = 0; b < (size_t) numBands; ++b)
            out[b] = publishedGainsDb[b].load (std::memory_order_relaxed);
    }

    int BalanceModule::getActiveFilterCount() const noexcept
    {
        return numActiveBands;
    }

    // v0.6 GLAZE 每帧快照（设计规格-GLAZE-分析器第 4 节）：写者（片 2）与读者都在调用 process 的线程上，不需要原子量。
    int BalanceModule::readFramesSince (std::int64_t& cursor, FrameSnapshot* out, int maxCount) const noexcept
    {
        if (out == nullptr || maxCount <= 0 || frameRing.empty())
            return 0;

        auto from = cursor;
        bool dropped = false;

        // 游标比已发布的帧数还靠后：模块被 reset 过而读者没跟着清。从头读，按「丢过」报告。
        if (from > framesWritten || from < 0)
        {
            from = 0;
            dropped = true;
        }

        // 落后超过容量：最旧的那些已被覆盖，跳到最旧可用的一帧。
        const auto oldestAvailable = juce::jmax ((std::int64_t) 0, framesWritten - (std::int64_t) frameRingCapacity);

        if (from < oldestAvailable)
        {
            from = oldestAvailable;
            dropped = true;
        }

        const auto count = (int) juce::jmin ((std::int64_t) maxCount, framesWritten - from);

        for (int i = 0; i < count; ++i)
            out[i] = frameRing[(size_t) ((from + i) % frameRingCapacity)];

        cursor = from + count;
        return dropped ? -count : count;
    }

    void BalanceModule::publishFrameSnapshot() noexcept
    {
        // 本帧原始段功率（measureBandPowers 刚算完、还没进任何时间平均）：线性与 dB 两份，时间戳取片 0（hop 边界）时的；
        // v0.1.7.0 起连同同一帧的左右交叉功率（measureBandCross）。只读 bandPower / bandCross，不碰任何跟踪 / 判断状态——
        // G 与输出逐位不变；拆片与否，发布的内容逐位相同。
        // bandCross 已钳在 ±bandPower 内，double → float 的舍入单调（且对称），所以快照里 |bandCross| ≤ bandPower 照样成立。
        if (frameRing.empty())
            return;

        auto& slot = frameRing[(size_t) (framesWritten % frameRingCapacity)];

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            slot.bandPower[b]   = (float) bandPower[b];
            slot.bandPowerDb[b] = (float) (10.0 * std::log10 (juce::jmax (powerFloor, bandPower[b])));
            slot.bandCross[b]   = (float) bandCross[b];
        }

        slot.endSample = frameEndSample;
        ++framesWritten;
    }

    //==============================================================================
    void BalanceModule::process (juce::AudioBuffer<float>& io)
    {
        if (sampleRate <= 0.0 || io.getNumSamples() == 0)
            return;

        for (int start = 0; start < io.getNumSamples(); start += maxBlockSize)
            processChunk (io, start, juce::jmin (maxBlockSize, io.getNumSamples() - start));
    }

    void BalanceModule::processChunk (juce::AudioBuffer<float>& io, int startSample, int numSamples)
    {
        const auto numChannelsToUse = juce::jmin (numChannels, io.getNumChannels());

        if (numChannelsToUse <= 0)
            return;

        // 1. 干路 = 输入本身（零延迟），旁通交叉淡化用。
        for (int ch = 0; ch < numChannelsToUse; ++ch)
            dryScratch.copyFrom (ch, 0, io, ch, startSample, numSamples);

        // 2. 按「到下一次系数重算 / 下一次 hop 分析 / 下一片分析」切成最长 64 样本的小段：分析、各分析片与重算都落在
        //    从 reset 起算的固定绝对位置上，输出与块大小无关。同一个样本上的先后：hop 分析片 0 → 到点的后续分析片 →
        //    系数重算。hop 边界上那次重算的插值比例是 0，只用到片 0 提交的插值起点；新目标由最后一片在 hop 后第 64 个
        //    样本算好，紧接着那一次重算第一次用到它——所以把分析摊开做，G 与输出都与一口气做完逐位相同。
        int offset = 0;

        while (offset < numSamples)
        {
            if (samplesUntilAnalysis == 0)
            {
                beginAnalysis();
                samplesUntilAnalysis = hopSize;
                samplesSinceHop = 0;
            }

            while (nextAnalysisSlice > 0 && samplesUntilNextSlice == 0)
                runNextAnalysisSlice();

            if (samplesUntilCoefficientUpdate == 0)
            {
                updateCoefficients();
                samplesUntilCoefficientUpdate = coefficientUpdateInterval;
            }

            auto length = juce::jmin (numSamples - offset, samplesUntilAnalysis, samplesUntilCoefficientUpdate);

            if (nextAnalysisSlice > 0)
                length = juce::jmin (length, samplesUntilNextSlice);

            pushIntoAnalysisRing (io, startSample + offset, length, numChannelsToUse);
            processSegment (io, startSample + offset, length, numChannelsToUse);

            samplesUntilAnalysis          -= length;
            samplesUntilCoefficientUpdate -= length;
            samplesSinceHop               += length;
            processedSamples              += length;
            offset                        += length;

            if (nextAnalysisSlice > 0)
                samplesUntilNextSlice -= length;
        }

        // 3. 旁通交叉淡化（只在开 / 关的 20 ms 里真正混合；完全旁通逐位等于输入，完全打开逐位等于处理后信号）。
        auto* const* channels = io.getArrayOfWritePointers();

        for (int i = 0; i < numSamples; ++i)
        {
            const auto mix = bypassMixSmoothed.getNextValue();

            if (mix <= 0.0f)
                continue;

            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                auto& out = channels[ch][startSample + i];
                const auto dry = dryScratch.getReadPointer (ch)[i];
                out = (mix >= 1.0f) ? dry : out + (dry - out) * mix;
            }
        }
    }

    void BalanceModule::pushIntoAnalysisRing (const juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse)
    {
        // 分析的是本模块的输入（前馈：修正量只由输入的偏差决定，不看自己的输出，没有反馈回路可自激）。
        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            const auto* in = io.getReadPointer (ch) + startSample;
            auto* ring = analysisRing.data() + (size_t) ch * (size_t) fftSize;
            auto write = ringWriteIndex;

            for (int i = 0; i < numSamples; ++i)
            {
                ring[write] = in[i];

                if (++write == fftSize)
                    write = 0;
            }
        }

        ringWriteIndex = (ringWriteIndex + numSamples) % fftSize;
        analysedChannels = numChannelsToUse;
    }

    //==============================================================================
    void BalanceModule::processSegment (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse) noexcept
    {
        // 没有一个滤波器在工作（三项修正都为 0 的稳态、静音门下、刚复位）：什么都不碰，输出逐位等于输入。
        if (numActiveBands == 0)
            return;

        auto* const* channels = io.getArrayOfWritePointers();
        int ch = 0;

       #if CELADON_BALANCE_PACKED_NEON || CELADON_BALANCE_PACKED_SSE2
        // 两个声道一组打包（立体声就是一组）；落单的声道（单声道）走标量。
        for (; packedFiltering && ch + 1 < numChannelsToUse; ch += 2)
            filterChannelPairPacked (channels[ch] + startSample, channels[ch + 1] + startSample, numSamples, ch,
                                     activeBands.data(), numActiveBands);
       #else
        juce::ignoreUnused (packedFiltering);
       #endif

        for (; ch < numChannelsToUse; ++ch)
            filterChannelScalar (channels[ch] + startSample, numSamples, ch, activeBands.data(), numActiveBands);
    }

    void BalanceModule::filterChannelScalar (float* samples, int numSamples, int channel,
                                             const int* bands, int numActive) noexcept
    {
        // 样本在外层、段在内层：一个样本依次过在工作的各段（段号从低到高，与 v0.4 的级联顺序相同），样本在 27 个
        // 滤波器之间保持 double。段 b 的状态只依赖它自己上一个样本，乱序执行会把前后样本的段链重叠起来——v0.4 是
        // 段在外层、逐样本受「乘 → 减 → 加」的延迟限制；每一步运算与 v0.4 相同，所以输出与状态逐位相同。
        // 系数与状态先拷成按工作顺序排的紧凑局部表，内层循环顺序读。
        BiquadCoefficients c[(size_t) numBands];
        BiquadState s[(size_t) numBands];

        for (int j = 0; j < numActive; ++j)
        {
            const auto b = (size_t) bands[j];
            c[j] = coefficients[b];
            s[j] = filterStates[b * (size_t) numChannels + (size_t) channel];
        }

        for (int i = 0; i < numSamples; ++i)
        {
            auto x = (double) samples[i];

            for (int j = 0; j < numActive; ++j)
                x = processBiquad (x, c[j], s[j]);

            samples[i] = (float) x;
        }

        for (int j = 0; j < numActive; ++j)
            filterStates[(size_t) bands[j] * (size_t) numChannels + (size_t) channel] = s[j];
    }

   #if CELADON_BALANCE_PACKED_NEON
    void BalanceModule::filterChannelPairPacked (float* first, float* second, int numSamples, int channel,
                                                 const int* bands, int numActive) noexcept
    {
        // 两个声道打包成 float64x2 一起算（2026-09-25 研究 P4 R3）：系数广播到两条通道，两个声道的状态各占一条。
        // 每一步与 processBiquad 一一对应——y = fma (b0, x, s1)；s1 = (b1·x − a1·y) + s2；s2 = b2·x − a2·y，
        // 其余各自舍入——通道之间没有任何交叉运算，所以每个声道的输出与状态都与标量路径逐位相同
        // （BalanceTests：两声道相同的立体声 = 同一信号的单声道，逐位）。样本在外层、段在内层，同 filterChannelScalar。
        // 滤波部分比 v0.4 快约 5 倍（2026-09-25 实测：梳状素材二十多段在工作，48 kHz 块 512，滤波 0.76% → 0.15% 单核）。
        // 不改成 float：float 级联与 double 的误差在 192 kHz 下到 −31 dB（研究 P4），对母带均衡不可接受。
        struct PackedBand
        {
            float64x2_t b0, b1, b2, a1, a2, s1, s2;
        };

        PackedBand packed[(size_t) numBands];

        for (int j = 0; j < numActive; ++j)
        {
            const auto b = (size_t) bands[j];
            const auto& c = coefficients[b];
            auto& p = packed[j];

            p.b0 = vdupq_n_f64 (c.b0);
            p.b1 = vdupq_n_f64 (c.b1);
            p.b2 = vdupq_n_f64 (c.b2);
            p.a1 = vdupq_n_f64 (c.a1);
            p.a2 = vdupq_n_f64 (c.a2);

            // 同一段相邻两个声道的状态在内存里是 {s1, s2, s1, s2}：vld2q_f64 拆成 {s1, s1} 与 {s2, s2}。
            const auto states = vld2q_f64 (&filterStates[b * (size_t) numChannels + (size_t) channel].s1);
            p.s1 = states.val[0];
            p.s2 = states.val[1];
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float32x2_t input { first[i], second[i] };
            auto x = vcvt_f64_f32 (input);   // float → double 是精确的

            for (int j = 0; j < numActive; ++j)
            {
                auto& p = packed[j];

                const auto y   = vfmaq_f64 (p.s1, p.b0, x);
                const auto bx1 = vmulq_f64 (p.b1, x);
                const auto ay1 = vmulq_f64 (p.a1, y);
                p.s1 = vaddq_f64 (vsubq_f64 (bx1, ay1), p.s2);

                const auto bx2 = vmulq_f64 (p.b2, x);
                const auto ay2 = vmulq_f64 (p.a2, y);
                p.s2 = vsubq_f64 (bx2, ay2);

                x = y;
            }

            // double → float 按当前舍入模式（就近取偶），与标量路径的 (float) x 相同。
            const auto output = vcvt_f32_f64 (x);
            first[i]  = vget_lane_f32 (output, 0);
            second[i] = vget_lane_f32 (output, 1);
        }

        for (int j = 0; j < numActive; ++j)
        {
            const auto b = (size_t) bands[j];
            const float64x2x2_t states { { packed[j].s1, packed[j].s2 } };
            vst2q_f64 (&filterStates[b * (size_t) numChannels + (size_t) channel].s1, states);
        }
    }
   #elif CELADON_BALANCE_PACKED_SSE2
    void BalanceModule::filterChannelPairPacked (float* first, float* second, int numSamples, int channel,
                                                 const int* bands, int numActive) noexcept
    {
        // x64 版（Intel Mac、Windows）：两个声道打包成 SSE2 的 __m128d（低位 = first，高位 = second），结构与 AArch64 版
        // 相同——样本在外层、段在内层，系数广播到两条通道。每一步与 x64 上的 processBiquad 一一对应：y = b0·x + s1
        // （乘、加各自舍入）；s1 = (b1·x − a1·y) + s2；s2 = b2·x − a2·y。打包的 mulpd / addpd / subpd 与标量的
        // mulsd / addsd / subsd 逐通道同一 IEEE 运算、同一 MXCSR（舍入、FTZ / DAZ），通道之间没有交叉运算，所以每个声道的
        // 输出与状态都与标量路径逐位相同（BalanceTests「stereo with identical channels ... packed vs scalar」与
        // 「stereo with different channels ... each channel matches its own mono run」）。
        struct PackedBand
        {
            __m128d b0, b1, b2, a1, a2, s1, s2;
        };

        PackedBand packed[(size_t) numBands];

        for (int j = 0; j < numActive; ++j)
        {
            const auto b = (size_t) bands[j];
            const auto& c = coefficients[b];
            auto& p = packed[j];

            p.b0 = _mm_set1_pd (c.b0);
            p.b1 = _mm_set1_pd (c.b1);
            p.b2 = _mm_set1_pd (c.b2);
            p.a1 = _mm_set1_pd (c.a1);
            p.a2 = _mm_set1_pd (c.a2);

            // 同一段相邻两个声道的状态在内存里是 {s1, s2}{s1, s2}：各读 16 字节，按低 / 高位重新配成 {s1, s1} 与 {s2, s2}。
            const auto* states = &filterStates[b * (size_t) numChannels + (size_t) channel];
            const auto firstState  = _mm_loadu_pd (&states[0].s1);
            const auto secondState = _mm_loadu_pd (&states[1].s1);
            p.s1 = _mm_unpacklo_pd (firstState, secondState);
            p.s2 = _mm_unpackhi_pd (firstState, secondState);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // float → double 是精确的（cvtps2pd 与标量的 cvtss2sd 相同）。
            auto x = _mm_cvtps_pd (_mm_setr_ps (first[i], second[i], 0.0f, 0.0f));

            for (int j = 0; j < numActive; ++j)
            {
                auto& p = packed[j];

                const auto y   = _mm_add_pd (_mm_mul_pd (p.b0, x), p.s1);
                const auto bx1 = _mm_mul_pd (p.b1, x);
                const auto ay1 = _mm_mul_pd (p.a1, y);
                p.s1 = _mm_add_pd (_mm_sub_pd (bx1, ay1), p.s2);

                const auto bx2 = _mm_mul_pd (p.b2, x);
                const auto ay2 = _mm_mul_pd (p.a2, y);
                p.s2 = _mm_sub_pd (bx2, ay2);

                x = y;
            }

            // double → float 按 MXCSR 的舍入模式（就近取偶），cvtpd2ps 与标量 (float) x 的 cvtsd2ss 相同。
            const auto output = _mm_cvtpd_ps (x);
            first[i]  = _mm_cvtss_f32 (output);
            second[i] = _mm_cvtss_f32 (_mm_shuffle_ps (output, output, _MM_SHUFFLE (1, 1, 1, 1)));
        }

        for (int j = 0; j < numActive; ++j)
        {
            auto* states = &filterStates[(size_t) bands[j] * (size_t) numChannels + (size_t) channel];
            _mm_storeu_pd (&states[0].s1, _mm_unpacklo_pd (packed[j].s1, packed[j].s2));
            _mm_storeu_pd (&states[1].s1, _mm_unpackhi_pd (packed[j].s1, packed[j].s2));
        }
    }
   #endif

    //==============================================================================
    void BalanceModule::designPeak (int band, double gainDb)
    {
        // RBJ Audio EQ Cookbook 峰值滤波器（b1 与 a1 本来就相同）。gainDb = 0 → A = 1：alpha·A 与 alpha/A 都精确
        // 等于 alpha，b0 = a0 / a0 = 1.0、b2 与 a2 是同一个表达式 —— 逐位直通（推导同 ColorModule::designShelf）。
        const auto b = (size_t) band;
        const auto a = gainDb == 0.0 ? 1.0 : std::pow (10.0, gainDb / 40.0);
        const auto alphaTimesA = alpha[b] * a;
        const auto alphaOverA  = alpha[b] / a;
        const auto a0          = 1.0 + alphaOverA;

        auto& c = coefficients[b];
        c.b0 = (1.0 + alphaTimesA) / a0;
        c.b1 = (-2.0 * cosW0[b]) / a0;
        c.b2 = (1.0 - alphaTimesA) / a0;
        c.a1 = c.b1;
        c.a2 = (1.0 - alphaOverA) / a0;
    }

    void BalanceModule::updateCoefficients()
    {
        // 增益（dB）在两次 hop 之间按样本线性插值，每 64 样本取一次插值结果重算系数（设计规格-Balance「执行」）。
        // 分析前 samplesSinceHop 可能超过一个 hop，但那时起点与目标都还是 0，插出来仍是精确的 0。
        const auto fraction = (double) samplesSinceHop / (double) hopSize;
        numActiveBands = 0;

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            const auto gainDb = hopStartGainDb[b] + (targetGainDb[b] - hopStartGainDb[b]) * fraction;

            if (gainDb != gainForCoefficients[b])
            {
                designPeak ((int) b, gainDb);
                gainForCoefficients[b] = gainDb;
            }

            // 状态归零在这里做（从 reset 起算的固定网格上），而不是在块尾：归零时刻与块大小无关。
            // 增益精确为 0 时门槛放宽到 1e-10（见 zeroGainStateSnapThreshold 的说明）。
            const auto threshold = gainDb == 0.0 ? zeroGainStateSnapThreshold : stateSnapThreshold;
            bool statesAreZero = true;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& s = filterStates[b * (size_t) numChannels + (size_t) ch];
                snapToZero (s.s1, threshold);
                snapToZero (s.s2, threshold);
                statesAreZero = statesAreZero && s.s1 == 0.0 && s.s2 == 0.0;
            }

            // 增益回到 0 后系数是直通，状态归零之后再跑它就是纯粹的直通乘法，直接跳过：既省 CPU，
            // 也保证 Recover = Tame = Brighten = 0 时逐位等于输入。在工作的段按段号从低到高登记，级联顺序不变。
            if (gainDb != 0.0 || ! statesAreZero)
                activeBands[(size_t) numActiveBands++] = (int) b;
        }
    }

    //==============================================================================
    // 分析拆片（2026-09-25 研究 P4 R5）：v0.4 在 hop 边界那一个样本上一口气做完两声道加窗、两次 FFT、27 段求和与
    // 跟踪判断，192 kHz（8192 点）时约 50 µs 全落在同一块里——小块下是插件最坏块的主因。新目标最早在 hop 后第
    // 64 个样本那次系数重算才用到，所以分析拆成三片，落在 hop 后第 0 / 32 / 64 个样本上，最后一片紧挨着第 64 个
    // 样本那次重算之前做完：
    //   片 0（hop 边界上）：提交插值起点（边界上那次重算的插值比例是 0，只用到它）；快照本帧用到的参数、声道数、
    //       旁通比例（后面几片只用快照：中途宿主改参数、切旁通都不影响本帧）；趁环形缓冲还没被新样本覆盖，把各声道
    //       加窗后的整帧拷出来；前一半声道（立体声 = 左声道）做 FFT。
    //   片 1（+32）：前一半声道的功率谱累加。
    //   片 2（+64）：后一半声道（立体声 = 右声道）的 FFT 与功率谱累加、27 段求和、跟踪、判断、发布。
    //       v0.1.7.0 起 27 段求和之后多一步左右交叉功率（measureBandCross：要两个声道都变换完，只能在这一片），
    //       只写 bandCross、只进快照；约 800 次复数乘加（段只覆盖约 35 Hz ～ 18 kHz，各采样率都一样），估计约 1 µs——
    //       192 kHz（最坏块的来源）下比片 2 那次约 22 µs 的 FFT 小一个数量级以上，不改变三片的负担分布。
    // 两次 FFT 是分析里的大头（192 kHz 每次约 22 µs，求和与跟踪合计约 2.5 µs），这样分：块 32 时三块各一片；
    // 块 64 时两次 FFT 分在相邻两块（研究原稿「0 / 32 / 64 各一次 FFT」在块 64 时两次都落进同一块）。
    // 各片的运算与 v0.4 完全相同，只是时间摊开：功率谱仍是「清零 → 声道 0 累加 → 声道 1 累加」、同样的表达式。
    // 拆片关掉（只给测试用）时三片在 hop 边界上接着做完，就是 v0.4 的做法——两者 G 与输出逐位相同。
    void BalanceModule::beginAnalysis()
    {
        // 上一 hop 的目标此刻已到达（插值终点），作为新的起点 —— 取的是精确值，不带累加误差。
        for (size_t b = 0; b < (size_t) numBands; ++b)
            hopStartGainDb[b] = targetGainDb[b];

        frameParams        = params;
        frameAppliedAmount = 1.0f - bypassMixSmoothed.getCurrentValue();
        frameChannels      = juce::jmax (1, analysedChannels);
        frameEndSample     = processedSamples;   // 环形缓冲里此刻正是本帧的整窗：最后一个样本之后的时间戳

        for (int ch = 0; ch < frameChannels; ++ch)
        {
            const auto* ring = analysisRing.data() + (size_t) ch * (size_t) fftSize;
            auto* buffer = frameBuffer (ch);
            int n = 0;

            for (int i = ringWriteIndex; i < fftSize; ++i, ++n)
                buffer[n] = ring[i] * window[(size_t) n];

            for (int i = 0; i < ringWriteIndex; ++i, ++n)
                buffer[n] = ring[i] * window[(size_t) n];
        }

        std::fill (powerSpectrum.begin(), powerSpectrum.end(), 0.0);

        for (int ch = 0; ch < firstChannelGroupSize(); ++ch)
            transformFrame (ch);

        nextAnalysisSlice     = 1;
        analysisSliceSpacing  = sliceAnalysis ? coefficientUpdateInterval / 2 : 0;
        samplesUntilNextSlice = analysisSliceSpacing;
    }

    void BalanceModule::runNextAnalysisSlice()
    {
        const auto firstGroup = firstChannelGroupSize();

        if (nextAnalysisSlice == 1)
        {
            // 片 1：前一半声道的功率谱累加。
            for (int ch = 0; ch < firstGroup; ++ch)
                accumulatePower (ch);

            nextAnalysisSlice     = 2;
            samplesUntilNextSlice = analysisSliceSpacing;
            return;
        }

        // 片 2：后一半声道的 FFT 与功率谱累加（声道顺序不变），再段能量（连同左右交叉功率）→ 跟踪 → 判断 → 发布。
        for (int ch = firstGroup; ch < frameChannels; ++ch)
        {
            transformFrame (ch);
            accumulatePower (ch);
        }

        measureBandPowers();
        measureBandCross();       // v0.1.7.0：只写 bandCross；下面的跟踪 / 判断（含读各声道 FFT 的谐波保护）都不读它
        publishFrameSnapshot();
        updateTrackers();
        updateCorrectionTargets();
        publishGains();

        nextAnalysisSlice = -1;
    }

    int BalanceModule::firstChannelGroupSize() const noexcept
    {
        // 前一半声道（向上取整）：立体声 1、单声道 1。
        return (frameChannels + 1) / 2;
    }

    float* BalanceModule::frameBuffer (int channel) noexcept
    {
        return frameBuffers.data() + (size_t) channel * 2 * (size_t) fftSize;
    }

    const float* BalanceModule::frameBuffer (int channel) const noexcept
    {
        return frameBuffers.data() + (size_t) channel * 2 * (size_t) fftSize;
    }

    void BalanceModule::transformFrame (int channel)
    {
        // 每声道各做一次实数 FFT，功率谱相加后取平均（不是先把两声道相加再变换：反相内容会互相抵消）。
        // FFT 对象在 prepare 时建好；vDSP 引擎变换时不分配，JUCE 回退引擎对 ≤ 8192 点用 alloca（栈上）。
        // 输入只看缓冲的前一半（fftSize 点），后一半是输出空间：各声道各用一段缓冲，与 v0.4 共用一段结果相同。
        fft->performRealOnlyForwardTransform (frameBuffer (channel), true);
    }

    void BalanceModule::accumulatePower (int channel) noexcept
    {
        const auto half = fftSize / 2;
        const auto* buffer = frameBuffer (channel);

        for (int k = 0; k <= half; ++k)
        {
            const auto re = (double) buffer[2 * k];
            const auto im = (double) buffer[2 * k + 1];
            powerSpectrum[(size_t) k] += re * re + im * im;
        }
    }

    void BalanceModule::measureBandPowers() noexcept
    {
        // 各声道功率谱之和 → 27 段能量（按声道数取平均）。
        const auto channelScale = 1.0 / (double) juce::jmax (1, frameChannels);

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            double sum = 0.0;
            const auto* spectrum = powerSpectrum.data() + bandFirstBin[b];
            const auto* weights  = binWeights.data() + bandWeightOffset[b];

            for (int k = 0; k < bandNumBins[b]; ++k)
                sum += spectrum[k] * weights[k];

            auto power = sum * channelScale;

            // 输入里混进 NaN / Inf 时不让它污染跟踪状态（处理器随后会把整条链重置）。
            if (! std::isfinite (power))
                power = 0.0;

            bandPower[b] = power;
        }
    }

    void BalanceModule::measureBandCross() noexcept
    {
        // v0.1.7.0（修坯 Clay，设计规格-Clay-分析决策与验收 1.1）：27 段左右交叉功率 X[b]，与 bandPower 同一帧、同一段边界、
        // 同一套 bin 权重（binWeights 里已含 spectrumScale），随后一起进本帧快照；Clay 的读者由它推 P_M = (P + X) / 2、
        // P_S = (P − X) / 2、ρ = X / P。整个函数只写 bandCross：交叉功率由 const 的 crossPowerOfBand 从两声道的 FFT 结果读出，
        // frameBuffers（随后谐波保护还要读）与所有跟踪 / 判断状态都碰不到——修正量 G 与音频输出逐位不变。
        //
        // 本帧分析声道数为 1（单声道；或按立体声 prepare、宿主这一块只送来一个声道——那时声道 1 的帧缓冲是旧数据，绝不能用）：
        // 没有 S，X = bandPower（ρ = 1、P_S = 0），直接拷贝。
        if (frameChannels < 2)
        {
            bandCross = bandPower;
            return;
        }

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            auto cross = crossPowerOfBand (b);

            // 同 bandPower：输入混进 NaN / Inf 时归零（那时 bandPower 也已归零）。
            if (! std::isfinite (cross))
                cross = 0.0;

            // 精确运算下 |X| ≤ bandPower：逐 bin 有 2·Re(L·R*) ≤ 2·|L|·|R| ≤ |L|² + |R|²，权重非负，段和照样成立。
            // 越界只可能来自两边段内求和的舍入（左右相同 / 相反时差 1e-16 量级），钳住它，读者算出的 P_M、P_S 永不为负、
            // ρ 永在 [−1, 1]。bandPower ≥ 0（功率谱与权重都非负，非有限已归零），所以上下限不会颠倒。
            bandCross[b] = juce::jlimit (-bandPower[b], bandPower[b], cross);
        }
    }

    double BalanceModule::crossPowerOfBand (size_t band) const noexcept
    {
        // performRealOnlyForwardTransform 的交错复数布局：buffer[2k] = Re X_k、buffer[2k + 1] = Im X_k（k = 0 ～ fftSize / 2）。
        // Re(X_L · conj(X_R)) = reL·reR + imL·imR。两个 float 的乘积在 double 里是精确的（24 + 24 位有效数字 < 53），所以每个 bin
        // 只在相加时舍入一次、有没有乘加融合都一样——与 accumulatePower 的 re·re + im·im 是同一种运算，左右逐位相同时两者逐位相同。
        // 段内按与 measureBandPowers 相同的顺序与权重求和。bandPower 最后乘 1 / 声道数（两声道功率的平均）；交叉功率本身就是
        // 「每声道」的口径，不再除——左右相同时两者相等（设计规格：bandPower = (P_L + P_R) / 2）。
        //
        // 声道 0 / 1 = 立体声的左 / 右。处理器只给 1 / 2 声道；万一多于 2 声道也只取前两个（此时 bandPower 是所有声道的平均，
        // 口径不再严格对应，只保证不越界、只读本帧已经变换过的缓冲——片 2 进来时本帧所有声道都已变换完）。
        const auto* left    = frameBuffer (0);
        const auto* right   = frameBuffer (1);
        const auto* weights = binWeights.data() + bandWeightOffset[band];
        const auto first    = bandFirstBin[band];
        double sum = 0.0;

        for (int k = 0; k < bandNumBins[band]; ++k)
        {
            const auto bin = 2 * (first + k);
            const auto reL = (double) left[bin],  imL = (double) left[bin + 1];
            const auto reR = (double) right[bin], imR = (double) right[bin + 1];
            const auto cross = reL * reR + imL * imR;
            sum += cross * weights[k];
        }

        return sum;
    }

    void BalanceModule::updateTrackers()
    {
        const auto speed = (size_t) frameParams.speed;
        const auto& preCoeff = preAverageCoeff[speed];

        // 1) 每段：功率域 Welch 式时间平均 → L[b]（dB）→ 静音门 → 快跟踪 Lfast[b]。
        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            if (bandAlive[b])
            {
                // 运行均值：前 n 帧取精确的算术平均（系数 n / (n + 1)），平均帧数够了之后转成一阶指数平均。
                // 刚复活的段几帧之内就有可用的估计，不必等一个完整时间常数。
                const auto n = (double) hopsAlive[b];
                const auto retain = juce::jmin (preCoeff[b], n / (n + 1.0));
                averagedPower[b] = bandPower[b] + (averagedPower[b] - bandPower[b]) * retain;

                if (hopsAlive[b] < (1 << 30))
                    ++hopsAlive[b];
            }
            else
            {
                // 门下（或刚复位）的段不积累历史：直接镜像本帧，回到门上那一刻从这一帧重新开始平均。
                averagedPower[b] = bandPower[b];
                hopsAlive[b] = 1;
            }

            levelDb[b] = 10.0 * std::log10 (juce::jmax (powerFloor, averagedPower[b]));

            const auto aliveNow = levelDb[b] > gateDb;
            gateWeight[b] = juce::jlimit (0.0, 1.0, (levelDb[b] - gateDb) / gateFadeDb);

            if (bandAlive[b])
            {
                const auto coeff = levelDb[b] > fastLevelDb[b] ? attackCoeff[speed] : releaseCoeff[speed];
                fastLevelDb[b] = levelDb[b] + (fastLevelDb[b] - levelDb[b]) * coeff;
                bandRevived[b] = false;

                if (! aliveNow)
                    bandAlive[b] = false;   // 掉到门下：下一 hop 起镜像，复活时再从头来
            }
            else
            {
                // 复活（或第一帧）：快跟踪直接落到当前电平，参考稍后（第 4 步）也落到本段自己的电平（偏差从 0 起步）
                // —— 静音之后音乐进来，参考线不会从 −300 dB 慢慢爬上来把整段音乐先压 6 dB；
                // 真正的偏差随参考线向邻段轮廓靠拢逐渐显现。
                fastLevelDb[b]      = levelDb[b];
                bandRevived[b]      = true;
                hopsSinceRevival[b] = 0;
                bandAlive[b]        = aliveNow;
            }
        }

        // 2) 参考线 R = 形状 S（慢平滑）+ 全段共同的电平 C（每 hop 直接重估、瞬时跟随）。C 是活着的段
        //    (quickLevel − S) 的稳健位置估计（robustLocation，静音门权重加权）：整体抬 10 dB 时所有段的
        //    (quickLevel − S) 在 60 ms 内一起 +10，C 立刻跟上，参考线不用等 2 s ——不然主歌进副歌时所有段先被
        //    Tame 压 6 dB 再慢慢放开，那是宽带压缩，不是频谱平衡（实测静音后进音乐：全段 −6 dB 持续 2 s）。
        //    输入用所有段同速的快电平而不是各段自己的 Lfast，见 commonLevelSeconds 的说明。
        //    稳健而不是普通平均：只有少数段在动（贝斯进来、扫频经过）时 C 几乎不受牵连；
        //    刚复活的段本 hop 不参与（它的 S 还没定），它的快电平直接镜像本帧，不带门下的历史。
        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            if (bandAlive[b] && ! bandRevived[b])
                quickPower[b] = bandPower[b] + (quickPower[b] - bandPower[b]) * quickCoeff;
            else
                quickPower[b] = bandPower[b];

            quickLevelDb[b] = 10.0 * std::log10 (juce::jmax (powerFloor, quickPower[b]));
        }

        int numValues = 0;

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            if (bandAlive[b] && ! bandRevived[b])
            {
                locationValues[(size_t) numValues]  = quickLevelDb[b] - referenceShapeDb[b];
                locationWeights[(size_t) numValues] = gateWeight[b];
                ++numValues;
            }
        }

        commonLevelDb = robustLocation (locationValues.data(), locationWeights.data(), numValues, commonLevelDb);

        // 3) 参考轮廓：Lfast 在频率方向抹平。权重 = 核 × 静音门权重，越界或门下的段等于没有；
        //    全没有（孤零零的一段）就取自己 —— 偏差 0。门下的段（Lfast 可能低到 −300 dB）绝不能进来：
        //    否则 16 kHz 以上被削掉的音源会把 10 ～ 12.5 kHz 的参考线拖低几十 dB，Tame 把它们压死。
        for (int band = 0; band < numBands; ++band)
        {
            double numerator = 0.0, denominator = 0.0;

            for (int j = -kernelRadius; j <= kernelRadius; ++j)
            {
                const auto neighbour = band + j;

                if (neighbour < 0 || neighbour >= numBands)
                    continue;

                const auto weight = kernel[(size_t) (j + kernelRadius)] * gateWeight[(size_t) neighbour];
                numerator   += weight * fastLevelDb[(size_t) neighbour];
                denominator += weight;
            }

            smoothedLevelDb[(size_t) band] = denominator > 0.0 ? numerator / denominator
                                                               : fastLevelDb[(size_t) band];
        }

        // 4) 形状 S 再在时间上慢平滑（Slow 4 s / Mid 2 s / Fast 1 s）。复活后的前几 hop 参考锁在本段自己的电平
        //    （偏差 0）：静音后的第一帧只有窗尾的一小截信号，单帧估计误差 ±5 dB 以上，拿它当参考会让某些段先
        //    提 / 压 6 dB 再花 2 s 回来（实测）。锁过一整窗之后，S 按运行均值起步（前 n hop 取算术平均、再转
        //    指数平滑）：几百毫秒内就有可用的轮廓，不必等一个完整时间常数。门下的段跟着自己的电平走。
        //
        //    S 的目标 = 抹平后的 Lfast − 「本段看到的 C」（C 经与本段测量相同的动态滞后，laggedCommonDb）：
        //    整体电平一变，C 在 60 ms 内跳到位，而抹平后的 Lfast 按各段测量速度慢慢爬；若直接减 C，S 的目标会
        //    整体先凹下去再回来，慢平滑把这段凹陷积进 S，参考 R 随后偏低约 1 ～ 2 dB、持续约 1 s（实测 ±10 dB
        //    阶跃，100 / 100）。减滞后的 C 则两边同速爬，S 不受电平变化扰动；稳态下滞后的 C 就是 C，不动点不变。
        //    滞后的 C 不依赖 S，S 的更新仍是一阶（曾试「R − S」作滞后的 C：阶跃同样好，但 S 变成二阶回路，
        //    低频段收敛拖到十几秒，放弃；2026-09-25 评审 R1-01 修复时对照实测）。
        //
        // 5) 目标参考 S + C 再经过与本段测量完全相同的动态（功率域时间平均 + dB 域攻击 / 释放）才成为 R[b]：
        //    整体抬 10 dB 时 C 立刻跟上，但 40 Hz 段的测量要 1.4 s 才爬到位（自由度所限），16 kHz 段十几毫秒
        //    就到 —— 参考若不同步滞后，低段会先被 Recover 提 2.5 dB、高段先被 Tame 压 3 dB 再花一秒回来（实测）。
        //    稳态下 R = S + C，与不加这一步完全相同。
        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            if (! bandAlive[b] || hopsSinceRevival[b] < referenceLockHops)
            {
                referenceShapeDb[b]  = fastLevelDb[b] - commonLevelDb;
                referencePower[b]    = averagedPower[b];
                referenceDb[b]       = fastLevelDb[b];
                laggedCommonPower[b] = std::pow (10.0, commonLevelDb * 0.1);
                laggedCommonDb[b]    = commonLevelDb;
            }
            else
            {
                // 与第 1 步同一套系数：功率域运行均值（同样的分母）→ dB → 同样的攻击 / 释放。
                // 滞后的 C 与参考 R 都走这一套。
                const auto measured    = (double) hopsAlive[b];
                const auto powerRetain = juce::jmin (preCoeff[b], (measured - 1.0) / measured);

                const auto commonPower = std::pow (10.0, commonLevelDb * 0.1);
                laggedCommonPower[b] = commonPower + (laggedCommonPower[b] - commonPower) * powerRetain;

                const auto commonSeen = 10.0 * std::log10 (juce::jmax (powerFloor, laggedCommonPower[b]));
                const auto commonCoeff = commonSeen > laggedCommonDb[b] ? attackCoeff[speed] : releaseCoeff[speed];
                laggedCommonDb[b] = commonSeen + (laggedCommonDb[b] - commonSeen) * commonCoeff;

                const auto n = (double) (hopsSinceRevival[b] - referenceLockHops) + 1.0;   // 锁定期末的值算第一个样本
                const auto retain = juce::jmin (referenceCoeff[speed], n / (n + 1.0));
                const auto shape = smoothedLevelDb[b] - laggedCommonDb[b];
                referenceShapeDb[b] = shape + (referenceShapeDb[b] - shape) * retain;

                const auto targetPower = std::pow (10.0, (referenceShapeDb[b] + commonLevelDb) * 0.1);
                referencePower[b] = targetPower + (referencePower[b] - targetPower) * powerRetain;

                const auto referenceLevel = 10.0 * std::log10 (juce::jmax (powerFloor, referencePower[b]));
                const auto coeff = referenceLevel > referenceDb[b] ? attackCoeff[speed] : releaseCoeff[speed];
                referenceDb[b] = referenceLevel + (referenceDb[b] - referenceLevel) * coeff;
            }

            if (bandAlive[b] && hopsSinceRevival[b] < (1 << 30))
                ++hopsSinceRevival[b];
        }

        // 6) 重新定中心：把形状 S 的（静音门加权）均值挪进 C（各段滞后的 C 同步平移）。S + C 逐段不变，
        //    参考与输出不受影响；下一 hop 的 (quickLevel − S) 与 C 同步平移，Huber 估计结果也同步平移，闭环一致。
        //    不做这一步时，峰状频谱（少数段远高于轮廓）下「C 的输入」与「S 追的目标」之差不为零，
        //    C 与 S 会反向无界漂移（实测 −0.74 dB/s，90 s 漂到 −70 dB；2026-09-25 评审 R1-03）——
        //    输出暂时没错，但 C 不再是「电平」，以后任何读它的功能都会拿到垃圾。
        {
            double numerator = 0.0, denominator = 0.0;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                numerator   += gateWeight[b] * referenceShapeDb[b];
                denominator += gateWeight[b];
            }

            if (denominator > 0.0)
            {
                const auto mean = numerator / denominator;
                const auto powerScale = std::pow (10.0, mean * 0.1);

                for (size_t b = 0; b < (size_t) numBands; ++b)
                {
                    referenceShapeDb[b]  -= mean;
                    laggedCommonDb[b]    += mean;
                    laggedCommonPower[b] *= powerScale;
                }

                commonLevelDb += mean;
            }
        }
    }

    void BalanceModule::updateHarmonicProtection() noexcept
    {
        // 复用刚算好的功率谱，只扫描现有段覆盖的 bin。固定数组，无新 FFT、延迟或音频线程分配。
        PerBand<double> peakBin {}, peakPower {}, supported {};
        const auto lastBin = fftSize / 2 - 3;
        const auto binHz = sampleRate / (double) fftSize;
        const auto scale = spectrumScale / (double) juce::jmax (1, frameChannels);

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            if (! bandAlive[b] || bandPower[b] <= 0.0)
                continue;
            const auto first = juce::jmax (3, bandFirstBin[b]);
            const auto last = juce::jmin (lastBin, bandFirstBin[b] + bandNumBins[b] - 1);
            int peak = first;
            double power = 0.0;
            for (int k = first; k <= last; ++k)
                if (powerSpectrum[(size_t) k] > power)
                {
                    power = powerSpectrum[(size_t) k];
                    peak = k;
                }

            if (! std::isfinite (power) || ! (power * scale >= 0.15 * bandPower[b]))
                continue;
            const auto left = powerSpectrum[(size_t) peak - 1];
            const auto right = powerSpectrum[(size_t) peak + 1];
            const auto background = 0.25 * (powerSpectrum[(size_t) peak - 3] + powerSpectrum[(size_t) peak - 2]
                                          + powerSpectrum[(size_t) peak + 2] + powerSpectrum[(size_t) peak + 3]);
            if (! (power > left && power > right && power > harmonicPeakContrast * background))
                continue;

            // Hann 主瓣的幅度抛物线插值，避免非整数 bin 的正常音高被误判为不谐和。
            const auto l = std::sqrt (left), centre = std::sqrt (power), r = std::sqrt (right);
            const auto curvature = l - 2.0 * centre + r;
            const auto offset = curvature < 0.0 ? juce::jlimit (-0.5, 0.5, 0.5 * (l - r) / curvature) : 0.0;
            peakBin[b] = (double) peak + offset;
            peakPower[b] = power;
        }

        struct Line { double bin = 0.0, power = 0.0; };
        const auto channelLine = [&] (int channel, size_t band) noexcept
        {
            // 候选关系先由组合功率谱筛选，再从已经完成的各声道 FFT 核验；不让左基音和右泛音拼成保护证据。
            const auto* buffer = frameBuffer (channel);
            const auto powerAt = [buffer] (int bin) noexcept
            {
                const auto re = (double) buffer[2 * bin], im = (double) buffer[2 * bin + 1];
                return re * re + im * im;
            };
            const auto candidate = (int) (peakBin[band] + 0.5);
            const auto first = juce::jmax (3, candidate - 1), last = juce::jmin (lastBin, candidate + 1);
            int peak = first;
            double power = 0.0;
            for (int k = first; k <= last; ++k)
                if (const auto p = powerAt (k); p > power)
                {
                    peak = k;
                    power = p;
                }
            const auto left = powerAt (peak - 1), right = powerAt (peak + 1);
            const auto background = 0.25 * (powerAt (peak - 3) + powerAt (peak - 2)
                                          + powerAt (peak + 2) + powerAt (peak + 3));
            if (! std::isfinite (power) || ! (power > left && power > right && power > harmonicPeakContrast * background))
                return Line {};
            const auto l = std::sqrt (left), centre = std::sqrt (power), r = std::sqrt (right);
            const auto curvature = l - 2.0 * centre + r;
            const auto offset = curvature < 0.0 ? juce::jlimit (-0.5, 0.5, 0.5 * (l - r) / curvature) : 0.0;
            return Line { (double) peak + offset, power };
        };

        for (size_t root = 0; root < (size_t) numBands; ++root)
        {
            if (peakBin[root] < harmonicMinimumRootBin || peakBin[root] * binHz > harmonicMaximumRootHz)
                continue;
            const auto match = [&] (int harmonic) noexcept
            {
                const auto expected = peakBin[root] * harmonic;
                const auto tolerance = juce::jmax (0.65, 0.006 * expected);
                auto closest = tolerance;
                int found = -1;
                for (size_t b = root + 1; b < (size_t) numBands; ++b)
                {
                    if (peakPower[b] < peakPower[root] * harmonicMinimumRelativePower
                        || peakPower[b] > peakPower[root] * harmonicMaximumRelativePower)
                        continue;
                    const auto distance = std::abs (peakBin[b] - expected);
                    if (distance <= closest)
                    {
                        closest = distance;
                        found = (int) b;
                    }
                }
                return found;
            };
            const auto second = match (2), third = match (3);
            if (second >= 0 && third >= 0 && second != third)
            {
                auto sameChannel = frameChannels == 1;
                for (int ch = 0; ch < frameChannels && ! sameChannel; ++ch)
                {
                    const auto fundamental = channelLine (ch, root);
                    if (! (fundamental.power > 0.0))
                        continue;
                    const auto supports = [&] (size_t band, int harmonic) noexcept
                    {
                        const auto line = channelLine (ch, band);
                        const auto expected = fundamental.bin * harmonic;
                        return line.power >= fundamental.power * harmonicMinimumRelativePower
                            && line.power <= fundamental.power * harmonicMaximumRelativePower
                            && std::abs (line.bin - expected) <= juce::jmax (0.65, 0.006 * expected);
                    };
                    sameChannel = supports ((size_t) second, 2) && supports ((size_t) third, 3);
                }
                if (sameChannel)
                    supported[root] = supported[(size_t) second] = supported[(size_t) third] = 1.0;
            }
        }

        for (size_t b = 0; b < (size_t) numBands; ++b)
            harmonicProtection[b] += juce::jlimit (-hopSeconds / harmonicProtectionReleaseSeconds,
                                                   hopSeconds / harmonicProtectionAttackSeconds,
                                                   supported[b] - harmonicProtection[b]);
    }

    void BalanceModule::updateCorrectionTargets()
    {
        // 插值起点（hopStartGainDb）已在本帧片 0 提交（beginAnalysis）；参数用片 0 时的快照。
        const auto tame     = (double) frameParams.tame / 100.0;
        const auto recover  = (double) frameParams.recover / 100.0;
        const auto automaticRecover = (double) frameParams.automaticRecover / 100.0;
        const auto automaticTame = (double) frameParams.automaticTame / 100.0;
        const auto brighten = (double) frameParams.brighten / 100.0;
        const auto manualRecover = juce::jmax (0.0, recover - automaticRecover);
        PerBand<double> proposed {}, untilted {}, recoverPermit {};

        if (automaticTame > 0.0 || frameParams.harshAmount > 0.0f)
            updateHarmonicProtection();
        else
            harmonicProtection.fill (0.0);

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            double target = 0.0;
            double automaticWeight = 0.0;
            double automaticTameWeight = 0.0;

            if (automaticTame > 0.0)
            {
                const auto rising = bandPower[b] > automaticTameOnsetPowerRatio * juce::jmax (powerFloor, previousBandPower[b]);
                automaticOnsetHoldSeconds[b] = rising ? automaticTameOnsetHoldSeconds
                    : juce::jmax (0.0, automaticOnsetHoldSeconds[b] - hopSeconds);

                // 不用紧邻段作唯一支撑：单音的 Hann 主瓣/泄漏可能占住邻段。两侧各取距离 2、3 的段，
                // 都须有可信内容；端点或孤立持续单音没有这样的宽带参照，不擅自把它当作共振削掉。
                int leftSupport = 0, rightSupport = 0;
                for (int distance = 2; distance <= kernelRadius; ++distance)
                {
                    const auto supported = [&] (size_t neighbour)
                    {
                        return gateWeight[neighbour] >= 0.5
                            && quickPower[neighbour] >= bandPower[b] * automaticTameSupportRatio;
                    };
                    if ((int) b >= distance && supported (b - (size_t) distance)) ++leftSupport;
                    if (b + (size_t) distance < (size_t) numBands && supported (b + (size_t) distance)) ++rightSupport;
                }

                const auto persistentPeak = bandAlive[b] && leftSupport == 2 && rightSupport == 2
                    && fastLevelDb[b] - smoothedLevelDb[b] > deadZoneDb
                    && bandPower[b] >= averagedPower[b] * automaticTamePresentRatio
                    && automaticOnsetHoldSeconds[b] <= 0.0;
                automaticPeakSeconds[b] = persistentPeak
                    ? juce::jmin (automaticTameFullSeconds, automaticPeakSeconds[b] + hopSeconds) : 0.0;
                automaticTameWeight = juce::jlimit (0.0, 1.0,
                    (automaticPeakSeconds[b] - automaticTameStartSeconds)
                        / (automaticTameFullSeconds - automaticTameStartSeconds));
                automaticTameWeight *= 1.0 - harmonicProtectionAmount * harmonicProtection[b];
            }
            else
            {
                automaticPeakSeconds[b] = 0.0;
                automaticOnsetHoldSeconds[b] = 0.0;
            }
            previousBandPower[b] = bandPower[b];

            if (automaticRecover > 0.0 && bandAlive[b] && b > 0 && b + 1 < (size_t) numBands)
            {
                // 两边各 ±3 段里必须有高于本段的有效内容。单调谱/自然滚降只有一边更响，
                // 两端则没有外侧证据，都不给自动提升。原本的手动 Recover 与 Brighten 不受此限制。
                double leftSupport = 0.0, rightSupport = 0.0;

                for (int distance = 1; distance <= kernelRadius; ++distance)
                {
                    if ((int) b - distance >= 0)
                    {
                        const auto neighbour = b - (size_t) distance;
                        if (bandAlive[neighbour])
                            leftSupport = juce::jmax (leftSupport, levelDb[neighbour] - levelDb[b]);
                    }

                    if (b + (size_t) distance < (size_t) numBands)
                    {
                        const auto neighbour = b + (size_t) distance;
                        if (bandAlive[neighbour])
                            rightSupport = juce::jmax (rightSupport, levelDb[neighbour] - levelDb[b]);
                    }
                }

                // 自动 Recover 只认真实缺口；独立的手动 Brighten 稍后叠加，不受自动许可限制。
                const auto support = juce::jmin (leftSupport, rightSupport);
                if (support > automaticRecoverSupportDb && fastLevelDb[b] - referenceDb[b] < -deadZoneDb)
                {
                    automaticDeficitSeconds[b] = juce::jmin (automaticRecoverFullSeconds,
                                                             automaticDeficitSeconds[b] + hopSeconds);
                    const auto persistence = juce::jlimit (0.0, 1.0,
                        (automaticDeficitSeconds[b] - automaticRecoverStartSeconds)
                            / (automaticRecoverFullSeconds - automaticRecoverStartSeconds));
                    automaticWeight = persistence * juce::jlimit (0.0, 1.0, support - automaticRecoverSupportDb);
                }
                else
                    automaticDeficitSeconds[b] = 0.0;
            }
            else
                automaticDeficitSeconds[b] = 0.0;

            if (bandAlive[b])
            {
                // 动态修正只根据歌曲本身的偏差。Tame 保留原线性基础量，增强项只给当前频谱的真实局部峰，
                // 不放大全曲电平变化时慢参考的暂时误差。满刻度可压 8 dB；Recover 保持原有上限和缺口保护。
                const auto deviation = fastLevelDb[b] - referenceDb[b];
                const auto excess = deviation >  deadZoneDb ? deviation - deadZoneDb
                                  : deviation < -deadZoneDb ? deviation + deadZoneDb
                                  : 0.0;
                if (excess > 0.0 && tame > 0.0)
                {
                    // smoothedLevelDb 是本帧 Lfast 的频率邻域轮廓，未经慢时间参考；共同变响/变轻会相消。
                    // 稳定谱峰时局部超出量与 excess 一致，曲线达到 t*(1+t)；没有局部峰时只用原基础量。
                    const auto localPeakExcess = juce::jmax (0.0, fastLevelDb[b] - smoothedLevelDb[b] - deadZoneDb);
                    const auto extra = tame * tame * juce::jmin (excess, localPeakExcess);
                    target = -juce::jmin (excess * tame + extra, maxTameCorrectionDb * tame);
                    if (automaticTame > 0.0)
                    {
                        // 曲线非线性：先保留完整手动曲线，再限制「总量减手动量」的自动增量。
                        // automaticTame = 0 完全不进此分支，手动行为和原运算顺序不变。
                        const auto manualTame = juce::jmax (0.0, tame - automaticTame);
                        const auto manualExtra = manualTame * manualTame * juce::jmin (excess, localPeakExcess);
                        const auto manualDepth = juce::jmin (excess * manualTame + manualExtra, maxTameCorrectionDb * manualTame);
                        const auto automaticDepth = juce::jlimit (0.0, automaticTameMaxExtraDb, -target - manualDepth);
                        target = -(manualDepth + automaticDepth * automaticTameWeight);
                    }
                }
                else if (excess < 0.0 && recover > 0.0)
                {
                    // 只有自动份额按缺口证据衰减；手动 Recover 的动态修正仍沿用原式。
                    const auto permittedRecover = automaticRecover > 0.0
                        ? recover - automaticRecover + automaticRecover * automaticWeight : recover;
                    target = juce::jmin (-excess * permittedRecover, maxCorrectionDb * permittedRecover);
                }

                // 手动色调控制独立，不再被 Recover/Tame 二次缩小，也不被自动 Recover 许可吞掉。
                // 倾斜和动态修正的合计有界，随后统一走门限、原有每 hop 限速及插值。
                untilted[b] = target;
                target = juce::jlimit (-maxTameCorrectionDb, maxCorrectionDb, target + tiltPerBrighten[b] * brighten);

                // 静音门：L[b] < −60 dBFS 的段修正量归零（不把底噪提起来），门上 6 dB 内线性淡入。
                target *= gateWeight[b];
            }

            proposed[b] = target;
            recoverPermit[b] = manualRecover + automaticRecover * automaticWeight;
        }

        // 掩蔽转移放在限速之前。没有任何足够深的削减时 proposed 原样进入限速，与旧运算逐位相同。
        if (! (recover > 0.0 && tame > 0.0))
            maskingBoostDb.fill (0.0);
        else
        {
            PerBand<double> boost {};

            for (int masker = 0; masker < numBands; ++masker)
            {
                const auto maskerIndex = (size_t) masker;

                if (! bandAlive[maskerIndex] || untilted[maskerIndex] > -maskingMinCutDb)
                    continue;

                const auto cut = -untilted[maskerIndex];

                for (int distance = 1; distance <= maskingMaxDistance; ++distance)
                {
                    const auto band = masker + distance;

                    if (band < maskingLowBand || band > maskingHighBand)
                        continue;

                    const auto index = (size_t) band;
                    const auto permit = recoverPermit[index];

                    if (permit < 0.05 || gateWeight[index] < 0.5)
                        continue;

                    const auto gap = fastLevelDb[maskerIndex] - fastLevelDb[index];

                    if (gap <= 0.0 || gap > maskingMaxGapDb || fastLevelDb[index] < gateDb + 8.0)
                        continue;

                    const auto buried = gap - maskingFloorOffsetDb - maskingUpwardSlopeDb * (double) distance;

                    if (buried < maskingMinBuriedDb)
                        continue;

                    const auto room = maskingMaxBoostDb - boost[index];

                    if (room <= 0.0)
                        continue;

                    boost[index] += juce::jmin (room,
                                                juce::jmin (maskingTransfer * cut, 0.22 * buried)
                                                    * juce::jmin (permit, 1.0));
                }
            }

            const auto boostCoeff = maskingBoostSeconds <= 0.0 ? 0.0 : std::exp (-hopSeconds / maskingBoostSeconds);
            double totalBoost = 0.0;

            for (size_t index = 0; index < (size_t) numBands; ++index)
            {
                // 原始提升一帧一帧地闪时，先沿 hop 平滑再拿去改目标，避免扫频在邻段上来回抖。
                maskingBoostDb[index] = boost[index] + (maskingBoostDb[index] - boost[index]) * boostCoeff;

                if (maskingBoostDb[index] < 1.0e-4)
                    maskingBoostDb[index] = 0.0;

                boost[index] = maskingBoostDb[index];
                totalBoost += boost[index];
            }

            auto offset = 0.0;

            if (totalBoost >= maskingAnchorMinBoost)
            {
                auto weightSum = 0.0;
                auto gainSum = 0.0;

                for (int band = maskingLowBand; band <= maskingHighBand; ++band)
                {
                    const auto index = (size_t) band;

                    if (gateWeight[index] < 0.5 || (boost[index] <= 0.0 && untilted[index] > -0.2))
                        continue;

                    const auto weight = maskingLoudnessWeight (band) * gateWeight[index];
                    weightSum += weight;
                    gainSum += weight * (untilted[index] + boost[index]);
                }

                if (weightSum > 0.0)
                    offset = juce::jlimit (-maskingAnchorLimitDb, maskingAnchorLimitDb, gainSum / weightSum);
            }

            if (totalBoost > 0.0 || offset != 0.0)
            {
                for (int band = 0; band < numBands; ++band)
                {
                    const auto index = (size_t) band;
                    const auto interior = band >= maskingLowBand && band <= maskingHighBand && gateWeight[index] >= 0.5;
                    const auto paysForAnchor = offset != 0.0 && interior && untilted[index] < -0.2;

                    if (boost[index] <= 0.0 && ! paysForAnchor)
                        continue;

                    auto dynamic = untilted[index] + boost[index];

                    if (interior && (boost[index] > 0.0 || untilted[index] < -0.2))
                        dynamic -= offset;

                    auto revised = juce::jlimit (-maxTameCorrectionDb, maxCorrectionDb,
                                                 dynamic + tiltPerBrighten[index] * brighten);
                    revised *= gateWeight[index];
                    proposed[index] = revised;
                }
            }
        }

        if (frameParams.harshAmount > 0.0f || frameParams.targetAmount > 0.0f || masteringHasTail)
        {
            // Optional mastering corrections reuse this filter bank and analysis; no new audio path or latency.
            // Use the common-rate input tracker: per-band Welch time constants would confuse a level step with spectral tilt.
            // Centre both shapes on the same audible bands. A reference's recording level cannot become gain.
            double measuredMean = 0.0, desiredMean = 0.0, weights = 0.0;
            PerBand<double> desiredShape {};
            if (frameParams.targetAmount > 0.0f)
            {
                for (size_t b = 0; b < (size_t) numBands; ++b)
                {
                    desiredShape[b] = frameParams.hasTargetReference ? (double) frameParams.targetReferenceDb[b]
                                          : (double) frameParams.targetSlopeDbPerOct * ((double) b - 14.0) / 3.0;
                    const auto usableReference = ! frameParams.hasTargetReference || desiredShape[b] > -65.0;
                    if (b < 3 || b > 25 || ! bandAlive[b] || ! usableReference) continue;
                    const auto w = gateWeight[b];
                    measuredMean += w * (10.0 * std::log10 (juce::jmax (powerFloor, quickPower[b])));
                    desiredMean += w * desiredShape[b];
                    weights += w;
                }
            }
            if (weights > 0.0) { measuredMean /= weights; desiredMean /= weights; }
            double maximumHarsh = 0.0;
            masteringHasTail = false;
            const auto harshAttackCoeff = std::exp (-hopSeconds / 0.025);
            const auto harshReleaseCoeff = std::exp (-hopSeconds / 0.20);
            const auto targetCoeff = std::exp (-hopSeconds / (frameParams.targetAmount > 0.0f ? 2.0 : 0.15));
            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                double harsh = 0.0, correction = 0.0;
                const auto hz = (double) getBandCentreHz ((int) b);
                if (frameParams.harshAmount > 0.0f && hz >= 1900.0 && hz <= 8200.0 && bandAlive[b])
                {
                    const auto rising = bandPower[b] > 1.7 * juce::jmax (powerFloor, masteringPreviousPower[b]);
                    masteringOnsetHold[b] = rising ? 0.055 : juce::jmax (0.0, masteringOnsetHold[b] - hopSeconds);
                    // Neighbour support protects isolated tones; broad music with a persistent upper-mid peak is eligible.
                    const auto left = quickLevelDb[b - 3], right = quickLevelDb[b + 2];
                    const auto support = juce::jmin (gateWeight[b - 3], gateWeight[b + 2]);
                    const auto excess = juce::jmax (0.0, fastLevelDb[b] - 0.5 * (left + right) - 1.5);
                    const auto onsetPermit = masteringOnsetHold[b] > 0.0 ? 1.0 - (double) frameParams.transientPreserve : 1.0;
                    const auto frequencyWeight = juce::jlimit (0.0, 1.0, juce::jmin ((hz - 1500.0) / 1000.0, (9500.0 - hz) / 1500.0));
                    harsh = juce::jmin (3.0, excess * 0.65) * frameParams.harshAmount * support * frequencyWeight
                              * onsetPermit * (1.0 - 0.6 * harmonicProtection[b]);
                }
                masteringPreviousPower[b] = bandPower[b];
                const auto harshCoeff = (harsh > harshEnvelopeDb[b] ? harshAttackCoeff : harshReleaseCoeff);
                harshEnvelopeDb[b] = harsh + (harshEnvelopeDb[b] - harsh) * harshCoeff;
                if (frameParams.harshAmount <= 0.0f && harshEnvelopeDb[b] < 1.0e-5) harshEnvelopeDb[b] = 0.0;
                // Duck the additional reduction immediately at an onset, rather than carrying its release through the attack.
                const auto appliedHarsh = harshEnvelopeDb[b] * (masteringOnsetHold[b] > 0.0
                                         ? 1.0 - (double) frameParams.transientPreserve : 1.0);
                maximumHarsh = juce::jmax (maximumHarsh, appliedHarsh);
                if (frameParams.targetAmount > 0.0f && weights >= 6.0 && b >= 3 && b <= 25 && bandAlive[b]
                     && (! frameParams.hasTargetReference || desiredShape[b] > -65.0))
                {
                    const auto measured = 10.0 * std::log10 (juce::jmax (powerFloor, quickPower[b])) - measuredMean;
                    const auto error = desiredShape[b] - desiredMean - measured;
                    const auto outside = std::copysign (juce::jmax (0.0, std::abs (error) - 1.0), error);
                    correction = juce::jlimit (-2.5, 2.5, outside * 0.35) * frameParams.targetAmount * gateWeight[b];
                }
                masteringTargetDb[b] = correction + (masteringTargetDb[b] - correction) * targetCoeff;
                if (frameParams.targetAmount <= 0.0f && std::abs (masteringTargetDb[b]) < 1.0e-5) masteringTargetDb[b] = 0.0;
                masteringHasTail = masteringHasTail || harshEnvelopeDb[b] != 0.0 || masteringTargetDb[b] != 0.0;
                publishedTargetDb[b].store ((float) masteringTargetDb[b] * frameAppliedAmount, std::memory_order_relaxed);
                // Skip even the addition at exact zero: old sessions keep their original arithmetic.
                if (appliedHarsh != 0.0 || masteringTargetDb[b] != 0.0)
                    proposed[b] = juce::jlimit (-maxTameCorrectionDb, maxCorrectionDb, proposed[b] - appliedHarsh + masteringTargetDb[b]);
            }
            publishedHarshDb.store ((float) maximumHarsh * frameAppliedAmount, std::memory_order_relaxed);
        }

        for (size_t b = 0; b < (size_t) numBands; ++b)
            targetGainDb[b] = juce::jlimit (hopStartGainDb[b] - maxStepPerHopDb,
                                            hopStartGainDb[b] + maxStepPerHopDb,
                                            proposed[b]);
    }

    void BalanceModule::getTargetCorrectionDb (BandGains& out) const noexcept
    {
        for (size_t b = 0; b < out.size(); ++b) out[b] = publishedTargetDb[b].load (std::memory_order_relaxed);
    }

    void BalanceModule::publishGains() noexcept
    {
        // 给界面的是「真正作用到输出上的修正量」：旁通淡入淡出中按比例、完全旁通为 0（同 Color / Limit 的表，评审 R1-03）。
        // 比例取本帧片 0（hop 边界上）时的旁通状态，与一口气做完时发布的值逐位相同。
        const auto applied = frameAppliedAmount;

        for (size_t b = 0; b < (size_t) numBands; ++b)
            publishedGainsDb[b].store ((float) targetGainDb[b] * applied, std::memory_order_relaxed);
    }
}
