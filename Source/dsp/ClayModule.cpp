/*
  ==============================================================================

    ClayModule.cpp

  ==============================================================================
*/

#include "ClayModule.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// 只用标量写法（不做 AArch64 两声道打包）：测试服务器是 x86_64，打包路径在那里既编不到也测不到；Clay 每立体声样本
// 只有 5 次双二阶 + 4 级滑动和（切换分频点时：预热的 2(L − 1) 个样本里 12 级、淡化的 20 ms 里 8 级），标量已远在 CPU 预算内
// （设计规格-Clay-分析决策与验收 第 5 节）。
// 两声道走同一串表达式，左右互换对称、L = R 时两路逐位相同都不依赖编译器怎么融合乘加。

namespace celadon
{
    namespace
    {
        // 20 ms（设计规格-Clay 第 4 节）：低切频率的对数域平滑、a 与 widthDb 的线性平滑、低切的输出选择淡化、分频点切换的
        // 两套低通交叉淡化（设计规格-Clay-根治修订 第 2 节）、旁通淡化都用这一个时长。
        constexpr double smoothingSeconds          = 0.02;
        constexpr int    coefficientUpdateInterval = 64;      // 每 64 样本按平滑值重算一次系数（同 Balance）；低通切换也只在这些点上开始

        // 分频点切换的分摊预热（T-020）：新低通要先吃掉最近 4(L − 1) 个历史输入才追得上。一次喂完会让那一块多做 4(L − 1) 次
        // 滑动和（192 kHz / 40 Hz 为 4604 次，实测多 18 µs，192k / 32 最坏块 +11 个百分点）；改为每个实时样本除了本样本再多喂
        // 这么多个历史样本，2(L − 1) 个样本后追上（192 kHz / 40 Hz 约 12 ms），每块只多 2 × 块长次（192k / 32 约 +0.15 个百分点）。
        // 追上后到下一个网格点才开始 20 ms 淡化：切换比一次喂完晚最多 2(L − 1) + 64 个样本，新低通吃到的输入序列不变、输出逐位相同。
        // 切换开始时新低通的 reset 也是 O(1)（LinearPhaseLowpass 不清历史环），切换开始那一块不再有 O(L) 的一次性工作。
        constexpr int    lowpassCatchUpRate = 2;

        // ① 4 阶 Butterworth 高通 = 两级二阶高通，Q = 1 / (2 cos(π/8)) 与 1 / (2 cos(3π/8))。RBJ 高通在自己的 f0 处增益恰为 Q，
        // 两级之积 = 1/√2（−3.01 dB）：设定频率就是整条低切的半功率点（验收「−3 dB 点偏差 ≤ 1 %」的依据）。
        constexpr double cutFirstQ  = 0.54119610014619698;
        constexpr double cutSecondQ = 1.3065629648763766;

        // 滤波器状态的归零门槛（只在系数重算的网格点上检查，归零时刻与块长无关）：
        //  - 平时 1e-30：静音里状态指数衰减到这里就归零，不会掉进非正规数（同 Balance / Color）。
        //  - 高架系数逐位直通（widthDb 精确为 0）时 1e-10（约 −200 dBFS）：这时状态只是加宽拧回 0 留下的余振，按极点
        //    （48 kHz 下半径约 0.83 / 样本）自然衰减；但有信号时舍入误差可能让它在信号的 ulp 量级上徘徊，迟迟到不了 1e-30
        //    （同 Balance 的 zeroGainStateSnapThreshold）。归零那一刻改变的量 ≤ 1e-10，之后高架精确直通，
        //    「三级都不作用 → 逐位取纯延迟线」才能在加宽拧回 0 后几毫秒内恢复。
        constexpr double stateSnapThreshold         = 1.0e-30;
        constexpr double identityShelfSnapThreshold = 1.0e-10;

        // ④ 峰值守护（ADR-014，算法与理由见 ClayModule.h 文件头）。
        constexpr double peakGuardWindowSeconds   = 0.008;   // W：局部峰值 E 的半窗。低切 4 阶高通在 2·fc 处相位超前约 78°，
                                                             // fc 10 ～ 60 Hz 下 30 Hz 以上的峰值前移 ≤ 7 ms；稳态正弦幅度不变时守护不动作
        constexpr double peakGuardAttackSeconds   = 0.003;   // A：前瞻最小 + 滑动平均的长度（g 在峰值前 3 ms 内降到位）
        constexpr double peakGuardReleaseSeconds  = 0.1;     // g 从 0 线性回到 1 用 100 ms（与限幅器的释放同一量级）
        constexpr double peakGuardEnvelopeSeconds = 1.0;     // P 的下降时间常数
        constexpr double peakGuardRelativeGain    = 0.50118723362727224;   // τ = −6 dB（10^(−6/20)）
        constexpr double peakGuardFloor           = 0.01;    // F = −40 dBFS
        constexpr int    peakGuardResyncInterval  = 65536;   // 滑动平均的和按固定网格从环里精确重算（同 LinearPhaseLowpass）
        constexpr double peakGuardEnvelopeSnap    = 1.0e-30; // P 低于它归零（不进非正规数）

        bool sameBits (float a, float b) noexcept
        {
            std::uint32_t x = 0, y = 0;
            std::memcpy (&x, &a, sizeof (x));
            std::memcpy (&y, &b, sizeof (y));
            return x == y;
        }
    }

    // 手动收单的下限就是低通最长的那一档：它决定两套低通的分配长度与模块的固定延迟 D（设计规格-Clay-根治修订 第 2 节）。
    static_assert (static_cast<double> (ClayModule::minMonoHz) == LinearPhaseLowpass::minCutoffHz,
                   "ClayModule::minMonoHz must equal LinearPhaseLowpass::minCutoffHz (it sets the fixed latency D)");

    //==============================================================================
    // 线性斜坡：值只由 (from, to, position) 决定，与之前怎么切块、取没取过值无关。
    void ClayModule::LinearRamp::setLength (int numSamples) noexcept
    {
        length    = juce::jmax (1, numSamples);
        stepScale = 1.0 / static_cast<double> (length);
        snapToTarget();
    }

    void ClayModule::LinearRamp::setTarget (double newTarget) noexcept
    {
        // 宿主每块都会把同一个值再设一遍：目标没变就让斜坡照常走完，不重启。
        if (newTarget == to)
            return;

        // 新斜坡从「最近处理过的那个样本用的值」出发，走满一个完整时长（同 juce::SmoothedValue）。
        from     = currentValue();
        to       = newTarget;
        position = 0;
    }

    void ClayModule::LinearRamp::restart (double start, double target) noexcept
    {
        // 分频点切换的交叉淡化：每次都从 0（旧低通）完整走到 1（新低通），与上一次淡化停在哪里无关。
        from     = start;
        to       = target;
        position = 0;
    }

    void ClayModule::LinearRamp::snapToTarget() noexcept
    {
        from     = to;
        position = length;
    }

    void ClayModule::LinearRamp::advance (int numSamples) noexcept
    {
        // 封顶在 length：长时间静止也不会溢出。调用方每次最多推进一个 64 样本的段。
        position = juce::jmin (length, position + numSamples);
    }

    double ClayModule::LinearRamp::currentValue() const noexcept
    {
        return position >= length ? to : from + (to - from) * (static_cast<double> (position) * stepScale);
    }

    double ClayModule::LinearRamp::valueAt (int offset) const noexcept
    {
        // 走到第 length 步时直接给目标本身：端点逐位精确（0 / 1 的淡化端点才能「逐位取干路 / 逐位取处理后」）。
        const auto step = position + offset + 1;
        return step >= length ? to : from + (to - from) * (static_cast<double> (step) * stepScale);
    }

    ClayModule::FrequencyRamp::FrequencyRamp (double initialHz) noexcept
        : targetHz (initialHz)
    {
        logHz.from = std::log (initialHz);
        logHz.to   = logHz.from;
    }

    void ClayModule::FrequencyRamp::setTarget (double hz) noexcept
    {
        if (hz == targetHz)
            return;

        targetHz = hz;
        logHz.setTarget (std::log (hz));
    }

    double ClayModule::FrequencyRamp::valueAt (int offset) const noexcept
    {
        return logHz.reachesTargetAt (offset) ? targetHz : std::exp (logHz.valueAt (offset));
    }

    //==============================================================================
    double ClayModule::processBiquad (double x, const BiquadCoefficients& c, BiquadState& s) noexcept
    {
        // 转置直接 II 型（同 ColorModule::processBiquad）。乘积先落到具名变量再相减：编译器默认只在同一表达式内做乘加融合，
        // 这样 b1·x − a1·y 在 b1 == a1、y == x 时是两个逐位相同的乘积之差 = 精确的 0——widthDb = 0 的高架
        // （系数 1, c, d, c, d，推导见 designWidthShelf）从零状态出发状态恒为 0。
        const auto y = c.b0 * x + s.s1;

        const auto bx1 = c.b1 * x;
        const auto ay1 = c.a1 * y;
        s.s1 = (bx1 - ay1) + s.s2;

        const auto bx2 = c.b2 * x;
        const auto ay2 = c.a2 * y;
        s.s2 = bx2 - ay2;

        return y;
    }

    void ClayModule::snapState (BiquadState& s, double threshold) noexcept
    {
        // 影子滤波器一直在跑，输入里混进 NaN / Inf 时它的状态会变成非有限值并一直留着（关着的级也不例外），
        // 之后打开那一级就只会输出 NaN。整级清零、下一个样本起重新积累。链的 NaN 保护本来也会 reset 整个模块，这里不依赖它。
        // （② 的低通自己清洗输入，永远不会卡住，见 LinearPhaseLowpass::process。）
        if (! std::isfinite (s.s1) || ! std::isfinite (s.s2))
        {
            s = BiquadState {};
            return;
        }

        if (std::abs (s.s1) < threshold)
            s.s1 = 0.0;

        if (std::abs (s.s2) < threshold)
            s.s2 = 0.0;
    }

    ClayModule::BiquadCoefficients ClayModule::designHighpass (double fs, double frequency, double q) noexcept
    {
        // RBJ Audio EQ Cookbook 二阶高通（与 ColorModule::designHighpass 同式）；频率钳在 0.45 fs 以下（设计规格-Clay 第 4 节）。
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

    ClayModule::BiquadCoefficients ClayModule::designWidthShelf (double gainDb) const noexcept
    {
        // RBJ 高架，A = 10^(dB/40)，平台处增益 A² = widthDb、f0 处恰为一半（dB）、直流为 1（0 dB）。
        // gainDb = 0 → A = 1（不经 pow，精确）：带 (A − 1) 的项精确为 0，(A + 1) = 2、2√A·α = 2α 两边一样，乘 A = 1 是精确运算，
        // 所以 b0 == a0、b1 == a1、b2 == a2 逐位相等（推导同 ColorModule::designShelf；乘加融合与否结果都一样，因为被融合的
        // 乘积 0·cos 与 2·cos 本身精确），归一后 b0 = 1、b1 = a1、b2 = a2——逐位直通，状态从 0 出发恒为 0。
        const auto a       = gainDb == 0.0 ? 1.0 : std::pow (10.0, gainDb / 40.0);
        const auto aPlus1  = a + 1.0;
        const auto aMinus1 = a - 1.0;
        const auto twoSqrtAAlpha = 2.0 * std::sqrt (a) * shelfAlpha;

        const auto b0 =        a * (aPlus1 + aMinus1 * shelfCosW0 + twoSqrtAAlpha);
        const auto b1 = -2.0 * a * (aMinus1 + aPlus1 * shelfCosW0);
        const auto b2 =        a * (aPlus1 + aMinus1 * shelfCosW0 - twoSqrtAAlpha);
        const auto a0 =            (aPlus1 - aMinus1 * shelfCosW0 + twoSqrtAAlpha);
        const auto a1 =      2.0 * (aMinus1 - aPlus1 * shelfCosW0);
        const auto a2 =            (aPlus1 - aMinus1 * shelfCosW0 - twoSqrtAAlpha);

        BiquadCoefficients c;
        c.b0 = b0 / a0;   // b0 == a0 时精确等于 1.0
        c.b1 = b1 / a0;
        c.b2 = b2 / a0;
        c.a1 = a1 / a0;
        c.a2 = a2 / a0;
        return c;
    }

    //==============================================================================
    void ClayModule::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate  = spec.sampleRate;
        numChannels = juce::jlimit (1, 2, static_cast<int> (spec.numChannels));

        // 所有平滑 / 淡化都是 20 ms（按样本数计：44.1 k 882、48 k 960、96 k 1920、192 k 3840）。
        const auto rampLength = sampleRate > 0.0 ? static_cast<int> (std::lround (smoothingSeconds * sampleRate)) : 1;
        cutFrequencyRamp.logHz.setLength (rampLength);
        cutMixRamp.setLength (rampLength);
        monoAmountRamp.setLength (rampLength);
        lowpassFade.setLength (rampLength);
        widthRamp.setLength (rampLength);
        bypassRamp.setLength (rampLength);

        if (sampleRate > 0.0)
        {
            // ③ 高架与增益无关的部分：f0 = 2 kHz 固定（钳在 0.45 fs 以下）。斜率 S = 1 时 RBJ 的
            // alpha = sin(w0)/2 · √((A + 1/A)(1/S − 1) + 2) 根号里恒为 2，与增益无关，这里算一次。
            const auto w0 = juce::MathConstants<double>::twoPi
                              * juce::jmin (static_cast<double> (widthShelfHz), 0.45 * sampleRate) / sampleRate;
            shelfCosW0 = std::cos (w0);
            shelfAlpha = std::sin (w0) / 2.0 * std::sqrt (2.0);

            // ② 两套低通都按最长的一档（分频点 40 Hz）分配；模块的固定延迟 D = 这一档的群延迟 2(L_max − 1)——任何分频点的
            // 低通输出（晚 2(L − 1)）都能再补 D − 2(L − 1) ≥ 0 对齐到 S_d，延迟与参数无关、只上报一次。
            lowpassMaxLength = LinearPhaseLowpass::maxLengthFor (sampleRate);
            coreDelay        = LinearPhaseLowpass::groupDelayFor (lowpassMaxLength);

            for (auto& lowpass : lowpasses)
                lowpass.prepare (lowpassMaxLength);
        }
        else
        {
            lowpassMaxLength = 2;
            coreDelay        = 0;
        }

        // ④ 按采样率定前瞻 G；上报的固定延迟 = D + G（采样率无效时两者都是 0，process 什么都不做）。
        peakGuard.prepare (sampleRate, numChannels);
        latencySamples = coreDelay > 0 ? coreDelay + peakGuard.lookahead : 0;

        // 延迟线各长 D；S 历史环长 2D + 1：S_d 读 D 个样本之前，切换预热最早读 D + 2(L − 1) ≤ 2D 个样本之前。
        const auto delayLength = static_cast<std::size_t> (coreDelay);

        for (auto& channel : dryDelay)
            channel.assign (delayLength, 0.0f);

        midDelay.assign (delayLength, 0.0);
        cutFlagDelay.assign (delayLength, static_cast<std::uint8_t> (0));
        sideHistory.assign (coreDelay > 0 ? 2 * delayLength + 1 : std::size_t { 0 }, 0.0);

        reset();
    }

    int ClayModule::latencyFor (double fs) noexcept
    {
        if (! (fs > 0.0))
            return 0;

        return LinearPhaseLowpass::groupDelayFor (LinearPhaseLowpass::maxLengthFor (fs)) + PeakGuard::windowFor (fs) + PeakGuard::attackFor (fs);
    }

    ClayModule::PressureReport ClayModule::getLastProcessPressure() const noexcept
    {
        PressureReport report;

        if (peakGuard.pressureBlocks > 0 && sampleRate > 0.0)
        {
            report.meanDb = static_cast<float> (peakGuard.pressureSumDb / static_cast<double> (peakGuard.pressureBlocks));
            report.seconds = static_cast<double> (peakGuard.pressureBlocks) * static_cast<double> (peakGuard.costBlockLength) / sampleRate;
        }

        return report;
    }

    ClayModule::PeakGuardStats ClayModule::getAndResetPeakGuardStats() noexcept
    {
        const auto result = peakGuard.stats;
        peakGuard.stats = PeakGuardStats {};
        return result;
    }

    void ClayModule::reset()
    {
        for (auto& channel : cutStates)
            for (auto& state : channel)
                state = BiquadState {};

        shelfState = BiquadState {};

        // 平滑器与淡化直接落到当前参数的目标（先按 params 设一次目标：prepare 前没调过 setParams 时也对）。
        applyTargets();
        cutFrequencyRamp.snapToTarget();
        cutMixRamp.snapToTarget();
        monoAmountRamp.snapToTarget();
        widthRamp.snapToTarget();
        bypassRamp.snapToTarget();

        // ② 两套低通清零、直接取目标长度，不淡化：reset 之后与「prepare 前就设好这个分频点」的模块逐位相同。
        for (auto& lowpass : lowpasses)
            lowpass.reset (targetLowpassLength);

        activeLowpass = 0;
        lowpassWarming = false;
        lowpassBacklog = 0;
        lowpassFading = false;
        lowpassFade.snapToTarget();

        // 延迟线清零 = 「此前输入全是 0」：重置后的头 D + G 个输出是静音（链的 restartWithFadeIn 把整段延迟算进淡入前的静音段）。
        clearDelayLines();
        peakGuard.reset();

        // 系数重算网格从这里起算：reset 后第 0、64、128 … 个样本（与块长无关）。
        samplesUntilCoefficientUpdate = 0;

        if (sampleRate <= 0.0)
            return;

        // 按目标直接设计（prepare 可能换了采样率，旧系数不能沿用）：reset 之后 isPassThrough() 立刻反映真实状态。
        designCutFilters (cutFrequencyRamp.targetHz);
        designWidthFilter (widthRamp.to);
    }

    void ClayModule::clearDelayLines() noexcept
    {
        for (auto& channel : dryDelay)
            std::fill (channel.begin(), channel.end(), 0.0f);

        std::fill (midDelay.begin(), midDelay.end(), 0.0);
        std::fill (cutFlagDelay.begin(), cutFlagDelay.end(), static_cast<std::uint8_t> (0));
        std::fill (sideHistory.begin(), sideHistory.end(), 0.0);

        delayPosition = 0;
        sideWritePosition = 0;
        cutSamplesInDelay = 0;
    }

    //==============================================================================
    void ClayModule::setParams (const Params& newParams)
    {
        // 范围见头文件，模块不信任外部值。非有限值：低切按「关」、a 与 widthDb 按 0（中性）；monoHz 保持原值——
        // 它只在 a > 0 时起作用，没有「中性」的频率可回退。0 < lowCutHz < 10 按 10 Hz（头文件「0 = 关；否则钳到 10 ～ 60」）。
        const auto lowCut = newParams.lowCutHz;
        params.lowCutHz = (std::isfinite (lowCut) && lowCut > 0.0f) ? juce::jlimit (minLowCutHz, maxLowCutHz, lowCut) : 0.0f;

        if (std::isfinite (newParams.monoHz))
            params.monoHz = juce::jlimit (minMonoHz, maxMonoHz, newParams.monoHz);

        params.monoAmount = std::isfinite (newParams.monoAmount) ? juce::jlimit (0.0f, 1.0f, newParams.monoAmount) : 0.0f;
        params.widthDb    = std::isfinite (newParams.widthDb) ? juce::jlimit (minWidthDb, maxWidthDb, newParams.widthDb) : 0.0f;
        params.bypass     = newParams.bypass;

        applyTargets();
    }

    void ClayModule::applyTargets() noexcept
    {
        const auto lowCutOn = params.lowCutHz > 0.0f;
        cutMixRamp.setTarget (lowCutOn ? 1.0 : 0.0);

        // 低切关着时影子滤波器停在上次开着的频率（从没开过就是 10 Hz）：再开同一频率时状态现成，只做输出淡化、频率不用滑；
        // 从没开过时从 10 Hz 滑到目标——10 Hz 的 4 阶高通在 20 Hz 只衰减 0.02 dB，与干路的淡化几乎无感。
        if (lowCutOn)
            cutFrequencyRamp.setTarget (static_cast<double> (params.lowCutHz));

        // ② 分频点 → 低通长度（整数）。长度的变化不走斜坡：在网格点换一套低通、两套 20 ms 交叉淡化（updateLowpassSwitch）。
        // 没 prepare 时（采样率 0）lengthFor 给 2；prepare 末尾的 reset 会按真实采样率再算一次。
        targetLowpassLength = juce::jlimit (2, lowpassMaxLength,
                                            LinearPhaseLowpass::lengthFor (sampleRate, static_cast<double> (params.monoHz)));

        monoAmountRamp.setTarget (static_cast<double> (params.monoAmount));
        widthRamp.setTarget (static_cast<double> (params.widthDb));
        bypassRamp.setTarget (params.bypass ? 1.0 : 0.0);
    }

    //==============================================================================
    void ClayModule::designCutFilters (double frequency) noexcept
    {
        cutCoefficients[0] = designHighpass (sampleRate, frequency, cutFirstQ);
        cutCoefficients[1] = designHighpass (sampleRate, frequency, cutSecondQ);
        cutHzForCoefficients = frequency;
    }

    void ClayModule::designWidthFilter (double gainDb) noexcept
    {
        shelfCoefficients = designWidthShelf (gainDb);
        widthDbForCoefficients = gainDb;
    }

    void ClayModule::updateCoefficients() noexcept
    {
        // 网格点（reset 后每 64 样本）：取本段第一个样本的平滑值重算系数，值没变就不算（静止时不碰三角函数）。
        // 低切频率在对数域平滑、widthDb 线性平滑，都只在这里取值；a 与各淡化是逐样本取值的，不在这里。
        const auto cutHz = cutFrequencyRamp.valueAt (0);

        if (cutHz != cutHzForCoefficients)
            designCutFilters (cutHz);

        const auto gainDb = widthRamp.valueAt (0);

        if (gainDb != widthDbForCoefficients)
            designWidthFilter (gainDb);

        // 状态归零 / 清洗也在网格上做：归零时刻与块长无关。
        for (auto& channel : cutStates)
            for (auto& state : channel)
                snapState (state, stateSnapThreshold);

        snapState (shelfState, widthDbForCoefficients == 0.0 ? identityShelfSnapThreshold : stateSnapThreshold);

        // 分频点的切换也只在网格点上开始 / 收尾：切换时刻只取决于参数在哪个样本改变，与块长无关。
        updateLowpassSwitch();
    }

    void ClayModule::updateLowpassSwitch() noexcept
    {
        // 上一次切换的淡化已经走完（最后一个淡化样本取的就是新低通本身）：新低通转正，旧的闲置、下一次切换时重建。
        if (lowpassFading && ! lowpassFade.isRamping())
        {
            activeLowpass = 1 - activeLowpass;
            lowpassFading = false;
        }

        // 预热追上了（上一段里最后一个历史样本已喂完）：从这个网格点开始两套低通的 20 ms 淡化。
        if (lowpassWarming && lowpassBacklog == 0)
        {
            lowpassWarming = false;
            lowpassFade.restart (0.0, 1.0);
            lowpassFading = true;
        }

        // 预热或淡化中来的新目标等这次切换走完再处理（上面转正之后的网格点）；都不在进行、目标长度与正在用的不同 → 开始切换。
        // 目标只取最新的 monoHz：拖旋钮时是一串切换，中间值被跳过也没关系（每一段都是两套完整低通之间的线性淡化）。
        if (! lowpassWarming && ! lowpassFading
            && targetLowpassLength != lowpasses[static_cast<std::size_t> (activeLowpass)].getLength())
            startLowpassSwitch (targetLowpassLength);
    }

    void ClayModule::startLowpassSwitch (int newLength) noexcept
    {
        auto& incoming = lowpasses[static_cast<std::size_t> (1 - activeLowpass)];
        incoming.reset (newLength);

        // 预热（分摊，见 lowpassCatchUpRate）：新低通在时刻 m 的输入是 S[m − offset]（offset = D − 2(L − 1)，输出才对齐 S_d）。
        // 时刻 n（= 下一个要处理的样本）的输出要用到 m = n − 4(L − 1) … n 的输入，所以欠着 4(L − 1) 个历史样本；处理循环从最早的
        // 那个起按顺序补喂，每个实时样本最多多喂 lowpassCatchUpRate 个。喂给它的序列与「在这里一次喂完、再逐样本跟上」完全相同，
        // 追上以后的输出逐位一样（与一直在跑的同长度低通只差舍入）。最早读到 S[n − D − 2(L − 1)]，不超过 2D 个样本之前，
        // 仍在长 2D + 1 的历史环里；追赶中读得越来越近。
        lowpassBacklog = 2 * LinearPhaseLowpass::groupDelayFor (incoming.getLength());
        lowpassWarming = true;
    }

    void ClayModule::advanceRamps (int numSamples) noexcept
    {
        cutFrequencyRamp.advance (numSamples);
        cutMixRamp.advance (numSamples);
        monoAmountRamp.advance (numSamples);
        lowpassFade.advance (numSamples);
        widthRamp.advance (numSamples);
        bypassRamp.advance (numSamples);
    }

    //==============================================================================
    bool ClayModule::stagesIdle (int channelsToUse) const noexcept
    {
        // 「此刻起三级都不作用，且在参数不变的前提下一直保持」——只看影响输出的量，频率变化不算（见头文件）。
        // ① 输出选择静止在干路，且延迟线里已没有低切参与过的样本（① 在延迟之前，淡化要再过 D 个样本才走出延迟线）。
        if (cutMixRamp.isRamping() || cutMixRamp.to != 0.0 || cutSamplesInDelay != 0)
            return false;

        // 单声道只有 ①（② ③ 只动 S，单声道没有 S）。
        if (channelsToUse < 2)
            return true;

        // ② a 静止在 0：S' 直接取 S_d，低通不参与（它照样在影子里跑、切换分频点，都不影响输出）。
        if (monoAmountRamp.isRamping() || monoAmountRamp.to != 0.0)
            return false;

        // ③ 增益静止在 0、当前系数已是逐位直通、余振已归零：此后高架对任何样本都直接取 S'（见 processStereo）。
        return ! widthRamp.isRamping() && widthRamp.to == 0.0 && widthDbForCoefficients == 0.0
                 && shelfState.s1 == 0.0 && shelfState.s2 == 0.0;
    }

    bool ClayModule::isPassThrough() const noexcept
    {
        // 头文件的定义：此刻起输出 = 输入逐位延迟 D + G。旁通淡化中是处理后与干路的混合，不算；完全旁通（静止）时每个样本都取
        // 纯延迟线；否则要三级都不作用（单声道模块只看 ①）。④ 的前瞻环里还有「处理后 ≠ 干路」的样本时，它们还要再输出出去。
        if (bypassRamp.isRamping() || peakGuard.processedInRing != 0)
            return false;

        return bypassRamp.to >= 1.0 || stagesIdle (numChannels);
    }

    //==============================================================================
    void ClayModule::process (juce::AudioBuffer<float>& io)
    {
        const auto numSamples    = io.getNumSamples();
        const auto channelsToUse = juce::jmin (numChannels, io.getNumChannels());

        peakGuard.pressureSumDb = 0.0;
        peakGuard.pressureBlocks = 0;

        if (sampleRate <= 0.0 || coreDelay <= 0 || numSamples <= 0 || channelsToUse <= 0)
            return;

        // 按「到下一个系数重算网格点」切成最长 64 样本的段：重算与分频点切换落在从 reset 起算的固定绝对位置上，输出与块长无关；
        // 逐样本处理、延迟线是环，样本数超过 prepare 的块长也照常（0 分配）。输出总要写：即使整段逐位纯延迟，
        // 写回的也是 D 个样本之前的输入。
        for (int offset = 0; offset < numSamples;)
        {
            if (samplesUntilCoefficientUpdate == 0)
            {
                updateCoefficients();
                samplesUntilCoefficientUpdate = coefficientUpdateInterval;
            }

            const auto length = juce::jmin (numSamples - offset, samplesUntilCoefficientUpdate);

            if (channelsToUse >= 2)
                processStereo (io.getWritePointer (0, offset), io.getWritePointer (1, offset), length);
            else
                processMono (io.getWritePointer (0, offset), length);

            advanceRamps (length);
            samplesUntilCoefficientUpdate -= length;
            offset += length;
        }
    }

    void ClayModule::processStereo (float* left, float* right, int numSamples) noexcept
    {
        // 系数、状态与各环的位置拷成局部量（循环里留在寄存器，同 BalanceModule::filterChannelScalar），段尾写回。
        // 原地处理：每个样本先读输入、再写输出，同一位置不会读到本段已写过的值。
        const auto cutFirst  = cutCoefficients[0];
        const auto cutSecond = cutCoefficients[1];
        const auto sideShelf = shelfCoefficients;
        const auto shelfIsIdentity = widthDbForCoefficients == 0.0;

        auto leftFirst   = cutStates[0][0];
        auto leftSecond  = cutStates[0][1];
        auto rightFirst  = cutStates[1][0];
        auto rightSecond = cutStates[1][1];
        auto sideShelfState = shelfState;

        // 延迟线（容量正好 D）：同一个位置先读出 D 个样本之前写的，再写本样本。
        const auto delayLength = coreDelay;
        auto* delayedInputLeft  = dryDelay[0].data();
        auto* delayedInputRight = dryDelay[1].data();
        auto* delayedMids       = midDelay.data();
        auto* delayedCutFlags   = cutFlagDelay.data();
        auto position       = delayPosition;
        auto flaggedInDelay = cutSamplesInDelay;

        // S 历史环与低通：正在用的那套读 S 延迟 D − 2(L − 1)，输出就对齐 S_d；预热 / 淡化中新的那套按它自己的 L 对齐。
        // 两套低通、预热与淡化的起止只在网格点换（本段内不变）；预热欠着的历史样本数逐样本减少。
        auto* history = sideHistory.data();
        const auto historyLength = static_cast<int> (sideHistory.size());
        auto writePosition = sideWritePosition;
        auto& current  = lowpasses[static_cast<std::size_t> (activeLowpass)];
        auto& incoming = lowpasses[static_cast<std::size_t> (1 - activeLowpass)];
        const auto currentOffset  = delayLength - LinearPhaseLowpass::groupDelayFor (current.getLength());
        const auto incomingOffset = delayLength - LinearPhaseLowpass::groupDelayFor (incoming.getLength());
        const auto fading = lowpassFading;
        const auto warming = lowpassWarming;
        auto backlog = lowpassBacklog;

        // 历史环里的第 index 格（index 可以是负的，最多往回一整圈）。
        const auto historyAt = [history, historyLength] (int index) noexcept
        {
            return history[index < 0 ? index + historyLength : index];
        };

        // ④ 要的干路（输入延迟 D，float 原样）按段暂存；段长 ≤ 64（process 按系数网格切段）。
        jassert (numSamples <= coefficientUpdateInterval);
        std::array<float, coefficientUpdateInterval> dryLefts {}, dryRights {};

        for (int i = 0; i < numSamples; ++i)
        {
            const auto inputLeft  = left[i];
            const auto inputRight = right[i];
            const auto dryLeft  = static_cast<double> (inputLeft);
            const auto dryRight = static_cast<double> (inputRight);

            // ① 低切：两声道各过两级高通（一直在跑），输出按 cutMix 在干路与低切后之间选择 / 线性淡化；
            //    端点 0 / 1 直接取对应一路（不做乘法，逐位）。这一级在延迟之前（输入时刻）。
            const auto cutLeft   = processBiquad (processBiquad (dryLeft,  cutFirst, leftFirst),  cutSecond, leftSecond);
            const auto cutRight  = processBiquad (processBiquad (dryRight, cutFirst, rightFirst), cutSecond, rightSecond);
            const auto cutAmount = cutMixRamp.valueAt (i);

            auto selectedLeft  = dryLeft;
            auto selectedRight = dryRight;

            if (cutAmount >= 1.0)
            {
                selectedLeft  = cutLeft;
                selectedRight = cutRight;
            }
            else if (cutAmount > 0.0)
            {
                selectedLeft  = dryLeft  + (cutLeft  - dryLeft)  * cutAmount;
                selectedRight = dryRight + (cutRight - dryRight) * cutAmount;
            }

            // M/S 编码：M = (L + R) / 2，S = (L − R) / 2。
            const auto mid  = (selectedLeft + selectedRight) * 0.5;
            const auto side = (selectedLeft - selectedRight) * 0.5;

            // 延迟线：输入原样（纯延迟 / 旁通干路）、M、以及「这个样本低切参与了没有」。
            const auto delayedLeft   = delayedInputLeft[position];
            const auto delayedRight  = delayedInputRight[position];
            const auto delayedMid    = delayedMids[position];
            const auto delayedWasCut = delayedCutFlags[position] != 0;
            const auto cutNow        = cutAmount > 0.0;

            dryLefts[static_cast<std::size_t> (i)]  = delayedLeft;
            dryRights[static_cast<std::size_t> (i)] = delayedRight;

            delayedInputLeft[position]  = inputLeft;
            delayedInputRight[position] = inputRight;
            delayedMids[position]       = mid;
            delayedCutFlags[position]   = static_cast<std::uint8_t> (cutNow ? 1 : 0);
            flaggedInDelay += (cutNow ? 1 : 0) - (delayedWasCut ? 1 : 0);
            position = position + 1 == delayLength ? 0 : position + 1;

            // ② 线性相位收单：S 写进历史环；S_d = D 个样本之前的 S；低通一直在跑（影子运行，a = 0、旁通时也跑）。
            //    分频点切换：新低通先预热（按时间顺序补喂欠着的历史样本，本样本的输入排在最后；输出不用），追上后下一个网格点起
            //    两套低通 20 ms 线性交叉淡化，最后一个淡化样本直接取新低通（端点逐位）。
            history[writePosition] = side;
            const auto delayedSide = historyAt (writePosition - delayLength);
            auto lowSide = current.process (historyAt (writePosition - currentOffset));

            if (warming)
            {
                // 欠着的是时刻 n − backlog … n − 1 的输入，加上本样本共 backlog + 1 个；按顺序喂最多 1 + lowpassCatchUpRate 个。
                // 最早读到 S[n − offset − backlog]，backlog ≤ 4(L − 1) → 不超过 D + 2(L − 1) ≤ 2D 个样本之前。
                const auto steps = juce::jmin (backlog, lowpassCatchUpRate) + 1;

                for (int step = 0; step < steps; ++step)
                    incoming.process (historyAt (writePosition - incomingOffset - (backlog - step)));

                backlog -= steps - 1;
            }
            else if (fading)
            {
                const auto incomingLow = incoming.process (historyAt (writePosition - incomingOffset));
                const auto weight = lowpassFade.valueAt (i);
                lowSide = weight >= 1.0 ? incomingLow : lowSide + (incomingLow - lowSide) * weight;
            }

            writePosition = writePosition + 1 == historyLength ? 0 : writePosition + 1;

            // S' = S_d − a · S_low（输出时刻的 a）；a = 0 / 1 时直接取端点（a = 0 时低通的输出完全不参与）。
            const auto amount = monoAmountRamp.valueAt (i);
            auto monoSide = delayedSide;

            if (amount >= 1.0)
                monoSide = delayedSide - lowSide;
            else if (amount > 0.0)
                monoSide = delayedSide - amount * lowSide;

            // ③ 加宽：S' 过 2 kHz 高架。高架此刻逐位直通（增益 0 的系数、状态为 0）时直接取 S'：对有限样本与跑一遍
            //    滤波器数值相同（y = 1·S' + 0，状态保持 0；至多 −0 变 +0 的区别），而输入里的 ±Inf 也不会把它的状态弄成 NaN
            //    ——「三级都不作用」因此不会被一个坏样本打断到下一个网格点。
            const auto shelfWasIdle = shelfIsIdentity && sideShelfState.s1 == 0.0 && sideShelfState.s2 == 0.0;
            const auto wideSide = shelfWasIdle ? monoSide : processBiquad (monoSide, sideShelf, sideShelfState);

            const auto bypass = bypassRamp.valueAt (i);

            // 完全旁通，或延迟后的这个样本三级都不作用（没经过低切、a = 0、高架直通且无余振）：逐位取纯延迟线。后者不能交给
            // M/S 解码——(L + R)/2 + (L − R)/2 在 double 里不保证逐位还原 L；而且处理后 = 干路时任何旁通混合比例的结果都是干路。
            if (bypass >= 1.0 || (! delayedWasCut && amount <= 0.0 && shelfWasIdle))
            {
                left[i]  = delayedLeft;
                right[i] = delayedRight;
                continue;
            }

            // M/S 解码：L' = M_d + S''，R' = M_d − S''；旁通淡化中与同样延迟 D 的干路线性交叉淡化（延迟补偿，对齐）。
            auto wetLeft  = delayedMid + wideSide;
            auto wetRight = delayedMid - wideSide;

            if (bypass > 0.0)
            {
                wetLeft  += (static_cast<double> (delayedLeft)  - wetLeft)  * bypass;
                wetRight += (static_cast<double> (delayedRight) - wetRight) * bypass;
            }

            left[i]  = static_cast<float> (wetLeft);
            right[i] = static_cast<float> (wetRight);
        }

        cutStates[0][0] = leftFirst;
        cutStates[0][1] = leftSecond;
        cutStates[1][0] = rightFirst;
        cutStates[1][1] = rightSecond;
        shelfState = sideShelfState;

        delayPosition     = position;
        cutSamplesInDelay = flaggedInDelay;
        sideWritePosition = writePosition;
        lowpassBacklog    = backlog;

        // ④ 峰值守护：本段每个样本的 (干路, ①～③ 输出) 送进前瞻环，写回 G 个样本之前那个样本的守护后输出。
        for (int i = 0; i < numSamples; ++i)
        {
            const float dry[2] { dryLefts[static_cast<std::size_t> (i)], dryRights[static_cast<std::size_t> (i)] };
            const float wet[2] { left[i], right[i] };
            float out[2] {};
            peakGuard.process (dry, wet, out);
            left[i]  = out[0];
            right[i] = out[1];
        }
    }

    void ClayModule::processMono (float* samples, int numSamples) noexcept
    {
        // 单声道只做 ①（② ③ 只动 S，单声道没有 S），再延迟 D；用第 0 声道那一份低切状态与延迟线（midDelay 存 ① 选择之后的样本）。
        // 与立体声 L = R 的输入是同一串运算：那时 L、R 两路逐位相同，M = L、S = 0，解码后数值仍是 L（至多 −0 变 +0）。
        const auto cutFirst  = cutCoefficients[0];
        const auto cutSecond = cutCoefficients[1];
        auto first  = cutStates[0][0];
        auto second = cutStates[0][1];

        const auto delayLength = coreDelay;
        auto* delayedInputs   = dryDelay[0].data();
        auto* delayedValues   = midDelay.data();
        auto* delayedCutFlags = cutFlagDelay.data();
        auto position       = delayPosition;
        auto flaggedInDelay = cutSamplesInDelay;

        jassert (numSamples <= coefficientUpdateInterval);
        std::array<float, coefficientUpdateInterval> drySamples {};

        for (int i = 0; i < numSamples; ++i)
        {
            const auto input = samples[i];
            const auto dry = static_cast<double> (input);
            const auto cut = processBiquad (processBiquad (dry, cutFirst, first), cutSecond, second);
            const auto cutAmount = cutMixRamp.valueAt (i);

            auto selected = dry;

            if (cutAmount >= 1.0)
                selected = cut;
            else if (cutAmount > 0.0)
                selected = dry + (cut - dry) * cutAmount;

            const auto delayedInput  = delayedInputs[position];
            const auto delayedValue  = delayedValues[position];
            const auto delayedWasCut = delayedCutFlags[position] != 0;
            const auto cutNow        = cutAmount > 0.0;

            drySamples[static_cast<std::size_t> (i)] = delayedInput;

            delayedInputs[position]   = input;
            delayedValues[position]   = selected;
            delayedCutFlags[position] = static_cast<std::uint8_t> (cutNow ? 1 : 0);
            flaggedInDelay += (cutNow ? 1 : 0) - (delayedWasCut ? 1 : 0);
            position = position + 1 == delayLength ? 0 : position + 1;

            const auto bypass = bypassRamp.valueAt (i);

            if (bypass >= 1.0 || ! delayedWasCut)
            {
                samples[i] = delayedInput;
                continue;
            }

            auto wet = delayedValue;

            if (bypass > 0.0)
                wet += (static_cast<double> (delayedInput) - wet) * bypass;

            samples[i] = static_cast<float> (wet);
        }

        cutStates[0][0] = first;
        cutStates[0][1] = second;

        delayPosition     = position;
        cutSamplesInDelay = flaggedInDelay;

        // 单声道不跑低通（没有 S），但分频点切换的时序照立体声走：预热欠的历史样本按同样的速度记账，不然切换会一直停在预热里。
        if (lowpassWarming)
            lowpassBacklog = juce::jmax (0, lowpassBacklog - lowpassCatchUpRate * numSamples);

        // ④ 单声道守护（同立体声，一个声道）。
        for (int i = 0; i < numSamples; ++i)
        {
            const float dry[1] { drySamples[static_cast<std::size_t> (i)] };
            const float wet[1] { samples[i] };
            float out[1] {};
            peakGuard.process (dry, wet, out);
            samples[i] = out[0];
        }
    }


    //==============================================================================
    // ④ 峰值守护（ADR-014；算法、理由与保证见 ClayModule.h 文件头）。
    //
    // 时序（m = 刚送进来的样本号，都从 reset 起算）：
    //   E(k) 要干路 [k − W, k + W]：k = m − W 时刚好知道；P(k)、B(k)、g_req(k) 随之可算。
    //   h(j) = min g_req[j, j + A]：j = k − A = m − G 时刚好知道；r(j) = min(h(j), r(j − 1) + 释放步长)；
    //   g(j) = r[j − A, j] 的平均。输出样本 j 的 x + g·(y − x)。所以输出比输入晚 G = W + A。
    // 保证：r ≤ h（释放只让它更小），而 i ∈ [j − A, j] 时 h(i) 的窗 [i, i + A] 含 j → h(i) ≤ g_req(j)；平均之后 g(j) ≤ g_req(j)，
    //   |x + g·(y − x)| ≤ B(j)（{g : |x + g(y − x)| ≤ B} 是含 0 的区间，g_req 是它的右端）。
    // 逐样本、只依赖从 reset 起的样本号：输出与块长无关。滑动平均的和逐样本加减、按从 reset 起每 65536 样本的固定网格从环里
    //   精确重算；窗里全是 1 时直接给 1（g 精确为 1 → 输出逐位 = y）。
    int ClayModule::PeakGuard::windowFor (double fs) noexcept
    {
        return fs > 0.0 ? static_cast<int> (std::lround (peakGuardWindowSeconds * fs)) : 0;
    }

    int ClayModule::PeakGuard::attackFor (double fs) noexcept
    {
        return fs > 0.0 ? static_cast<int> (std::lround (peakGuardAttackSeconds * fs)) : 0;
    }

    void ClayModule::PeakGuard::MonotonicQueue::prepare (int windowLength)
    {
        capacity = juce::jmax (1, windowLength + 1);
        indices.assign (static_cast<std::size_t> (capacity), 0);
        values.assign (static_cast<std::size_t> (capacity), 0.0);
        clear();
    }

    void ClayModule::PeakGuard::MonotonicQueue::pushMax (std::int64_t index, double value) noexcept
    {
        // 队尾比新值小或相等的永远不会再是最大：弹掉。容量 = 窗长 + 1，调用方每推一个就 dropBefore，不会溢出。
        while (count > 0)
        {
            const auto back = (head + count - 1) % capacity;

            if (values[static_cast<std::size_t> (back)] > value)
                break;

            --count;
        }

        const auto slot = static_cast<std::size_t> ((head + count) % capacity);
        indices[slot] = index;
        values[slot] = value;
        ++count;
    }

    void ClayModule::PeakGuard::MonotonicQueue::pushMin (std::int64_t index, double value) noexcept
    {
        while (count > 0)
        {
            const auto back = (head + count - 1) % capacity;

            if (values[static_cast<std::size_t> (back)] < value)
                break;

            --count;
        }

        const auto slot = static_cast<std::size_t> ((head + count) % capacity);
        indices[slot] = index;
        values[slot] = value;
        ++count;
    }

    void ClayModule::PeakGuard::MonotonicQueue::dropBefore (std::int64_t oldest) noexcept
    {
        // 至少留一个（刚推进去的那个样本号一定 ≥ oldest）。
        while (count > 1 && indices[static_cast<std::size_t> (head)] < oldest)
        {
            head = head + 1 == capacity ? 0 : head + 1;
            --count;
        }
    }

    void ClayModule::PeakGuard::prepare (double fs, int channelCount)
    {
        channels  = juce::jlimit (1, 2, channelCount);
        window    = windowFor (fs);
        attack    = attackFor (fs);
        lookahead = window + attack;

        envelopeRetain = fs > 0.0 ? std::exp (-1.0 / (peakGuardEnvelopeSeconds * fs)) : 0.0;
        releaseStep    = fs > 0.0 ? 1.0 / (peakGuardReleaseSeconds * fs) : 1.0;

        ringLength = lookahead + 1;

        for (auto& ring : dryRing)
            ring.assign (static_cast<std::size_t> (ringLength), 0.0f);

        for (auto& ring : wetRing)
            ring.assign (static_cast<std::size_t> (ringLength), 0.0f);

        processedFlags.assign (static_cast<std::size_t> (ringLength), static_cast<std::uint8_t> (0));
        costBlockLength = fs > 0.0 ? juce::jmax (1, static_cast<int> (std::lround (0.01 * fs))) : 1;
        peakQueue.prepare (2 * window + 1);
        requiredQueue.prepare (attack + 1);
        releaseRing.assign (static_cast<std::size_t> (attack + 1), 1.0);

        reset();
    }

    void ClayModule::PeakGuard::reset() noexcept
    {
        // 「此前全是 0」：环里干路与处理后都是 0（逐位相同 → 不算处理过），g 全是 1、P = 0。
        for (auto& ring : dryRing)
            std::fill (ring.begin(), ring.end(), 0.0f);

        for (auto& ring : wetRing)
            std::fill (ring.begin(), ring.end(), 0.0f);

        std::fill (processedFlags.begin(), processedFlags.end(), static_cast<std::uint8_t> (0));
        std::fill (releaseRing.begin(), releaseRing.end(), 1.0);

        writeIndex = 0;
        processedInRing = 0;
        peakQueue.clear();
        requiredQueue.clear();
        releaseIndex = 0;
        releaseSum = static_cast<double> (releaseRing.size());
        releaseBelowOne = 0;
        released = 1.0;
        envelope = 0.0;
        sampleCount = 0;
        stats = PeakGuardStats {};
        costBlockPosition = 0;
        costBlockMaxRatio = 0.0;
        pressureSumDb = 0.0;
        pressureBlocks = 0;
    }

    void ClayModule::PeakGuard::process (const float* dry, const float* wet, float* out) noexcept
    {
        const auto m = sampleCount;
        const auto slotOf = [this] (int samplesBack) noexcept
        {
            const auto index = writeIndex - samplesBack;
            return static_cast<std::size_t> (index < 0 ? index + ringLength : index);
        };

        // 1. 写环（覆盖的是 m − G − 1，上一个样本已经输出过它）；记「处理后 ≠ 干路」的个数。
        const auto newest = static_cast<std::size_t> (writeIndex);
        auto processed = false;
        auto magnitude = 0.0;

        for (int c = 0; c < channels; ++c)
        {
            const auto channel = static_cast<std::size_t> (c);
            dryRing[channel][newest] = dry[c];
            wetRing[channel][newest] = wet[c];
            processed = processed || ! sameBits (dry[c], wet[c]);

            // 非有限的干路不参与局部峰值（链的 NaN 保护会整体 reset）；E 只要有限的量级。
            const auto value = static_cast<double> (dry[c]);

            if (std::isfinite (value))
                magnitude = juce::jmax (magnitude, std::abs (value));
        }

        processedInRing += (processed ? 1 : 0) - (processedFlags[newest] != 0 ? 1 : 0);
        processedFlags[newest] = static_cast<std::uint8_t> (processed ? 1 : 0);

        // 2. 局部峰值 E(k)，k = m − W：干路 [k − W, k + W] = [m − 2W, m] 的最大。
        peakQueue.pushMax (m, magnitude);
        peakQueue.dropBefore (m - 2 * window);
        const auto localPeak = peakQueue.front();

        // 3. 包络 P(k) 与界 B(k)，需求 g_req(k)。
        envelope = juce::jmax (localPeak, envelope * envelopeRetain);

        if (envelope < peakGuardEnvelopeSnap)
            envelope = 0.0;

        const auto bound = juce::jmax (localPeak, juce::jmax (peakGuardRelativeGain * envelope, peakGuardFloor));
        const auto k = slotOf (window);
        auto required = 1.0;

        for (int c = 0; c < channels; ++c)
        {
            const auto channel = static_cast<std::size_t> (c);
            const auto x = static_cast<double> (dryRing[channel][k]);
            const auto y = static_cast<double> (wetRing[channel][k]);

            if (! std::isfinite (x) || ! std::isfinite (y) || ! (std::abs (y) > bound))
                continue;

            // |x| ≤ E ≤ B 而 |y| > B：越界在 y − x 的方向那一侧，g = (B − sgn(d)·x) / |d| ∈ [0, 1)。
            const auto d = y - x;
            const auto limit = (bound - (d > 0.0 ? x : -x)) / std::abs (d);
            required = juce::jmin (required, juce::jlimit (0.0, 1.0, limit));
            costBlockMaxRatio = juce::jmax (costBlockMaxRatio, std::abs (y) / bound);
        }

        // 代价格（压力反馈）：k 的时间轴上每 costBlockLength 个样本收一格。
        if (++costBlockPosition >= costBlockLength)
        {
            pressureSumDb += costBlockMaxRatio > 1.0 ? 20.0 * std::log10 (costBlockMaxRatio) : 0.0;
            ++pressureBlocks;
            costBlockMaxRatio = 0.0;
            costBlockPosition = 0;
        }

        // 4. 前瞻最小 h(j) = min g_req[j, j + A]，j = k − A = m − G。
        const auto kIndex = m - window;
        requiredQueue.pushMin (kIndex, required);
        requiredQueue.dropBefore (kIndex - attack);
        const auto held = requiredQueue.front();

        // 5. 线性释放 r(j) 与长 A + 1 的滑动平均 g(j)。
        const auto releasedNow = juce::jmin (held, released + releaseStep);
        released = releasedNow;

        const auto releaseSlot = static_cast<std::size_t> (releaseIndex);
        const auto leaving = releaseRing[releaseSlot];
        releaseRing[releaseSlot] = releasedNow;
        releaseIndex = releaseIndex + 1 == static_cast<int> (releaseRing.size()) ? 0 : releaseIndex + 1;
        releaseBelowOne += (releasedNow < 1.0 ? 1 : 0) - (leaving < 1.0 ? 1 : 0);
        releaseSum += releasedNow - leaving;

        if (releaseBelowOne == 0)
        {
            releaseSum = static_cast<double> (releaseRing.size());   // 全是 1：精确
        }
        else if (m % peakGuardResyncInterval == 0)
        {
            // 固定网格上按环的固定下标顺序精确重算（与块长无关）。
            auto sum = 0.0;

            for (const auto value : releaseRing)
                sum += value;

            releaseSum = sum;
        }

        const auto gain = releaseBelowOne == 0 ? 1.0 : juce::jlimit (0.0, 1.0, releaseSum / static_cast<double> (releaseRing.size()));

        // 6. 输出样本 j = m − G：g 精确为 1 → 逐位 = y；否则 x + g·(y − x) 写成 y + (1 − g)·(x − y)（y 与 x 逐位相同时仍逐位 = y）。
        const auto j = slotOf (lookahead);

        for (int c = 0; c < channels; ++c)
        {
            const auto channel = static_cast<std::size_t> (c);
            const auto y = wetRing[channel][j];

            if (gain >= 1.0)
            {
                out[c] = y;
                continue;
            }

            const auto yd = static_cast<double> (y);
            const auto xd = static_cast<double> (dryRing[channel][j]);
            out[c] = static_cast<float> (yd + (xd - yd) * (1.0 - gain));
        }

        if (gain < 1.0)
        {
            ++stats.guardedSamples;
            stats.minGain = juce::jmin (stats.minGain, static_cast<float> (gain));
        }

        writeIndex = writeIndex + 1 == ringLength ? 0 : writeIndex + 1;
        ++sampleCount;
    }
}
