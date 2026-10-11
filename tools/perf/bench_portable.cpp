/*
  ==============================================================================

    bench_portable.cpp
    celadon_bench_portable：可移植的 CPU 基准（T-096f；计划 §13 Q2 / Q10 / Q12 / Q16）。

    只用 JUCE 与 Source/**，没有 Apple 框架。为 DGX（aarch64、GCC 13、无 LTO）上的「同机不退步」基线而写：只比同一台机器上
    基线二进制与当前二进制的差。Mac 也能编，但不是目标——Mac 的绝对数仍由 Max 用 tools/perf/bench.mm（scripts/perf-local.sh）跑。
    不进 CI；根 CMakeLists.txt 里是 EXCLUDE_FROM_ALL 目标；DGX 入口 scripts/perf-dgx.sh；对比 tools/perf/compare_perf.py；
    方法与口径见 docs/validation/2026-10-11-T096f-cpu-baseline/README.md。

    两层（--layer）：
      processor = 真 CeladonAudioProcessor::processBlock：参数读取、输入 NaN 扫描、频谱仪接头、LookaheadHold、整链（含实时 GLAZE）。
                  只在能编界面库的构建里有（Linux 链 celadon_juce_gui，照 celadon_ui_snapshot；CELADON_BENCH_PROCESSOR_LAYER = 1）。
      chain     = 直接调 CeladonChain：setHostInfo + setParams 记一段、process 记一段，两段之和是整块。不再手抄链逻辑
                  （vault 踩坑-性能基准手抄了链里的逻辑）。分级（Balance / Clay / Color / Limit）计时探针排在 T-095 之后（子项 f2），本轮不做。

    每块记墙钟（steady_clock）与本线程 CPU 时间（CLOCK_THREAD_CPUTIME_ID）；每遍记本线程的自愿 / 非自愿切换（getrusage RUSAGE_THREAD，
    只 Linux 有）。百分比一律 = 耗时 ÷ 块时长 × 100。主判 structWorst（口径照 tools/perf/bench.mm:105-139：按「块在 Balance 分析 hop
    里的相位」分组、各组取中位数再取最大）；另报（只报不判）：按控制层网格对齐分组的 structWorst（1 s 决策网格、实时画像 5 ms 分片网格）、
    逐块线程 CPU 时间、非自愿切换、p99.9、maxAllPasses。

    场景（--list 列全）：default / manual / auto100 / house-club-35 / house-club-100 / general-standard-35 / general-standard-100，
    以及 GLAZE 0 孪生（default-g0 / house-club-g0 / general-standard-g0）。点名 GLAZE > 0 的场景时自动补上它的孪生，并多出一行
    row_kind = estimate：「分析器 + 控制层」≈ GLAZE > 0 整链 − GLAZE 0 整链——这是估计（分析器在 GLAZE 0 也在跑，差里另含
    模块因实际参数变化的增减与切片开销，见 README）。

    素材：固定种子合成的 House 结构曲（124 BPM，前奏 / drop / breakdown / drop 各 8 小节，一圈约 61.9 s，循环喂），峰值 −1 dBFS。
    宿主时间线：在播、位置 = 从 prepare 起的样本数、124 BPM、4/4、小节起点——两层看到同一份宿主信息（House 档按小节记账要拍点）。

    用法：
      celadon_bench_portable [--scenario 名字,名字|all] [--sr 44100,48000,96000,192000] [--bs 32,64,512]
                             [--layer processor,chain|all] [--warm 秒] [--pass 秒（0 = 素材一整圈）] [--passes N] [--out 文件|-]
      celadon_bench_portable --list | --selftest | --help
    退出码：0 正常；1 基础设施错（输出写不了、参数表缺 ID、输出出现非有限值）；2 命令行用法错。selftest 有失败也返回 1。

  ==============================================================================
*/

#if ! defined (CELADON_BENCH_PROCESSOR_LAYER)
 #define CELADON_BENCH_PROCESSOR_LAYER 0
#endif

#if CELADON_BENCH_PROCESSOR_LAYER
 #include <JuceHeader.h>
 #include "../../Source/PluginProcessor.h"
#else
 #include <juce_audio_basics/juce_audio_basics.h>
 #include <juce_core/juce_core.h>
 #include <juce_dsp/juce_dsp.h>
#endif

#include "../../Source/ParameterDefaults.h"
#include "../../Source/ParameterIds.h"
#include "../../Source/dsp/BalanceModule.h"
#include "../../Source/dsp/CeladonChain.h"
#include "../../Source/dsp/ChainParams.h"
#include "../../Source/dsp/GlazeTypes.h"
#include "../../Source/dsp/LinearPhaseLowpass.h"
#include "../../Source/dsp/LiveProfile.h"

#include <sys/resource.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    //==============================================================================
    // 可调常量（集中一处）
    constexpr std::int64_t materialSeed = 20261011;              // 固定种子：基线与当前两版二进制喂同一份素材
    constexpr double materialBpm = 124.0;                        // 素材速度；宿主时间线也按它报拍点
    constexpr int beatsPerBar = 4;
    constexpr int materialBars = 32;                             // 32 小节 ≈ 61.9 s（计划要求循环 ≥ 60 s）
    constexpr int barsPerSection = 8;                            // 前奏 / drop / breakdown / drop 各 8 小节
    constexpr double materialPeakLinear = 0.89125093813374556;   // 素材峰值归一到 −1 dBFS（母带前的混音电平）
    constexpr double defaultWarmSeconds = 20.0;                  // 空跑 ≥ 20 s，让实时画像进稳态（计划 §13）
    constexpr double minimumWarmSeconds = 20.0;                  // 低于它照跑，但在 stderr 提示
    constexpr int defaultPasses = 3;                             // 每遍默认 = 素材一整圈，各遍内容相同，取多遍最好才有意义
    constexpr int minimumBlocksPerPass = 8;                      // 同 bench.mm
    constexpr double p999Quantile = 0.999;                       // 同 bench.mm 的取法：排序后第 floor(0.999 n) 个
    constexpr double decisionGridSeconds = 1.0;                  // GlazeController.cpp:1253 grid = llround (sampleRate)
    const char* const defaultSampleRates = "44100,48000,96000,192000";
    const char* const defaultBlockSizes = "32,64,512";

    // 与 tools/render/RenderMain.cpp 的 genreIds / deliveryIds 同一写法（下标 = glaze::Genre / glaze::Delivery）。
    constexpr std::array<const char*, 4> genreIds { { "general", "deep", "tech", "melodic" } };
    constexpr std::array<const char*, 3> deliveryIds { { "standard", "streaming", "club" } };

    // na_reason 列的代码（README 有中文解释）。Q12：算不出的格子写空值并在这一列写原因，绝不填 0。
    const char* const naSegmentsChainOnly = "segments_chain_only";               // 两段拆分只在链层有
    const char* const naThreadCpuUnavailable = "thread_cpu_unavailable";         // CLOCK_THREAD_CPUTIME_ID 读不到
    const char* const naRusageThreadUnavailable = "rusage_thread_unavailable";   // 本系统没有 RUSAGE_THREAD（如 macOS）
    const char* const naEstimateNotDifferenced = "estimate_not_differenced";     // 估计行：maxAllPasses / 计数 / 状态不做差

    struct InfrastructureError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    constexpr double notAvailable() noexcept { return std::numeric_limits<double>::quiet_NaN(); }

    //==============================================================================
    // 纯逻辑：统计与分组（不碰时间、文件，--selftest 覆盖）

    // 中位数：与 bench.mm 同一取法（v[n / 2]，偶数个取上中位数）。空 → NaN。
    double medianOf (std::vector<double> values)
    {
        if (values.empty())
            return notAvailable();
        const auto middle = values.begin() + (std::ptrdiff_t) (values.size() / 2);
        std::nth_element (values.begin(), middle, values.end());
        return *middle;
    }

    // 一遍的 p99.9：排序后第 min(n − 1, floor(0.999 n)) 个（同 bench.mm summarise）。
    double quantileOfSorted (const std::vector<double>& sorted, double quantile)
    {
        if (sorted.empty())
            return notAvailable();
        const auto index = std::min (sorted.size() - 1, (std::size_t) std::floor (quantile * (double) sorted.size()));
        return sorted[index];
    }

    // 多遍归约（同 bench.mm runGrid）：均值 / 最坏 / p99.9 取各遍里最好的一遍，maxAll 取所有块的最大。
    struct PassSummary
    {
        double bestMean = notAvailable();
        double bestMax = notAvailable();
        double bestP999 = notAvailable();
        double maxAll = notAvailable();
    };

    PassSummary summarisePasses (const std::vector<double>& values, int blocksPerPass, int passes)
    {
        PassSummary summary;
        if (blocksPerPass <= 0 || passes <= 0 || values.size() < (std::size_t) blocksPerPass * (std::size_t) passes)
            return summary;

        std::vector<double> sorted;
        for (int pass = 0; pass < passes; ++pass)
        {
            const auto first = values.begin() + (std::ptrdiff_t) pass * blocksPerPass;
            const auto last = first + blocksPerPass;
            const auto mean = std::accumulate (first, last, 0.0) / (double) blocksPerPass;
            const auto maximum = *std::max_element (first, last);
            sorted.assign (first, last);
            std::sort (sorted.begin(), sorted.end());
            const auto p999 = quantileOfSorted (sorted, p999Quantile);

            const auto firstPass = pass == 0;
            summary.bestMean = firstPass ? mean : std::min (summary.bestMean, mean);
            summary.bestMax = firstPass ? maximum : std::min (summary.bestMax, maximum);
            summary.bestP999 = firstPass ? p999 : std::min (summary.bestP999, p999);
            summary.maxAll = firstPass ? maximum : std::max (summary.maxAll, maximum);
        }
        return summary;
    }

    // 结构性分组。两种：
    //   blockStartPhase = 块起点在周期里的相位（bench.mm 的 hop / 65536 分组，原样保留以便与 Mac 读数同口径）；
    //   gridCrossing    = 按「块里有没有网格点、离上一个网格点多远」分组：第 0 组恰好是含网格点的块（事件在这一块里触发），
    //                     周期不是块长整数倍（44.1 kHz 的 1 s = 44100）时也只落在第 0 组，不会散到相邻两组。
    //   条件推导：GlazeController 处理完一片后 timelineSamples ≥ k·G 就决策，即块 [pos, pos + bs) 含样本 k·G − 1
    //   ⇔ (pos, pos + bs] 里有 G 的倍数 ⇔ (pos + bs) mod G < bs。组号 = ((pos + bs) mod G) / bs。
    struct Grouping
    {
        enum class Kind { blockStartPhase, gridCrossing };
        Kind kind = Kind::blockStartPhase;
        std::int64_t periodSamples = 1;
    };

    int groupCount (const Grouping& grouping, int blockSize) noexcept
    {
        const auto period = std::max<std::int64_t> (1, grouping.periodSamples);
        if (grouping.kind == Grouping::Kind::blockStartPhase)
            return blockSize >= period ? 1 : (int) (period / blockSize);
        return (int) ((period + blockSize - 1) / blockSize);
    }

    int groupIndex (const Grouping& grouping, std::int64_t position, int blockSize) noexcept
    {
        const auto period = std::max<std::int64_t> (1, grouping.periodSamples);
        if (grouping.kind == Grouping::Kind::blockStartPhase)
            return (int) (((position % period) / blockSize) % groupCount (grouping, blockSize));
        return (int) (((position + blockSize) % period) / blockSize);
    }

    // values[i] 是从 firstPosition 起第 i 块（块与块首尾相接）的耗时。各组取中位数再取最大；没有数据 → NaN。
    double structuralWorst (const std::vector<double>& values, std::int64_t firstPosition, int blockSize, const Grouping& grouping)
    {
        std::vector<std::vector<double>> byGroup ((std::size_t) groupCount (grouping, blockSize));
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            const auto position = firstPosition + (std::int64_t) i * blockSize;
            byGroup[(std::size_t) groupIndex (grouping, position, blockSize)].push_back (values[i]);
        }

        auto worst = notAvailable();
        for (auto& group : byGroup)
        {
            if (group.empty())
                continue;
            const auto median = medianOf (std::move (group));
            worst = std::isnan (worst) ? median : std::max (worst, median);
        }
        return worst;
    }

    double percentOfBlock (double nanoseconds, double blockNanoseconds) noexcept
    {
        return std::isfinite (nanoseconds) && blockNanoseconds > 0.0 ? 100.0 * nanoseconds / blockNanoseconds : notAvailable();
    }

    double differenceOrNotAvailable (double a, double b) noexcept
    {
        return std::isfinite (a) && std::isfinite (b) ? a - b : notAvailable();
    }

    //==============================================================================
    // 素材：固定种子的 House 结构曲（纯函数：同一采样率两次生成逐位相同）

    enum class Section { intro = 0, drop = 1, breakdown = 2, drop2 = 3 };

    struct SectionMix
    {
        double gain;   // 整段增益（drop 最响，前奏 / breakdown 低约 6 ～ 15 dB，给实时画像分出主段与非主段）
        double pad;    // 铺底和弦电平
        double pink;   // 粉噪底电平
    };

    constexpr std::array<SectionMix, 4> sectionMixes { { { 0.6, 0.06, 0.6 },     // 前奏：开镲（第 3 小节起）、底鼓（第 5 小节起）
                                                         { 1.0, 0.04, 1.0 },     // drop：全编制
                                                         { 0.6, 0.16, 0.5 },     // breakdown：无鼓无贝斯，铺底更响，后 4 小节噪声上扬
                                                         { 1.0, 0.04, 1.0 } } }; // drop 2

    constexpr std::array<double, 4> bassRootsHz { { 55.0, 43.65, 65.41, 49.0 } };   // A1 F1 C2 G1（Am – F – C – G 每小节一换）
    constexpr std::array<std::array<double, 3>, 4> padChordsHz { { { { 220.0, 261.63, 329.63 } },
                                                                    { { 174.61, 220.0, 261.63 } },
                                                                    { { 196.0, 261.63, 329.63 } },
                                                                    { { 196.0, 246.94, 293.66 } } } };

    double secondsPerBeat() noexcept { return 60.0 / materialBpm; }

    double materialLoopSeconds() noexcept { return (double) (materialBars * beatsPerBar) * secondsPerBeat(); }

    std::int64_t materialLoopSamples (double sampleRate) noexcept
    {
        return (std::int64_t) std::llround (materialLoopSeconds() * sampleRate);
    }

    Section sectionOfBar (int bar) noexcept
    {
        return static_cast<Section> (juce::jlimit (0, 3, bar / barsPerSection));
    }

    // 半拍以后的时间（秒），用于反拍开镲；返回负数 = 还在前半拍。
    double secondsSinceOffbeat (double beatFraction) noexcept
    {
        return (beatFraction - 0.5) * secondsPerBeat();
    }

    juce::AudioBuffer<float> makeMaterial (double sampleRate)
    {
        const auto total = (int) materialLoopSamples (sampleRate);
        std::array<std::vector<double>, 2> mix { { std::vector<double> ((std::size_t) total), std::vector<double> ((std::size_t) total) } };

        juce::Random rng (materialSeed);
        const auto twoPi = juce::MathConstants<double>::twoPi;
        const auto beatSeconds = secondsPerBeat();

        double pinkState[2][7] = {};
        float previousWhite[2] = {};
        double bassPhase = 0.0;
        double padPhase[3][2] = {};

        // 粉噪：同 bench.mm makeSignal 的 Paul Kellet 滤波。
        auto pink = [&pinkState] (int channel, float whiteSample) noexcept
        {
            auto* s = pinkState[channel];
            const auto w = (double) whiteSample;
            s[0] = 0.99886 * s[0] + w * 0.0555179;
            s[1] = 0.99332 * s[1] + w * 0.0750759;
            s[2] = 0.96900 * s[2] + w * 0.1538520;
            s[3] = 0.86650 * s[3] + w * 0.3104856;
            s[4] = 0.55000 * s[4] + w * 0.5329522;
            s[5] = -0.7616 * s[5] - w * 0.0168980;
            const auto out = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + w * 0.5362;
            s[6] = w * 0.115926;
            return out * 0.11;
        };

        for (int n = 0; n < total; ++n)
        {
            const auto t = (double) n / sampleRate;
            const auto beats = t / beatSeconds;
            const auto wholeBeats = std::floor (beats);
            const auto beatFraction = beats - wholeBeats;
            const auto sinceBeat = beatFraction * beatSeconds;
            const auto beatIndex = (int) wholeBeats;
            const auto bar = juce::jmin (materialBars - 1, beatIndex / beatsPerBar);
            const auto beatInBar = beatIndex % beatsPerBar;
            const auto section = sectionOfBar (bar);
            const auto& sectionMix = sectionMixes[(std::size_t) section];
            const auto chord = (std::size_t) (bar % 4);

            const auto fullBand = section == Section::drop || section == Section::drop2;
            const auto kickOn = fullBand || (section == Section::intro && bar >= 4);
            const auto openHatOn = fullBand || (section == Section::intro && bar >= 2);
            const auto riserOn = section == Section::breakdown && bar >= 2 * barsPerSection + 4;

            // 每个样本固定取 3 个随机数（与分段无关），各段的噪声互不牵连。
            const auto common = rng.nextFloat() * 2.0f - 1.0f;
            float white[2] = {};
            for (auto& w : white)
                w = 0.7f * common + 0.3f * (rng.nextFloat() * 2.0f - 1.0f);

            // 底鼓：每拍一下，45 Hz 起 + 90 Hz 下滑（τ 30 ms），衰减 τ 160 ms；单声道。
            double kick = 0.0;
            if (kickOn)
            {
                const auto phase = twoPi * (45.0 * sinceBeat + 90.0 * 0.03 * (1.0 - std::exp (-sinceBeat / 0.03)));
                kick = 0.9 * std::sin (phase) * std::exp (-sinceBeat / 0.16);
            }

            // 贝斯：滚动低音，跟底鼓做旁链式下压；单声道。
            double bass = 0.0;
            if (fullBand)
            {
                bassPhase += twoPi * bassRootsHz[chord] / sampleRate;
                if (bassPhase >= twoPi)
                    bassPhase -= twoPi;
                const auto pump = 1.0 - 0.85 * std::exp (-sinceBeat / 0.09);
                bass = 0.42 * std::tanh (2.2 * std::sin (bassPhase)) * pump;
            }

            const auto clapEnvelope = fullBand && (beatInBar == 1 || beatInBar == 3) ? 0.35 * std::exp (-sinceBeat / 0.045) : 0.0;
            const auto offbeat = secondsSinceOffbeat (beatFraction);
            const auto openHatEnvelope = openHatOn && offbeat >= 0.0 ? 0.10 * std::exp (-offbeat / 0.05) : 0.0;
            const auto sixteenth = beats * 4.0 - std::floor (beats * 4.0);
            const auto closedHatEnvelope = fullBand ? 0.05 * std::exp (-sixteenth * beatSeconds * 0.25 / 0.012) : 0.0;
            const auto riserProgress = riserOn ? (beats - (double) ((2 * barsPerSection + 4) * beatsPerBar)) / (double) (4 * beatsPerBar) : 0.0;
            const auto padLfo = 0.8 + 0.2 * std::sin (twoPi * 0.25 * t);

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto highPassed = (double) (white[ch] - previousWhite[ch]) * 0.5;   // 一阶差分 = 亮的镲 / 噪声
                previousWhite[ch] = white[ch];

                double pad = 0.0;
                for (std::size_t voice = 0; voice < 3; ++voice)
                {
                    auto& phase = padPhase[voice][ch];
                    phase += twoPi * (padChordsHz[chord][voice] + (ch == 1 ? 0.4 : 0.0)) / sampleRate;   // 右声道微失谐 = 宽度
                    if (phase >= twoPi)
                        phase -= twoPi;
                    pad += std::sin (phase);
                }
                pad *= sectionMix.pad * padLfo / 3.0;

                const auto clap = clapEnvelope * (double) white[ch];
                const auto hats = (openHatEnvelope + closedHatEnvelope) * highPassed;
                const auto riser = 0.15 * riserProgress * riserProgress * highPassed;
                const auto bed = sectionMix.pink * pink (ch, white[ch]);

                mix[(std::size_t) ch][(std::size_t) n] = sectionMix.gain * (kick + bass + clap + hats + pad + riser + bed);
            }
        }

        double peak = 0.0;
        for (const auto& channel : mix)
            for (const auto v : channel)
                peak = std::max (peak, std::abs (v));
        const auto scale = peak > 0.0 ? materialPeakLinear / peak : 0.0;

        juce::AudioBuffer<float> material (2, total);
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < total; ++n)
                material.setSample (ch, n, (float) (mix[(std::size_t) ch][(std::size_t) n] * scale));
        return material;
    }

    // 循环喂素材（同 bench.mm Feeder）。
    struct Feeder
    {
        const juce::AudioBuffer<float>& source;
        int position = 0;

        void fill (juce::AudioBuffer<float>& destination)
        {
            const auto wanted = destination.getNumSamples();
            const auto total = source.getNumSamples();
            for (int done = 0; done < wanted;)
            {
                const auto chunk = juce::jmin (wanted - done, total - position);
                for (int ch = 0; ch < destination.getNumChannels(); ++ch)
                    destination.copyFrom (ch, done, source, ch, position, chunk);
                done += chunk;
                position += chunk;
                if (position >= total)
                    position = 0;
            }
        }
    };

    //==============================================================================
    // 宿主时间线：在播、位置 = 从 prepare 起的样本数、素材速度、4/4。

    struct HostTimeline
    {
        double sampleRate = 48000.0;

        double samplesPerQuarter() const noexcept { return secondsPerBeat() * sampleRate; }
        double ppqAt (std::int64_t position) const noexcept { return (double) position / samplesPerQuarter(); }
        double barStartPpqAt (std::int64_t position) const noexcept
        {
            return std::floor (ppqAt (position) / (double) beatsPerBar) * (double) beatsPerBar;
        }

        // 与 CeladonAudioProcessor::readHostInfo 读同一份播放头时得到的 HostInfo 相同（宿主给了位置就报 hasLoop、循环关）。
        celadon::glaze::HostInfo hostInfoAt (std::int64_t position) const noexcept
        {
            celadon::glaze::HostInfo info;
            info.offline = false;
            info.hasPlayState = true;
            info.isPlaying = true;
            info.hasPosition = true;
            info.positionSamples = position;
            info.hasLoop = true;
            info.looping = false;
            info.hasTempo = true;
            info.bpm = materialBpm;
            info.hasPpq = true;
            info.ppqPosition = ppqAt (position);
            info.hasBarStart = true;
            info.ppqLastBarStart = barStartPpqAt (position);
            info.hasTimeSignature = true;
            info.timeSignatureNumerator = beatsPerBar;
            info.timeSignatureDenominator = 4;
            return info;
        }
    };

    //==============================================================================
    // 场景（两层共用一份定义）

    struct Scenario
    {
        const char* name;
        float glazePercent;
        float limitGainDb;
        celadon::glaze::Genre genre;
        celadon::glaze::Delivery delivery;
        const char* twin;   // GLAZE 0 孪生场景名（只差 GLAZE）；自身 GLAZE 0 时为 nullptr
    };

    namespace defaults = celadon::param::defaults;
    using celadon::glaze::Delivery;
    using celadon::glaze::Genre;
    constexpr auto factoryGenre = celadon::glaze::genreFromIndex (defaults::glazeGenreIndex);
    constexpr auto factoryDelivery = celadon::glaze::deliveryFromIndex (defaults::glazeDeliveryIndex);

    // default 跟出厂默认走（ParameterDefaults.h 改了它就跟着变）；general-standard-* 把通用 + 标准 + GLAZE 钉死——今天两者按值相同，
    // 出厂默认以后变了也还有一组固定参照。
    const std::array<Scenario, 10> scenarioTable { {
        { "default",              defaults::glazeAmount, defaults::limitGainDb, factoryGenre,     factoryDelivery,    "default-g0" },
        { "default-g0",           0.0f,                  defaults::limitGainDb, factoryGenre,     factoryDelivery,    nullptr },
        { "manual",               0.0f,                  9.0f,                  factoryGenre,     factoryDelivery,    nullptr },
        { "auto100",              100.0f,                defaults::limitGainDb, factoryGenre,     factoryDelivery,    "default-g0" },
        { "house-club-35",        35.0f,                 defaults::limitGainDb, Genre::deepHouse, Delivery::club,     "house-club-g0" },
        { "house-club-100",       100.0f,                defaults::limitGainDb, Genre::deepHouse, Delivery::club,     "house-club-g0" },
        { "house-club-g0",        0.0f,                  defaults::limitGainDb, Genre::deepHouse, Delivery::club,     nullptr },
        { "general-standard-35",  35.0f,                 defaults::limitGainDb, Genre::general,   Delivery::standard, "general-standard-g0" },
        { "general-standard-100", 100.0f,                defaults::limitGainDb, Genre::general,   Delivery::standard, "general-standard-g0" },
        { "general-standard-g0",  0.0f,                  defaults::limitGainDb, Genre::general,   Delivery::standard, nullptr },
    } };

    const Scenario* findScenario (const std::string& name) noexcept
    {
        for (const auto& scenario : scenarioTable)
            if (name == scenario.name)
                return &scenario;
        return nullptr;
    }

    // 用户点名的场景按原顺序，GLAZE > 0 的场景后面紧跟它的孪生（已在列里就不重复）——孪生在时间上挨着测，漂移最小。
    std::vector<const Scenario*> withTwins (const std::vector<const Scenario*>& requested)
    {
        std::vector<const Scenario*> resolved;
        auto addOnce = [&resolved] (const Scenario* scenario)
        {
            if (scenario != nullptr && std::find (resolved.begin(), resolved.end(), scenario) == resolved.end())
                resolved.push_back (scenario);
        };
        for (const auto* scenario : requested)
        {
            addOnce (scenario);
            if (scenario->twin != nullptr)
                addOnce (findScenario (scenario->twin));
        }
        return resolved;
    }

    celadon::ChainParams chainParamsFor (const Scenario& scenario)
    {
        auto params = defaults::factoryChainParams();
        params.glazePercent = scenario.glazePercent;
        params.limit.gainDb = scenario.limitGainDb;
        params.glazeGenre = static_cast<int> (scenario.genre);
        params.glazeDelivery = static_cast<int> (scenario.delivery);
        return params;
    }

    //==============================================================================
    // 计时与线程统计（副作用，只在测量里用）

    using Clock = std::chrono::steady_clock;

    double nanosecondsBetween (Clock::time_point from, Clock::time_point to) noexcept
    {
        return std::chrono::duration<double, std::nano> (to - from).count();
    }

    double threadCpuNanoseconds() noexcept
    {
        timespec now {};
        if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &now) != 0)
            return notAvailable();
        return (double) now.tv_sec * 1.0e9 + (double) now.tv_nsec;
    }

    struct ThreadSwitches
    {
        bool valid = false;
        long long voluntary = 0;
        long long involuntary = 0;
    };

    ThreadSwitches readThreadSwitches() noexcept
    {
       #if defined (RUSAGE_THREAD)
        rusage usage {};
        if (getrusage (RUSAGE_THREAD, &usage) == 0)
            return { true, (long long) usage.ru_nvcsw, (long long) usage.ru_nivcsw };
       #endif
        return {};
    }

    //==============================================================================
    // 两层

    enum class Layer { processor, chain };

    const char* layerName (Layer layer) noexcept
    {
        return layer == Layer::processor ? "processor" : "chain";
    }

    constexpr bool processorLayerAvailable() noexcept { return CELADON_BENCH_PROCESSOR_LAYER != 0; }

    // 一块的耗时（纳秒）。setParams / process 两段只在链层有；处理器层为 NaN。
    struct BlockTiming
    {
        double wall;
        double setParams;
        double process;
    };

    class ChainLayer
    {
    public:
        ChainLayer (const Scenario& scenario, double rate, int block)
            : params (chainParamsFor (scenario)), genre (scenario.genre), timeline { rate }, sampleRate (rate), blockSize (block)
        {
        }

        static constexpr bool hasSegments = true;

        double prepare()
        {
            chain = std::make_unique<celadon::CeladonChain>();
            const auto start = Clock::now();
            // 顺序照 CeladonAudioProcessor::prepareToPlay：先交参数再 prepare，最后把曲风对应的主段模型交给实时画像。
            chain->setParams (params, false);
            chain->prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
            chain->syncGlazeLiveModel (genre);
            return 1.0e-6 * nanosecondsBetween (start, Clock::now());
        }

        BlockTiming process (juce::AudioBuffer<float>& buffer, std::int64_t position)
        {
            const auto start = Clock::now();
            chain->setHostInfo (timeline.hostInfoAt (position));
            chain->setParams (params, false);
            const auto paramsDone = Clock::now();
            if (chain->process (buffer))
                ++nonFinite;
            const auto end = Clock::now();
            return { nanosecondsBetween (start, end), nanosecondsBetween (start, paramsDone), nanosecondsBetween (paramsDone, end) };
        }

        int latency() const { return chain->getLatencySamples(); }
        int nonFiniteBlocks() const noexcept { return nonFinite; }
        celadon::glaze::Status status() const { return chain->getGlazeStatus(); }

    private:
        celadon::ChainParams params;
        celadon::glaze::Genre genre;
        HostTimeline timeline;
        double sampleRate;
        int blockSize;
        std::unique_ptr<celadon::CeladonChain> chain;   // 约 70 KB，放堆上（同处理器把链当成员放在堆上）
        int nonFinite = 0;
    };

   #if CELADON_BENCH_PROCESSOR_LAYER
    class BenchPlayHead final : public juce::AudioPlayHead
    {
    public:
        explicit BenchPlayHead (HostTimeline hostTimeline) : timeline (hostTimeline) {}

        void setPosition (std::int64_t newPosition) noexcept { position = newPosition; }

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            TimeSignature signature;
            signature.numerator = beatsPerBar;
            signature.denominator = 4;
            info.setIsPlaying (true);
            info.setIsLooping (false);
            info.setTimeInSamples (position);
            info.setTimeInSeconds ((double) position / timeline.sampleRate);
            info.setBpm (materialBpm);
            info.setPpqPosition (timeline.ppqAt (position));
            info.setPpqPositionOfLastBarStart (timeline.barStartPpqAt (position));
            info.setTimeSignature (signature);
            return info;
        }

    private:
        HostTimeline timeline;
        std::int64_t position = 0;
    };

    void setParameter (CeladonAudioProcessor& processor, const char* id, float value)
    {
        auto* parameter = processor.getParameters().getParameter (id);
        if (parameter == nullptr)
            throw InfrastructureError (std::string ("参数表里没有 ") + id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    void applyScenario (CeladonAudioProcessor& processor, const Scenario& scenario)
    {
        namespace id = celadon::param;
        setParameter (processor, id::glazeAmount, scenario.glazePercent);
        setParameter (processor, id::limitGain, scenario.limitGainDb);
        setParameter (processor, id::glazeGenre, (float) static_cast<int> (scenario.genre));
        setParameter (processor, id::glazeDelivery, (float) static_cast<int> (scenario.delivery));
    }

    class ProcessorLayer
    {
    public:
        ProcessorLayer (const Scenario& chosen, double rate, int block)
            : scenario (chosen), playHead (HostTimeline { rate }), sampleRate (rate), blockSize (block)
        {
        }

        ~ProcessorLayer()
        {
            if (processor != nullptr)
            {
                processor->releaseResources();
                processor->setPlayHead (nullptr);
            }
        }

        ProcessorLayer (const ProcessorLayer&) = delete;
        ProcessorLayer& operator= (const ProcessorLayer&) = delete;

        static constexpr bool hasSegments = false;

        double prepare()
        {
            processor = std::make_unique<CeladonAudioProcessor>();
            processor->setPlayConfigDetails (2, 2, sampleRate, blockSize);   // 同 bench.mm：主输入立体声，参考侧链关
            applyScenario (*processor, scenario);                            // 曲风要在 prepare 之前设（prepare 才同步主段模型）
            processor->setPlayHead (&playHead);
            const auto start = Clock::now();
            processor->prepareToPlay (sampleRate, blockSize);
            return 1.0e-6 * nanosecondsBetween (start, Clock::now());
        }

        BlockTiming process (juce::AudioBuffer<float>& buffer, std::int64_t position)
        {
            playHead.setPosition (position);
            const auto start = Clock::now();
            processor->processBlock (buffer, midi);
            const auto end = Clock::now();
            return { nanosecondsBetween (start, end), notAvailable(), notAvailable() };
        }

        int latency() const { return processor->getLatencySamples(); }
        int nonFiniteBlocks() const noexcept { return processor->getNonFiniteBlockCount(); }
        celadon::glaze::Status status() const { return processor->getGlazeStatus(); }

    private:
        const Scenario& scenario;
        BenchPlayHead playHead;   // 比处理器活得久（处理器后析构）
        double sampleRate;
        int blockSize;
        juce::MidiBuffer midi;
        std::unique_ptr<CeladonAudioProcessor> processor;
    };
   #endif

    //==============================================================================
    // 一行结果

    struct Row
    {
        std::string scenario, rowKind = "measured", basis = "direct", layer, genre, delivery;
        double glaze = notAvailable(), sampleRate = notAvailable();
        int blockSize = 0;

        double avg = notAvailable(), worst = notAvailable(), p999 = notAvailable(), maxAllPasses = notAvailable();
        double structWorst = notAvailable(), structWorst64k = notAvailable(), structWorst1s = notAvailable(), structWorst5ms = notAvailable();
        double threadCpuWorst = notAvailable(), threadCpuMaxAllPasses = notAvailable();
        double involuntarySwitches = notAvailable(), voluntarySwitches = notAvailable();
        double setParamsAvg = notAvailable(), setParamsStructWorst = notAvailable();
        double processAvg = notAvailable(), processStructWorst = notAvailable();
        double latency = notAvailable(), prepareMs = notAvailable(), nonFiniteBlocks = notAvailable();
        int blocksPerPass = 0, passes = 0;
        double warmSeconds = notAvailable(), passSeconds = notAvailable();
        double confidenceWarm = notAvailable(), confidenceEnd = notAvailable(), mainnessEnd = notAvailable(), checksum = notAvailable();
        std::vector<std::string> naReasons;

        void addReason (const char* reason)
        {
            if (std::find (naReasons.begin(), naReasons.end(), reason) == naReasons.end())
                naReasons.emplace_back (reason);
        }
    };

    std::string formatNumber (double value, const char* format)
    {
        if (! std::isfinite (value))
            return {};   // Q12：算不出写空值（原因在 na_reason），绝不填 0
        char text[64];
        std::snprintf (text, sizeof (text), format, value);
        return text;
    }

    std::string formatPercent (double value) { return formatNumber (value, "%.4f"); }
    std::string formatCount (double value) { return formatNumber (value, "%.0f"); }

    std::string joined (const std::vector<std::string>& items, const char* separator)
    {
        std::string text;
        for (std::size_t i = 0; i < items.size(); ++i)
            text += (i > 0 ? separator : "") + items[i];
        return text;
    }

    // 列名与取值写在同一处：表头由「空行」的列名生成，列序不会两处不一致。
    std::vector<std::pair<const char*, std::string>> rowCells (const Row& row)
    {
        return {
            { "scenario", row.scenario },
            { "row_kind", row.rowKind },
            { "basis", row.basis },
            { "layer", row.layer },
            { "glaze", formatNumber (row.glaze, "%.1f") },
            { "genre", row.genre },
            { "delivery", row.delivery },
            { "sr", formatCount (row.sampleRate) },
            { "bs", row.blockSize > 0 ? std::to_string (row.blockSize) : std::string() },
            { "avg", formatPercent (row.avg) },
            { "worst", formatPercent (row.worst) },
            { "p99.9", formatPercent (row.p999) },
            { "maxAllPasses", formatPercent (row.maxAllPasses) },
            { "structWorst", formatPercent (row.structWorst) },
            { "structWorst64k", formatPercent (row.structWorst64k) },
            { "structWorst_1s", formatPercent (row.structWorst1s) },
            { "structWorst_5ms", formatPercent (row.structWorst5ms) },
            { "thread_cpu_worst", formatPercent (row.threadCpuWorst) },
            { "thread_cpu_maxAllPasses", formatPercent (row.threadCpuMaxAllPasses) },
            { "invol_switches", formatCount (row.involuntarySwitches) },
            { "vol_switches", formatCount (row.voluntarySwitches) },
            { "setparams_avg", formatPercent (row.setParamsAvg) },
            { "setparams_structWorst", formatPercent (row.setParamsStructWorst) },
            { "process_avg", formatPercent (row.processAvg) },
            { "process_structWorst", formatPercent (row.processStructWorst) },
            { "latency", formatCount (row.latency) },
            { "prepare_ms", formatNumber (row.prepareMs, "%.3f") },
            { "nonfinite_blocks", formatCount (row.nonFiniteBlocks) },
            { "blocks_per_pass", row.blocksPerPass > 0 ? std::to_string (row.blocksPerPass) : std::string() },
            { "passes", row.passes > 0 ? std::to_string (row.passes) : std::string() },
            { "warm_s", formatNumber (row.warmSeconds, "%.3f") },
            { "pass_s", formatNumber (row.passSeconds, "%.3f") },
            { "confidence_warm", formatNumber (row.confidenceWarm, "%.4f") },
            { "confidence_end", formatNumber (row.confidenceEnd, "%.4f") },
            { "mainness_end", formatNumber (row.mainnessEnd, "%.4f") },
            { "chk", formatNumber (row.checksum, "%.6g") },
            { "na_reason", joined (row.naReasons, ";") },
        };
    }

    // 「分析器 + 控制层」估计行 = GLAZE > 0 那行 − GLAZE 0 孪生那行（同层、同采样率、同块长）。只对耗时类做差；
    // maxAllPasses（单次尖峰，噪声主导）、计数与状态不做差，写空值并注明。
    Row estimateRow (const Row& withGlaze, const Row& twin)
    {
        Row row;
        row.scenario = withGlaze.scenario;
        row.rowKind = "estimate";
        row.basis = withGlaze.scenario + "-minus-" + twin.scenario;
        row.layer = withGlaze.layer;
        row.glaze = withGlaze.glaze;
        row.genre = withGlaze.genre;
        row.delivery = withGlaze.delivery;
        row.sampleRate = withGlaze.sampleRate;
        row.blockSize = withGlaze.blockSize;
        row.blocksPerPass = withGlaze.blocksPerPass;
        row.passes = withGlaze.passes;
        row.warmSeconds = withGlaze.warmSeconds;
        row.passSeconds = withGlaze.passSeconds;

        row.avg = differenceOrNotAvailable (withGlaze.avg, twin.avg);
        row.worst = differenceOrNotAvailable (withGlaze.worst, twin.worst);
        row.p999 = differenceOrNotAvailable (withGlaze.p999, twin.p999);
        row.structWorst = differenceOrNotAvailable (withGlaze.structWorst, twin.structWorst);
        row.structWorst64k = differenceOrNotAvailable (withGlaze.structWorst64k, twin.structWorst64k);
        row.structWorst1s = differenceOrNotAvailable (withGlaze.structWorst1s, twin.structWorst1s);
        row.structWorst5ms = differenceOrNotAvailable (withGlaze.structWorst5ms, twin.structWorst5ms);
        row.threadCpuWorst = differenceOrNotAvailable (withGlaze.threadCpuWorst, twin.threadCpuWorst);
        row.setParamsAvg = differenceOrNotAvailable (withGlaze.setParamsAvg, twin.setParamsAvg);
        row.setParamsStructWorst = differenceOrNotAvailable (withGlaze.setParamsStructWorst, twin.setParamsStructWorst);
        row.processAvg = differenceOrNotAvailable (withGlaze.processAvg, twin.processAvg);
        row.processStructWorst = differenceOrNotAvailable (withGlaze.processStructWorst, twin.processStructWorst);

        row.addReason (naEstimateNotDifferenced);
        for (const auto& reason : withGlaze.naReasons)
            row.addReason (reason.c_str());
        for (const auto& reason : twin.naReasons)
            row.addReason (reason.c_str());
        return row;
    }

    //==============================================================================
    // 一个配置（场景 × 层 × 采样率 × 块长）的测量

    struct Groupings
    {
        Grouping balanceHop, longPeriod, decisionGrid, sliceGrid;
    };

    // Balance 的分析 hop 直接问模块（不再抄一份算法）：44.1 / 48 k 为 512，采样率每翻一倍翻一倍。
    std::int64_t balanceHopSamples (double sampleRate, int blockSize)
    {
        celadon::BalanceModule balance;
        balance.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
        return balance.getAnalysisHopSamples();
    }

    Groupings groupingsFor (double sampleRate, int blockSize)
    {
        Groupings g;
        g.balanceHop = { Grouping::Kind::blockStartPhase, balanceHopSamples (sampleRate, blockSize) };
        // 65536：LinearPhaseLowpass 与修坯峰值守护按固定网格精确重算滑动和的间隔（同 bench.mm structWorst64k）。
        g.longPeriod = { Grouping::Kind::blockStartPhase, (std::int64_t) celadon::LinearPhaseLowpass::resyncIntervalSamples };
        // 决策网格：编排器每 llround(采样率) 个样本决策一次，从 prepare（coldStart）起算——与本基准的样本位置同一零点。
        g.decisionGrid = { Grouping::Kind::gridCrossing, (std::int64_t) std::llround (decisionGridSeconds * sampleRate) };
        // 实时画像分片网格：同 LiveProfile::prepare 的换算（网格点 = 绝对样本，与宿主块长无关；本基准宿主位置 = 样本位置）。
        g.sliceGrid = { Grouping::Kind::gridCrossing,
                        std::max<std::int64_t> (1, (std::int64_t) std::llround (celadon::glaze::LiveProfile::sliceGridSeconds * sampleRate)) };
        return g;
    }

    struct RunConfig
    {
        double sampleRate;
        int blockSize;
        double warmSeconds;
        double passSeconds;
        int passes;
        Groupings groupings;
    };

    int blocksFor (double seconds, double sampleRate, int blockSize) noexcept
    {
        return (int) std::ceil (seconds * sampleRate / (double) blockSize);
    }

    template <typename LayerType>
    Row measureConfig (const Scenario& scenario, LayerType& layer, Layer kind, const juce::AudioBuffer<float>& material, const RunConfig& config)
    {
        juce::ScopedNoDenormals noDenormals;   // 链层没有处理器 processBuses 那一道 FTZ / DAZ：整段测量统一打开（处理器层里再设一次是空操作）

        Row row;
        row.scenario = scenario.name;
        row.layer = layerName (kind);
        row.glaze = scenario.glazePercent;
        row.genre = genreIds[(std::size_t) static_cast<int> (scenario.genre)];
        row.delivery = deliveryIds[(std::size_t) static_cast<int> (scenario.delivery)];
        row.sampleRate = config.sampleRate;
        row.blockSize = config.blockSize;
        row.passes = config.passes;
        row.warmSeconds = config.warmSeconds;
        row.passSeconds = config.passSeconds;

        row.prepareMs = layer.prepare();

        const auto blockSize = config.blockSize;
        juce::AudioBuffer<float> buffer (2, blockSize);
        Feeder feeder { material, 0 };   // 两个字段都给：GCC 的 -Wmissing-field-initializers 不看默认成员初值
        std::int64_t position = 0;

        const auto warmBlocks = blocksFor (config.warmSeconds, config.sampleRate, blockSize);
        for (int i = 0; i < warmBlocks; ++i)
        {
            feeder.fill (buffer);
            layer.process (buffer, position);
            position += blockSize;
        }
        row.confidenceWarm = (double) layer.status().confidence;

        const auto blocksPerPass = std::max (minimumBlocksPerPass, blocksFor (config.passSeconds, config.sampleRate, blockSize));
        const auto totalBlocks = (std::size_t) blocksPerPass * (std::size_t) config.passes;
        row.blocksPerPass = blocksPerPass;

        // 计时数组先分配好：计时循环里不分配（分配会扰动缓存，也是被测代码本身不许做的事）。
        std::vector<double> wall (totalBlocks), threadCpu (totalBlocks), setParams (totalBlocks), process (totalBlocks);
        const auto firstTimedPosition = position;
        long long voluntary = 0, involuntary = 0;
        bool switchesValid = true;
        double checksum = 0.0;

        for (int pass = 0; pass < config.passes; ++pass)
        {
            const auto before = readThreadSwitches();
            for (int i = 0; i < blocksPerPass; ++i)
            {
                feeder.fill (buffer);
                const auto index = (std::size_t) pass * (std::size_t) blocksPerPass + (std::size_t) i;
                // 线程 CPU 读数夹在墙钟计时外面：两次 clock_gettime 系统调用不算进墙钟。
                const auto cpuStart = threadCpuNanoseconds();
                const auto timing = layer.process (buffer, position);
                const auto cpuEnd = threadCpuNanoseconds();
                wall[index] = timing.wall;
                setParams[index] = timing.setParams;
                process[index] = timing.process;
                threadCpu[index] = cpuEnd - cpuStart;
                position += blockSize;
                checksum += (double) std::abs (buffer.getSample (0, blockSize / 2));
            }
            const auto after = readThreadSwitches();
            switchesValid = switchesValid && before.valid && after.valid;
            voluntary += after.voluntary - before.voluntary;
            involuntary += after.involuntary - before.involuntary;
        }

        const auto status = layer.status();
        row.confidenceEnd = (double) status.confidence;
        row.mainnessEnd = (double) status.currentMainness;
        row.latency = (double) layer.latency();
        row.nonFiniteBlocks = (double) layer.nonFiniteBlocks();
        row.checksum = checksum;

        const auto blockNanoseconds = 1.0e9 * (double) blockSize / config.sampleRate;
        const auto& g = config.groupings;
        const auto wallSummary = summarisePasses (wall, blocksPerPass, config.passes);
        row.avg = percentOfBlock (wallSummary.bestMean, blockNanoseconds);
        row.worst = percentOfBlock (wallSummary.bestMax, blockNanoseconds);
        row.p999 = percentOfBlock (wallSummary.bestP999, blockNanoseconds);
        row.maxAllPasses = percentOfBlock (wallSummary.maxAll, blockNanoseconds);
        row.structWorst = percentOfBlock (structuralWorst (wall, firstTimedPosition, blockSize, g.balanceHop), blockNanoseconds);
        row.structWorst64k = percentOfBlock (structuralWorst (wall, firstTimedPosition, blockSize, g.longPeriod), blockNanoseconds);
        row.structWorst1s = percentOfBlock (structuralWorst (wall, firstTimedPosition, blockSize, g.decisionGrid), blockNanoseconds);
        row.structWorst5ms = percentOfBlock (structuralWorst (wall, firstTimedPosition, blockSize, g.sliceGrid), blockNanoseconds);

        const auto cpuAvailable = std::all_of (threadCpu.begin(), threadCpu.end(), [] (double v) { return std::isfinite (v); });
        if (cpuAvailable)
        {
            const auto cpuSummary = summarisePasses (threadCpu, blocksPerPass, config.passes);
            row.threadCpuWorst = percentOfBlock (cpuSummary.bestMax, blockNanoseconds);
            row.threadCpuMaxAllPasses = percentOfBlock (cpuSummary.maxAll, blockNanoseconds);
        }
        else
        {
            row.addReason (naThreadCpuUnavailable);
        }

        if (switchesValid)
        {
            row.voluntarySwitches = (double) voluntary;
            row.involuntarySwitches = (double) involuntary;
        }
        else
        {
            row.addReason (naRusageThreadUnavailable);
        }

        if constexpr (LayerType::hasSegments)
        {
            row.setParamsAvg = percentOfBlock (summarisePasses (setParams, blocksPerPass, config.passes).bestMean, blockNanoseconds);
            row.setParamsStructWorst = percentOfBlock (structuralWorst (setParams, firstTimedPosition, blockSize, g.balanceHop), blockNanoseconds);
            row.processAvg = percentOfBlock (summarisePasses (process, blocksPerPass, config.passes).bestMean, blockNanoseconds);
            row.processStructWorst = percentOfBlock (structuralWorst (process, firstTimedPosition, blockSize, g.balanceHop), blockNanoseconds);
        }
        else
        {
            row.addReason (naSegmentsChainOnly);
        }

        return row;
    }

    Row measureWithLayer (Layer kind, const Scenario& scenario, const juce::AudioBuffer<float>& material, const RunConfig& config)
    {
       #if CELADON_BENCH_PROCESSOR_LAYER
        if (kind == Layer::processor)
        {
            ProcessorLayer layer (scenario, config.sampleRate, config.blockSize);
            return measureConfig (scenario, layer, kind, material, config);
        }
       #endif
        ChainLayer layer (scenario, config.sampleRate, config.blockSize);
        return measureConfig (scenario, layer, kind, material, config);
    }

    //==============================================================================
    // TSV 输出（副作用集中在这里）

    class TsvWriter
    {
    public:
        explicit TsvWriter (const std::string& path)
        {
            if (path == "-")
            {
                file = stdout;
            }
            else
            {
                file = std::fopen (path.c_str(), "w");
                ownsFile = true;
                if (file == nullptr)
                    throw InfrastructureError ("打不开输出文件 " + path);
            }

            try
            {
                writeLine (rowCells (Row {}), true);
            }
            catch (...)
            {
                // 构造函数抛出时析构函数不会跑：自己关掉刚打开的文件。
                if (ownsFile)
                    std::fclose (file);
                file = nullptr;
                throw;
            }
        }

        ~TsvWriter()
        {
            if (ownsFile && file != nullptr)
                std::fclose (file);
        }

        TsvWriter (const TsvWriter&) = delete;
        TsvWriter& operator= (const TsvWriter&) = delete;

        void write (const Row& row) { writeLine (rowCells (row), false); }

        // 正常收尾：关文件并检查写入错误（析构里不抛）。
        void finish()
        {
            const auto failed = std::fflush (file) != 0 || std::ferror (file) != 0;
            if (ownsFile)
            {
                const auto closeFailed = std::fclose (file) != 0;
                file = nullptr;
                if (closeFailed)
                    throw InfrastructureError ("关闭输出文件失败");
            }
            if (failed)
                throw InfrastructureError ("写输出失败");
        }

    private:
        void writeLine (const std::vector<std::pair<const char*, std::string>>& cells, bool header)
        {
            std::string line;
            for (std::size_t i = 0; i < cells.size(); ++i)
                line += (i > 0 ? "\t" : "") + (header ? std::string (cells[i].first) : cells[i].second);
            line += "\n";
            if (std::fputs (line.c_str(), file) < 0 || std::fflush (file) != 0)
                throw InfrastructureError ("写输出失败");
        }

        FILE* file = nullptr;
        bool ownsFile = false;
    };

    //==============================================================================
    // 命令行

    struct Options
    {
        std::vector<const Scenario*> scenarios;
        std::vector<double> sampleRates;
        std::vector<int> blockSizes;
        std::vector<Layer> layers;
        double warmSeconds = defaultWarmSeconds;
        double passSeconds = 0.0;   // 0 = 素材一整圈
        int passes = defaultPasses;
        std::string outPath = "-";
    };

    enum class Mode { run, list, selftest, help };

    std::vector<std::string> splitList (const std::string& text)
    {
        std::vector<std::string> items;
        std::string current;
        for (const auto c : text)
        {
            if (c == ',')
            {
                items.push_back (current);
                current.clear();
            }
            else
            {
                current += c;
            }
        }
        items.push_back (current);
        return items;
    }

    bool parseNumber (const std::string& text, double& value)
    {
        if (text.empty())
            return false;
        char* end = nullptr;
        value = std::strtod (text.c_str(), &end);
        return end != nullptr && *end == '\0' && std::isfinite (value);
    }

    bool parseSampleRates (const std::string& text, std::vector<double>& out, std::string& error)
    {
        out.clear();
        for (const auto& item : splitList (text))
        {
            double value = 0.0;
            if (! parseNumber (item, value) || value < 8000.0 || value > 768000.0)
            {
                error = "--sr 的值要在 8000 ～ 768000：" + item;
                return false;
            }
            out.push_back (value);
        }
        return true;
    }

    bool parseBlockSizes (const std::string& text, std::vector<int>& out, std::string& error)
    {
        out.clear();
        for (const auto& item : splitList (text))
        {
            double value = 0.0;
            if (! parseNumber (item, value) || value < 1.0 || value > 16384.0 || value != std::floor (value))
            {
                error = "--bs 的值要是 1 ～ 16384 的整数：" + item;
                return false;
            }
            out.push_back ((int) value);
        }
        return true;
    }

    bool parseScenarios (const std::string& text, std::vector<const Scenario*>& out, std::string& error)
    {
        out.clear();
        if (text == "all")
        {
            for (const auto& scenario : scenarioTable)
                out.push_back (&scenario);
            return true;
        }
        for (const auto& item : splitList (text))
        {
            const auto* scenario = findScenario (item);
            if (scenario == nullptr)
            {
                error = "没有这个场景：" + item + "（--list 看全部）";
                return false;
            }
            out.push_back (scenario);
        }
        return true;
    }

    bool parseLayers (const std::string& text, std::vector<Layer>& out, std::string& error)
    {
        out.clear();
        if (text == "all")
        {
            if (processorLayerAvailable())
                out.push_back (Layer::processor);
            out.push_back (Layer::chain);
            return true;
        }
        for (const auto& item : splitList (text))
        {
            if (item == "processor" && processorLayerAvailable())
                out.push_back (Layer::processor);
            else if (item == "chain")
                out.push_back (Layer::chain);
            else
            {
                error = item == "processor" ? "这个构建只有链层（没有界面库，见根 CMakeLists.txt 的 celadon_bench_portable）"
                                            : "--layer 只认 processor / chain / all：" + item;
                return false;
            }
        }
        return true;
    }

    void printUsage (FILE* stream)
    {
        std::fprintf (stream,
                      "用法：celadon_bench_portable [--scenario 名字,名字|all] [--sr %s] [--bs %s]\n"
                      "                             [--layer processor,chain|all] [--warm 秒（默认 %.0f）] [--pass 秒（0 = 素材一整圈，默认 0）]\n"
                      "                             [--passes N（默认 %d）] [--out 文件|-（默认 - = 标准输出）]\n"
                      "       celadon_bench_portable --list | --selftest | --help\n"
                      "本构建的层：%s\n",
                      defaultSampleRates, defaultBlockSizes, defaultWarmSeconds, defaultPasses,
                      processorLayerAvailable() ? "processor, chain" : "chain（没有界面库）");
    }

    bool parseArguments (int argc, char* argv[], Options& options, Mode& mode, std::string& error)
    {
        mode = Mode::run;
        std::string scenarios = "all", sampleRates = defaultSampleRates, blockSizes = defaultBlockSizes, layers = "all";

        for (int i = 1; i < argc; ++i)
        {
            const std::string name (argv[i]);
            if (name == "--list") { mode = Mode::list; continue; }
            if (name == "--selftest") { mode = Mode::selftest; continue; }
            if (name == "--help" || name == "-h") { mode = Mode::help; continue; }

            if (i + 1 >= argc)
            {
                error = "缺少 " + name + " 的值";
                return false;
            }
            const std::string value (argv[++i]);
            double number = 0.0;

            if (name == "--scenario") scenarios = value;
            else if (name == "--sr") sampleRates = value;
            else if (name == "--bs") blockSizes = value;
            else if (name == "--layer") layers = value;
            else if (name == "--out") options.outPath = value;
            else if (name == "--warm")
            {
                if (! parseNumber (value, number) || number < 0.0) { error = "--warm 要是 ≥ 0 的秒数"; return false; }
                options.warmSeconds = number;
            }
            else if (name == "--pass")
            {
                if (! parseNumber (value, number) || number < 0.0) { error = "--pass 要是 ≥ 0 的秒数（0 = 素材一整圈）"; return false; }
                options.passSeconds = number;
            }
            else if (name == "--passes")
            {
                if (! parseNumber (value, number) || number < 1.0 || number > 1000.0 || number != std::floor (number))
                {
                    error = "--passes 要是 1 ～ 1000 的整数";
                    return false;
                }
                options.passes = (int) number;
            }
            else
            {
                error = "不认识的选项：" + name;
                return false;
            }
        }

        return parseScenarios (scenarios, options.scenarios, error)
            && parseSampleRates (sampleRates, options.sampleRates, error)
            && parseBlockSizes (blockSizes, options.blockSizes, error)
            && parseLayers (layers, options.layers, error);
    }

    void printScenarioList()
    {
        std::printf ("scenario\tglaze\tlimit_gain_db\tgenre\tdelivery\ttwin\n");
        for (const auto& s : scenarioTable)
            std::printf ("%s\t%.1f\t%.1f\t%s\t%s\t%s\n", s.name, (double) s.glazePercent, (double) s.limitGainDb,
                         genreIds[(std::size_t) static_cast<int> (s.genre)], deliveryIds[(std::size_t) static_cast<int> (s.delivery)],
                         s.twin != nullptr ? s.twin : "-");
        std::printf ("# layers in this build: %s\n", processorLayerAvailable() ? "processor,chain" : "chain");
        std::printf ("# material: %d bars @ %.0f BPM = %.3f s loop, seed %lld\n", materialBars, materialBpm, materialLoopSeconds(),
                     (long long) materialSeed);
    }

    //==============================================================================
    // 主流程

    int runBenchmark (const Options& options)
    {
        if (options.warmSeconds < minimumWarmSeconds)
            std::fprintf (stderr, "提示：空跑 %.1f s 少于 %.0f s，实时画像可能还没进稳态（计划 §13）\n", options.warmSeconds, minimumWarmSeconds);

        TsvWriter writer (options.outPath);
        const auto scenarios = withTwins (options.scenarios);
        bool sawNonFinite = false;

        for (const auto sampleRate : options.sampleRates)
        {
            const auto material = makeMaterial (sampleRate);
            const auto loopSeconds = (double) material.getNumSamples() / sampleRate;

            for (const auto blockSize : options.blockSizes)
            {
                const RunConfig config { sampleRate, blockSize, options.warmSeconds,
                                         options.passSeconds > 0.0 ? options.passSeconds : loopSeconds,
                                         options.passes, groupingsFor (sampleRate, blockSize) };

                for (const auto layer : options.layers)
                {
                    std::map<std::string, Row> measured;
                    for (const auto* scenario : scenarios)
                    {
                        const auto started = Clock::now();
                        auto row = measureWithLayer (layer, *scenario, material, config);
                        std::fprintf (stderr, "PROGRESS,%s,%s,%.0f,%d,structWorst%%=%s,wall_s=%.1f\n", scenario->name, layerName (layer),
                                      sampleRate, blockSize, formatPercent (row.structWorst).c_str(),
                                      1.0e-9 * nanosecondsBetween (started, Clock::now()));
                        sawNonFinite = sawNonFinite || (std::isfinite (row.nonFiniteBlocks) && row.nonFiniteBlocks > 0.0);
                        writer.write (row);
                        measured.emplace (scenario->name, std::move (row));
                    }

                    for (const auto* scenario : scenarios)
                    {
                        if (scenario->twin == nullptr)
                            continue;
                        const auto withGlaze = measured.find (scenario->name);
                        const auto twin = measured.find (scenario->twin);
                        if (withGlaze != measured.end() && twin != measured.end())
                            writer.write (estimateRow (withGlaze->second, twin->second));
                    }
                }
            }
        }

        writer.finish();

        if (sawNonFinite)
        {
            std::fprintf (stderr, "错误：有配置的输出出现非有限值（nonfinite_blocks > 0），见 TSV\n");
            return 1;
        }
        return 0;
    }

    //==============================================================================
    // 自检：只覆盖纯逻辑，答案都是事先算好的

    struct SelfTest
    {
        int failures = 0;

        void check (bool ok, const std::string& name)
        {
            std::printf ("SELFTEST,%s,%s\n", ok ? "PASS" : "FAIL", name.c_str());
            if (! ok)
                ++failures;
        }
    };

    bool nearlyEqual (double a, double b, double tolerance) noexcept { return std::abs (a - b) <= tolerance; }

    void testStatistics (SelfTest& t)
    {
        t.check (medianOf ({ 5.0, 1.0, 3.0 }) == 3.0, "median of odd count");
        t.check (medianOf ({ 4.0, 1.0, 3.0, 2.0 }) == 3.0, "median of even count takes the upper middle (bench.mm v[n/2])");
        t.check (std::isnan (medianOf ({})), "median of nothing is NaN, not 0");

        // 两遍：[1 2 3 10]（均值 4、最大 10、p99.9 = 第 3 个 = 10）与 [2 2 2 2]（均值 2、最大 2）。
        const auto s = summarisePasses ({ 1.0, 2.0, 3.0, 10.0, 2.0, 2.0, 2.0, 2.0 }, 4, 2);
        t.check (s.bestMean == 2.0 && s.bestMax == 2.0 && s.bestP999 == 2.0 && s.maxAll == 10.0,
                 "passes reduce to best-of-passes (mean / max / p99.9) and max of all blocks");
        t.check (std::isnan (summarisePasses ({ 1.0 }, 4, 2).bestMean), "too few values give NaN summaries");

        t.check (nearlyEqual (percentOfBlock (100000.0, 1.0e9 * 32.0 / 48000.0), 15.0, 1e-9), "100 us in a 48 kHz / 32 block is 15 %");
        t.check (std::isnan (percentOfBlock (notAvailable(), 1000.0)), "percent of NaN stays NaN");
        t.check (differenceOrNotAvailable (3.0, 1.0) == 2.0 && std::isnan (differenceOrNotAvailable (notAvailable(), 1.0)),
                 "estimate difference propagates missing values");

        t.check (formatPercent (notAvailable()).empty() && formatPercent (1.23456) == "1.2346" && formatCount (0.0) == "0",
                 "missing values print empty, real zeros print 0");
    }

    void testGroupings (SelfTest& t)
    {
        const Grouping hop512 { Grouping::Kind::blockStartPhase, 512 };
        t.check (groupCount (hop512, 32) == 16 && groupIndex (hop512, 0, 32) == 0 && groupIndex (hop512, 480, 32) == 15
                     && groupIndex (hop512, 512, 32) == 0,
                 "block-start phase grouping matches bench.mm (hop 512, block 32)");
        t.check (groupCount (hop512, 512) == 1 && groupCount (hop512, 1024) == 1 && groupIndex (hop512, 1536, 1024) == 0,
                 "block-start phase grouping collapses to one group when block >= period");

        const Grouping second48k { Grouping::Kind::gridCrossing, 48000 };
        t.check (groupCount (second48k, 32) == 1500 && groupIndex (second48k, 47968, 32) == 0
                     && groupIndex (second48k, 47936, 32) == 1499 && groupIndex (second48k, 0, 32) == 1,
                 "grid-crossing grouping at 48 kHz / 32: the block holding sample 47999 is group 0");

        const Grouping second44k { Grouping::Kind::gridCrossing, 44100 };
        t.check (groupCount (second44k, 32) == 1379 && groupIndex (second44k, 44096, 32) == 0 && groupIndex (second44k, 44064, 32) == 1378,
                 "grid-crossing grouping at 44.1 kHz / 32: crossing block is group 0 although 44100 is not a multiple of 32");

        // 暴力核对：group 0 ⇔ 块 (pos, pos + bs] 里恰有网格点；各种周期 / 块长组合，走 10 个周期。
        bool exact = true;
        const std::array<std::pair<std::int64_t, int>, 6> cases { { { 48000, 32 }, { 44100, 32 }, { 44100, 64 }, { 221, 32 }, { 240, 512 }, { 960, 64 } } };
        for (const auto& [period, block] : cases)
        {
            const Grouping g { Grouping::Kind::gridCrossing, period };
            int crossings = 0, groupZero = 0;
            for (std::int64_t pos = 0; pos + block <= 10 * period; pos += block)
            {
                const auto holdsGridPoint = (pos + block) / period > pos / period;
                const auto index = groupIndex (g, pos, block);
                exact = exact && (index == 0) == holdsGridPoint && index >= 0 && index < groupCount (g, block);
                crossings += holdsGridPoint ? 1 : 0;
                groupZero += index == 0 ? 1 : 0;
            }
            exact = exact && crossings == groupZero;
        }
        t.check (exact, "grid-crossing group 0 is exactly the set of blocks that contain a grid point (brute force)");

        // 结构性最坏：基线 10，每个 1 s 网格点那一块 100（周期 1000、块 10 → 每 100 块一次，落在 hop-40 分组的同一相位 3 里只占 1/25）。
        std::vector<double> values (2000, 10.0);
        for (std::size_t i = 99; i < values.size(); i += 100)
            values[i] = 100.0;
        const Grouping hop40 { Grouping::Kind::blockStartPhase, 40 };
        const Grouping grid1000 { Grouping::Kind::gridCrossing, 1000 };
        t.check (structuralWorst (values, 0, 10, hop40) == 10.0, "hop-phase structWorst filters out a once-per-second spike");
        t.check (structuralWorst (values, 0, 10, grid1000) == 100.0, "decision-grid structWorst keeps the once-per-second spike");
        t.check (std::isnan (structuralWorst ({}, 0, 10, grid1000)), "structWorst of no data is NaN");
    }

    void testGroupingsFromSource (SelfTest& t)
    {
        t.check (balanceHopSamples (44100.0, 32) == 512 && balanceHopSamples (48000.0, 32) == 512 && balanceHopSamples (96000.0, 64) == 1024
                     && balanceHopSamples (192000.0, 512) == 2048,
                 "Balance analysis hop read from BalanceModule: 512 / 512 / 1024 / 2048");
        const auto g = groupingsFor (44100.0, 32);
        t.check (g.longPeriod.periodSamples == 65536 && g.decisionGrid.periodSamples == 44100 && g.sliceGrid.periodSamples == 221,
                 "44.1 kHz grids: 65536 / 1 s = 44100 / 5 ms = llround(220.5) = 221");
        t.check (groupingsFor (192000.0, 32).sliceGrid.periodSamples == 960, "192 kHz slice grid is 960 samples");
    }

    void testScenarios (SelfTest& t)
    {
        bool twinsOk = true, namesUnique = true;
        for (const auto& s : scenarioTable)
        {
            for (const auto& other : scenarioTable)
                namesUnique = namesUnique && (&s == &other || std::strcmp (s.name, other.name) != 0);
            if (s.twin == nullptr)
            {
                twinsOk = twinsOk && s.glazePercent == 0.0f;
                continue;
            }
            const auto* twin = findScenario (s.twin);
            twinsOk = twinsOk && twin != nullptr && s.glazePercent > 0.0f && twin->glazePercent == 0.0f && twin->twin == nullptr
                   && twin->limitGainDb == s.limitGainDb && twin->genre == s.genre && twin->delivery == s.delivery;
        }
        t.check (namesUnique, "scenario names are unique");
        t.check (twinsOk, "every GLAZE > 0 scenario has a GLAZE 0 twin that differs only in GLAZE");

        const auto* d = findScenario ("default");
        t.check (d != nullptr && d->glazePercent == defaults::glazeAmount && d->genre == factoryGenre && d->delivery == factoryDelivery,
                 "default follows the factory defaults (ParameterDefaults.h)");
        const auto* house = findScenario ("house-club-100");
        t.check (house != nullptr && house->genre == Genre::deepHouse && house->delivery == Delivery::club && house->glazePercent == 100.0f,
                 "house-club-100 is Deep House + club + GLAZE 100");
        const auto* manual = findScenario ("manual");
        const auto manualParams = chainParamsFor (*manual);
        t.check (manualParams.glazePercent == 0.0f && manualParams.limit.gainDb == 9.0f, "manual is GLAZE 0 + Gain 9 (bench.mm)");

        const auto resolved = withTwins ({ findScenario ("auto100"), findScenario ("default"), findScenario ("house-club-35") });
        std::vector<std::string> names;
        for (const auto* s : resolved)
            names.emplace_back (s->name);
        t.check (names == std::vector<std::string> { "auto100", "default-g0", "default", "house-club-35", "house-club-g0" },
                 "twins are inserted right after their first GLAZE > 0 scenario, once");
    }

    double rmsOfBars (const juce::AudioBuffer<float>& material, double sampleRate, int firstBar, int lastBar)
    {
        const auto barSamples = (double) beatsPerBar * secondsPerBeat() * sampleRate;
        const auto from = (int) std::llround ((double) firstBar * barSamples);
        const auto to = juce::jmin (material.getNumSamples(), (int) std::llround ((double) (lastBar + 1) * barSamples));
        double sum = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int n = from; n < to; ++n)
                sum += (double) material.getSample (ch, n) * (double) material.getSample (ch, n);
        return std::sqrt (sum / (2.0 * (double) juce::jmax (1, to - from)));
    }

    void testMaterial (SelfTest& t)
    {
        t.check (materialLoopSamples (48000.0) == 2972903 && materialLoopSamples (44100.0) == 2731355
                     && materialLoopSamples (192000.0) == 11891613,
                 "material loop is 32 bars at 124 BPM (61.935 s): 2972903 samples at 48 kHz");

        constexpr double testRate = 8000.0;   // 低采样率生成快；结构与种子相同
        const auto a = makeMaterial (testRate);
        const auto b = makeMaterial (testRate);
        bool identical = a.getNumSamples() == b.getNumSamples();
        bool stereo = false;
        float peak = 0.0f;
        for (int ch = 0; ch < 2 && identical; ++ch)
            for (int n = 0; n < a.getNumSamples(); ++n)
            {
                identical = identical && a.getSample (ch, n) == b.getSample (ch, n);
                peak = juce::jmax (peak, std::abs (a.getSample (ch, n)));
            }
        for (int n = 0; n < a.getNumSamples() && ! stereo; ++n)
            stereo = a.getSample (0, n) != a.getSample (1, n);
        t.check (identical, "material is deterministic (two generations are bit-identical)");
        t.check (nearlyEqual ((double) peak, materialPeakLinear, 1.0e-6), "material peak is normalised to -1 dBFS");
        t.check (stereo, "material is stereo (left and right differ)");

        const auto intro = rmsOfBars (a, testRate, 0, 7), drop = rmsOfBars (a, testRate, 8, 15);
        const auto breakdown = rmsOfBars (a, testRate, 16, 23), drop2 = rmsOfBars (a, testRate, 24, 31);
        const auto db = [] (double x) { return 20.0 * std::log10 (juce::jmax (1.0e-12, x)); };
        t.check (db (drop) - db (intro) >= 3.0 && db (drop) - db (breakdown) >= 3.0,
                 "drop is at least 3 dB louder than intro and breakdown (main vs non-main sections)");
        t.check (std::abs (db (drop) - db (drop2)) <= 1.0, "both drops are within 1 dB");
    }

    void testHostTimeline (SelfTest& t)
    {
        const HostTimeline timeline { 48000.0 };
        const auto quarter = timeline.samplesPerQuarter();
        const auto atFiveAndHalf = (std::int64_t) std::llround (5.5 * quarter);
        const auto info = timeline.hostInfoAt (atFiveAndHalf);
        t.check (nearlyEqual (quarter, 60.0 / 124.0 * 48000.0, 1e-9) && timeline.ppqAt (0) == 0.0, "timeline starts at ppq 0, 124 BPM");
        t.check (nearlyEqual (info.ppqPosition, 5.5, 1e-4) && info.ppqLastBarStart == 4.0 && info.positionSamples == atFiveAndHalf,
                 "timeline reports ppq, last bar start and position in samples");
        t.check (info.hasPlayState && info.isPlaying && info.hasTempo && info.bpm == materialBpm && info.hasTimeSignature
                     && info.timeSignatureNumerator == 4 && info.timeSignatureDenominator == 4 && ! info.offline && info.hasLoop && ! info.looping,
                 "timeline is playing, realtime, 4/4, loop off");
    }

    int runSelfTest()
    {
        SelfTest t;
        testStatistics (t);
        testGroupings (t);
        testGroupingsFromSource (t);
        testScenarios (t);
        testMaterial (t);
        testHostTimeline (t);
        std::printf ("SELFTEST,%s,%d failure(s)\n", t.failures == 0 ? "OK" : "FAILED", t.failures);
        return t.failures == 0 ? 0 : 1;
    }
}

int main (int argc, char* argv[])
{
    Options options;
    Mode mode = Mode::run;
    std::string error;

    if (! parseArguments (argc, argv, options, mode, error))
    {
        std::fprintf (stderr, "%s\n", error.c_str());
        printUsage (stderr);
        return 2;
    }

    if (mode == Mode::help)
    {
        printUsage (stdout);
        return 0;
    }

    if (mode == Mode::list)
    {
        printScenarioList();
        return 0;
    }

    if (mode == Mode::selftest)
        return runSelfTest();

   #if CELADON_BENCH_PROCESSOR_LAYER
    // 处理器的参数表与 30 Hz 定时器要消息管理器；不跑消息循环（定时器消息只排队，约 300 ms 重发一次），Linux 上不连 X。
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
   #endif

    try
    {
        return runBenchmark (options);
    }
    catch (const InfrastructureError& e)
    {
        std::fprintf (stderr, "错误：%s\n", e.what());
        return 1;
    }
}
