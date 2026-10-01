/*
  ==============================================================================

    BalancePerformanceTests.cpp
    设计规格-Balance「CPU：每 512 样本一次 2048 点 FFT + 54 个双二阶，48 k 单核 < 2%」的实测，
    以及 v0.5 性能优化（2026-09-25，研究 P4 R7）补上的口径。

    一、原口径（v0.3 起，保留）：48 kHz 立体声、块 512、Recover = Tame = 60（Mid），处理 20 s 固定种子的宽带白噪声，
    只给 process() 计时（juce::Time::getHighResolutionTicks），算「处理时间 / 音频时长」的百分比。
    同一套办法再量 Color（Heat 50、Glue 50）与整条 CeladonChain（三个模块都开着、都不极端），
    三组数字一起记进日志，方便横向比较。每组都先空跑 1 s，再连续跑 3 遍 20 s 取最短的一遍。
    只断言
      1. 输出全是有限值；
      2. Balance 的占用 < 25%（比规格宽 12 倍：真出大问题——比如每样本重算系数、每块重新分配——才会红）；
      3. 各组的 process() 从 prepare 之后第一块起都不经 operator new 分配（计数见 TestMain.cpp；
         直接 std::malloc 的 JUCE HeapBlock 不在计数内）——2026-09-25 评审 R2-03 补上计划承诺的这一条。
    规格的「< 2%」只作为日志里的对照写出来。

    二、v0.5 口径（研究 P4 R7）：白噪下 Balance 只有约 5 段滤波器在工作，量到的只是真实成本的零头；分析尖峰也只看
    平均看不出来。所以另外量：
      - 素材换成「梳状」：粉噪 + 每隔一段一个正弦，Recover = Tame = 100 时二十多段同时在工作（最坏的滤波成本）；
      - 每遍先重新 prepare（每遍每块的工作完全相同），每块取多遍里最短的一次（去掉中断与抢占），再取均值与最坏块；
      - 测试线程在 macOS 上临时设成 QoS user-interactive（落在性能核上，与 DAW 实时线程待遇接近），测完恢复；
      - 处理器层（链 + 输入 / 输出两块表 + 两次 NaN 扫描，与 PluginProcessor::processChain 同样的工作；v0.6 起两块表、
        输出 NaN 扫描与 GLAZE 编排器都在链里，IN 表开着响度计量）也记一笔，块 512 的均值与块 64 的最坏块。
    门槛按「电池 + 低电量模式（本机约慢 1.45 倍）也不误报、真退化一定红」定，见各常量处的实测值。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/BalanceModule.h"
#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/ColorModule.h"

#if JUCE_MAC
 #include <pthread.h>
 #include <pthread/qos.h>
#endif

namespace celadon::test
{
    namespace
    {
        constexpr double sampleRate = 48000.0;
        constexpr int numChannels = 2;
        constexpr int blockSize = 512;

        constexpr double warmUpSeconds = 1.0;
        constexpr double measuredSeconds = 20.0;
        constexpr int numPasses = 3;

        // 规格门槛（只记录）与测试门槛（真断言）。
        constexpr double specLoadPercent = 2.0;
        constexpr double balanceLoadLimitPercent = 25.0;

        // v0.5 门槛（研究 P4 R7；2026-09-25 本机 M2 Pro、-O3 测试构建实测后定，接电源 / 电池 + 低电量模式各测一次）：
        //  - 梳状素材 Recover = Tame = 100、48k 块 512 的 Balance 均值：两声道打包后接电源 0.20%、低电量 0.29%；
        //    v0.4 的逐段标量写法接电源 0.62%、低电量 0.90%。AArch64 门槛 0.45%：低电量下仍留 1.5 倍余量，
        //    退回 v0.4 那种写法在任何供电状态下都会红。其他平台（x86、32 位 ARM）走标量路径，
        //    只断言一个宽松的上限 2%（M2 Pro 上 v0.4 写法 0.62%，给慢一些的 x86 机器留 3 倍）。
        //  - 立体声 / 单声道（同一素材、同一次运行）：打包后实测 1.14；把打包路径关掉（只剩新的标量写法，接电源 0.35%，
        //    绝对门槛拦不住）实测 1.90。门槛 1.4，只在 AArch64 上断言，与机器快慢、供电状态无关。
        //  - 梳状素材下至少 15 段在工作（实测 22 段）：素材失效（比如只剩几段在动）时测出来的数字没有意义，先红。
        //  - 分析拆片：192k 块 32 的 Balance 最坏块，拆片后应不超过一口气做完（测试开关）的 65%；实测 0.51
        //    （37.3 → 19.1 µs：两次 8192 点 FFT 分到两块里）。同一次运行里两种做法对比，与机器快慢、供电状态无关。
        //  - 处理器层 48k 块 512 的均值：宽松上限 8%（研究 P4 R7 的建议值），防大退化。v0.5 各线优化集成后实测
        //    （2026-09-25 16:07，接电源）默认 0.68%、典型 1.12%，低电量下约 1.0% / 1.6%，余量 5 倍以上；
        //    v0.4 同口径是接电源 7.7% / 7.9%、低电量 11.3%。
       #if defined (__ARM_NEON) && defined (__aarch64__)
        constexpr double combLoadLimitPercent = 0.45;   // 两声道打包路径
        constexpr double stereoOverMonoLimit = 1.4;
       #else
        constexpr double combLoadLimitPercent = 2.0;    // 标量路径
       #endif
        constexpr int minimumActiveBandsOnComb = 15;
        constexpr double slicedWorstBlockRatioLimit = 0.65;
        constexpr double processorLayerLoadLimitPercent = 8.0;
        // 单音频线程预算上限：处理耗时 / 该块播放时长，不是活动监视器里的整机百分比。
        // Apple M2 Pro 上整插件的历史读数约 1.3%（48 kHz）到 5%（192 kHz）。15% 用来拦住空转回归
        //（2026-10-01 Max 定由 10% 放宽到 15%：云端 4 核测试机上，未改动的 main 在 96 kHz、块 64 读到 11.4%）。
        constexpr double audioThreadBudgetPercent = 15.0;

        // 白噪声本身就是宽带：每个 bin 都有能量，27 段都不会掉进 −60 dBFS 的静音门，
        // 分析、判断、27 个滤波器全程都在干活。幅度 0.5（峰值 −6 dBFS、RMS 约 −10.8 dBFS）：
        // 留出头顶，让整条链里 Limit Gain 抬上去之后限幅器真的有峰值可压（增益值见 makeChainParams）。
        constexpr float noiseAmplitude = 0.5f;
        constexpr juce::int64 noiseSeed = 20260924;

        struct Measurement
        {
            double bestLoadPercent = 0.0;          // 三遍里最短的一遍
            std::array<double, (size_t) numPasses> passLoadPercent {};
            double bestProcessSeconds = 0.0;       // 最短那遍处理 measuredSeconds 音频花的秒数
            bool allFinite = true;
            std::uint64_t allocations = 0;         // 空跑 + 三遍里 processBlock 经 operator new 分配的总次数
        };

        BalanceModule::Params makeBalanceParams (float recover, float tame)
        {
            BalanceModule::Params p;
            p.recover = recover;
            p.tame = tame;
            p.brighten = 0.0f;
            p.speed = BalanceModule::Speed::mid;
            p.bypass = false;
            return p;
        }

        ColorModule::Params makeColorParams (float heat, float glue)
        {
            ColorModule::Params p;
            p.toneDb = 0.0f;
            p.glue = glue;
            p.heat = heat;
            p.mixPercent = 100.0f;
            p.outputDb = 0.0f;
            p.bypass = false;
            return p;
        }

        // 「中性偏开」：三个模块都在工作，但都不是极端档——Balance 60 / 60、Color Heat 50 Glue 50、
        // Limit Gain +9 dB。噪声峰值 −6 dBFS，Glue 50 先压掉 2 ～ 3 dB、Heat 50 再削一点峰，+6 dB 时
        // 峰值到不了 −1 dBTP 的 Ceiling（实测限幅 GR = 0）；+9 dB 才让限幅器也持续动作，日志里的 GR 读数可核对。
        CeladonChain::Params makeChainParams()
        {
            CeladonChain::Params p;
            p.balance = makeBalanceParams (60.0f, 60.0f);
            p.color = makeColorParams (50.0f, 50.0f);
            p.limit.gainDb = 9.0f;
            p.limit.ceilingDb = -1.0f;
            p.limit.releaseMs = 100.0f;
            p.limit.autoRelease = true;
            p.limit.lookaheadMs = 2.0f;
            p.limit.clip = 0.0f;
            p.limit.listen = false;
            p.limit.bypass = false;
            return p;
        }

        juce::dsp::ProcessSpec makeSpec()
        {
            return { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
        }

        bool blockIsFinite (const juce::AudioBuffer<float>& block)
        {
            for (int ch = 0; ch < block.getNumChannels(); ++ch)
            {
                const auto* data = block.getReadPointer (ch);

                for (int i = 0; i < block.getNumSamples(); ++i)
                    if (! std::isfinite (data[i]))
                        return false;
            }

            return true;
        }

        // 把 input 里 [startSample, startSample + numSamples) 按 512 一块送进 processBlock，
        // 只给 processBlock 本身计时（拷贝进块、检查有限值都在计时区外）。返回处理时间（秒）；
        // finite 累计每一块的输出是否全是有限值；allocations 累计 processBlock 本身经 operator new 分配的次数
        // （计数也只夹住 processBlock，块缓冲在循环外建好、setSize 不超过已分配的大小，不会算进来）。
        template <typename ProcessBlock>
        double timeSection (ProcessBlock&& processBlock, const Signal& input, int startSample, int numSamples,
                            bool& finite, std::uint64_t& allocations)
        {
            juce::AudioBuffer<float> block (numChannels, blockSize);
            juce::int64 ticks = 0;

            for (int start = startSample; start < startSample + numSamples; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, startSample + numSamples - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                const auto allocationsBefore = allocationCountOnThisThread();
                const auto before = juce::Time::getHighResolutionTicks();
                processBlock (block);
                ticks += juce::Time::getHighResolutionTicks() - before;
                allocations += allocationCountOnThisThread() - allocationsBefore;

                finite = finite && blockIsFinite (block);
            }

            return juce::Time::highResolutionTicksToSeconds (ticks);
        }

        // 先空跑 warmUpSeconds，再连续跑 numPasses 遍 measuredSeconds（状态跨遍延续，像一次不间断的播放）。
        // input 的长度 = (warmUpSeconds + measuredSeconds) × fs：每遍都用同一段 20 s 噪声，
        // 遍与遍之间只有 CPU 抖动的差别，取最短的一遍就是最接近真实成本的数字。
        template <typename ProcessBlock>
        Measurement measure (ProcessBlock&& processBlock, const Signal& input)
        {
            Measurement m;
            const auto warmUpSamples = (int) (warmUpSeconds * sampleRate);
            const auto measuredSamples = (int) (measuredSeconds * sampleRate);

            timeSection (processBlock, input, 0, warmUpSamples, m.allFinite, m.allocations);

            m.bestProcessSeconds = std::numeric_limits<double>::max();

            for (int pass = 0; pass < numPasses; ++pass)
            {
                const auto seconds = timeSection (processBlock, input, warmUpSamples, measuredSamples, m.allFinite, m.allocations);
                m.passLoadPercent[(size_t) pass] = 100.0 * seconds / measuredSeconds;
                m.bestProcessSeconds = juce::jmin (m.bestProcessSeconds, seconds);
            }

            m.bestLoadPercent = 100.0 * m.bestProcessSeconds / measuredSeconds;
            return m;
        }

        juce::String percent (double value)
        {
            return juce::String (value, 3) + "%";
        }

        juce::String describe (const Measurement& m)
        {
            // 每 512 样本一块的平均微秒数：与「一块 10.67 ms 的音频预算」对照直观。
            const auto numBlocks = measuredSeconds * sampleRate / blockSize;
            const auto microsecondsPerBlock = m.bestProcessSeconds * 1.0e6 / numBlocks;

            juce::String passes;

            for (size_t pass = 0; pass < m.passLoadPercent.size(); ++pass)
                passes += (pass == 0 ? "" : ", ") + percent (m.passLoadPercent[pass]);

            return "best " + percent (m.bestLoadPercent)
                 + " (" + juce::String (microsecondsPerBlock, 1) + " us / " + juce::String (blockSize)
                 + "-sample block; passes " + passes + ")";
        }

        //==============================================================================
        // v0.5 口径（研究 P4 R7）：每遍先 setup() 回到同一起点（每遍每块的工作完全相同），把 input 按 blockLength 送一遍；
        // 第 0 遍空跑不记。每块取各遍里最短的一次（去掉中断 / 抢占），再取所有块的均值与最坏块，都换算成「占块预算的 %」
        // （= 单核占用）。只给 process 本身计时。
        struct BlockLoad
        {
            double meanPercent = 0.0;
            double worstPercent = 0.0;
            double worstMicroseconds = 0.0;
            bool allFinite = true;
        };

        template <typename Setup, typename Process>
        BlockLoad measurePerBlock (const Signal& input, double rate, int blockLength, int passes, Setup&& setup, Process&& process)
        {
            const auto channels = (int) input.size();
            const auto numSamples = (int) input.front().size();
            const auto numBlocks = (numSamples + blockLength - 1) / blockLength;
            std::vector<juce::int64> best ((size_t) numBlocks, std::numeric_limits<juce::int64>::max());
            juce::AudioBuffer<float> block (channels, blockLength);
            BlockLoad load;

            for (int pass = 0; pass <= passes; ++pass)
            {
                setup();

                for (int b = 0; b < numBlocks; ++b)
                {
                    const auto start = b * blockLength;
                    const auto length = juce::jmin (blockLength, numSamples - start);
                    block.setSize (channels, length, false, false, true);

                    for (int ch = 0; ch < channels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                    const auto before = juce::Time::getHighResolutionTicks();
                    process (block);
                    const auto elapsed = juce::Time::getHighResolutionTicks() - before;

                    if (pass > 0)
                    {
                        best[(size_t) b] = juce::jmin (best[(size_t) b], elapsed);
                        load.allFinite = load.allFinite && blockIsFinite (block);
                    }
                }
            }

            const auto budgetSeconds = (double) blockLength / rate;
            double sumSeconds = 0.0, worstSeconds = 0.0;

            for (auto ticks : best)
            {
                const auto seconds = juce::Time::highResolutionTicksToSeconds (ticks);
                sumSeconds += seconds;
                worstSeconds = juce::jmax (worstSeconds, seconds);
            }

            load.meanPercent = 100.0 * sumSeconds / ((double) numBlocks * budgetSeconds);
            load.worstPercent = 100.0 * worstSeconds / budgetSeconds;
            load.worstMicroseconds = worstSeconds * 1.0e6;
            return load;
        }

        juce::String describe (const BlockLoad& load)
        {
            return "mean " + percent (load.meanPercent) + ", worst block " + percent (load.worstPercent)
                 + " (" + juce::String (load.worstMicroseconds, 1) + " us)";
        }

        // 梳状素材：粉噪（−24 dBFS RMS，Paul Kellet 7 极点近似）+ 每隔一段的中心频率上一个 −20 dBFS 正弦（0.45 fs 以下）。
        // Recover = Tame = 100 时突出的段被压、夹在中间的段被提，二十多段滤波器同时在工作。固定种子。
        Signal makeCombSignal (double rate, double seconds, juce::int64 seed)
        {
            const auto numSamples = (int) (seconds * rate);
            auto signal = makeSilence (numChannels, numSamples);
            const auto targetRms = (double) juce::Decibels::decibelsToGain (-24.0f);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                juce::Random random (seed + (juce::int64) ch * 7919);
                double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;
                auto& channel = signal[(size_t) ch];
                double sumOfSquares = 0.0;

                for (int i = -8192; i < numSamples; ++i)
                {
                    const auto white = (double) random.nextFloat() * 2.0 - 1.0;
                    b0 = 0.99886 * b0 + white * 0.0555179;
                    b1 = 0.99332 * b1 + white * 0.0750759;
                    b2 = 0.96900 * b2 + white * 0.1538520;
                    b3 = 0.86650 * b3 + white * 0.3104856;
                    b4 = 0.55000 * b4 + white * 0.5329522;
                    b5 = -0.7616 * b5 - white * 0.0168980;
                    const auto pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
                    b6 = white * 0.115926;

                    if (i >= 0)
                    {
                        channel[(size_t) i] = (float) pink;
                        sumOfSquares += pink * pink;
                    }
                }

                const auto gain = (float) (targetRms / std::sqrt (sumOfSquares / (double) numSamples));

                for (auto& sample : channel)
                    sample *= gain;
            }

            const auto amplitude = juce::Decibels::decibelsToGain (-20.0f) * juce::MathConstants<float>::sqrt2;

            for (int band = 0; band < BalanceModule::numBands; band += 2)
            {
                const auto frequency = 1000.0 * std::pow (2.0, (double) (band - 14) / 3.0);

                if (frequency >= 0.45 * rate)
                    continue;

                for (int i = 0; i < numSamples; ++i)
                {
                    const auto value = amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate + 0.1 * band);

                    for (auto& channel : signal)
                        channel[(size_t) i] += value;
                }
            }

            return signal;
        }

        // 测试线程临时换成 QoS user-interactive（macOS：落在性能核上），析构时恢复原来的 QoS。其他平台什么都不做。
        struct ScopedPerformanceQos
        {
           #if JUCE_MAC
            ScopedPerformanceQos()
            {
                restore = pthread_get_qos_class_np (pthread_self(), &previousClass, &previousPriority) == 0;
                pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
            }

            ~ScopedPerformanceQos()
            {
                if (restore)
                    pthread_set_qos_class_self_np (previousClass, previousPriority);
            }

            qos_class_t previousClass = QOS_CLASS_UNSPECIFIED;
            int previousPriority = 0;
            bool restore = false;
           #endif
        };

        bool containsNonFinite (const juce::AudioBuffer<float>& buffer) noexcept
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* data = buffer.getReadPointer (ch);

                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    if (! std::isfinite (data[i]))
                        return true;
            }

            return false;
        }
    }

    class BalancePerformanceTests  : public juce::UnitTest
    {
    public:
        BalancePerformanceTests() : juce::UnitTest ("BalancePerformance", "Celadon") {}

        void runTest() override
        {
            beginTest ("balance / color / chain CPU load on 20 s of noise @48k stereo, block 512");

            const auto totalSamples = (int) ((warmUpSeconds + measuredSeconds) * sampleRate);
            const auto input = makeNoise (numChannels, totalSamples, noiseAmplitude, noiseSeed);

            logMessage ("  timer resolution " + juce::String (juce::Time::getHighResolutionTicksPerSecond()) + " ticks/s; "
                        + juce::String (numPasses) + " passes x " + juce::String (measuredSeconds, 0) + " s each after "
                        + juce::String (warmUpSeconds, 0) + " s warm-up; load = process time / audio time");

            // 计时器自己的开销：两次 getHighResolutionTicks 之间只夹一次空调用（拷贝进块与有限值检查都在计时区外）。
            // 实测为 0.000%，说明每块 2 次计时对下面的数字没有可见影响；记进日志是为了让读数的人不用猜。
            const auto timerOverhead = measure ([] (juce::AudioBuffer<float>&) {}, input);
            logMessage ("  timer overhead (empty process): " + describe (timerOverhead));

            // 分配计数的对照：空处理必须是 0（说明计数区里只有 processBlock，测量框架自己不分配）；
            // 故意分配的处理必须 > 0（说明 TestMain.cpp 替换的 operator new 真的生效了，下面的「= 0」不是空转）。
            // 故意分配用显式调用 ::operator new：new 表达式与 std::allocator 的分配允许编译器省掉，显式函数调用不会。
            expectEquals ((juce::int64) timerOverhead.allocations, (juce::int64) 0, "measurement harness must not allocate");

            const auto allocatingProbe = measure ([] (juce::AudioBuffer<float>& block)
                                                  {
                                                      auto* memory = ::operator new (sizeof (float) * (size_t) block.getNumSamples());
                                                      ::operator delete (memory);
                                                  },
                                                  input);
            expect (allocatingProbe.allocations > 0, "allocation counter must see a deliberate ::operator new call");

            testBalance (input);
            testColor (input);
            testChain (input);

            // ---- v0.5 口径（研究 P4 R7）----
            [[maybe_unused]] const ScopedPerformanceQos performanceQos;   // 非 Mac 上是空类型（GCC 报 unused）

            testBalanceOnCombMaterial();
            testAnalysisSlicingHalvesTheSpike();
            testProcessorLayer();
            testAudioThreadBudget();
        }

    private:
        // Balance：Recover = Tame = 60、Mid、Brighten 0。断言输出有限、占用 < 25%；规格的 < 2% 只记录。
        void testBalance (const Signal& input)
        {
            BalanceModule balance;
            balance.setParams (makeBalanceParams (60.0f, 60.0f));
            balance.prepare (makeSpec());
            balance.setParams (makeBalanceParams (60.0f, 60.0f));

            const auto m = measure ([&] (juce::AudioBuffer<float>& block) { balance.process (block); }, input);

            logMessage ("  Balance (recover 60, tame 60, mid): " + describe (m));
            logMessage ("    spec reference: < " + percent (specLoadPercent) + " single core @48k; test limit: < "
                        + percent (balanceLoadLimitPercent));

            expect (m.allFinite, "balance output must be finite");
            expectEquals ((juce::int64) m.allocations, (juce::int64) 0, "balance process must not allocate");
            expect (m.bestLoadPercent < balanceLoadLimitPercent,
                    "balance CPU load " + percent (m.bestLoadPercent) + " exceeds the " + percent (balanceLoadLimitPercent)
                    + " sanity limit (spec target " + percent (specLoadPercent) + ")");

            // 顺带记一笔旁通时的成本：规格「零延迟旁通」——旁通只剩 20 ms 交叉淡化之后的直通，应该接近框架底噪。
            // 只记录不断言：它是给「Balance 开着到底多贵」做对照的。
            BalanceModule bypassed;
            auto bypassParams = makeBalanceParams (60.0f, 60.0f);
            bypassParams.bypass = true;
            bypassed.setParams (bypassParams);
            bypassed.prepare (makeSpec());
            bypassed.setParams (bypassParams);

            const auto mb = measure ([&] (juce::AudioBuffer<float>& block) { bypassed.process (block); }, input);
            logMessage ("  Balance bypassed (same settings): " + describe (mb));
            expect (mb.allFinite, "bypassed balance output must be finite");
            expectEquals ((juce::int64) mb.allocations, (juce::int64) 0, "bypassed balance process must not allocate");
        }

        // Color：Heat 50（8× 过采样的 Tube 全程在跑）、Glue 50、Tone 0、Mix 100、Output 0。只断言有限值。
        void testColor (const Signal& input)
        {
            ColorModule color;
            color.setParams (makeColorParams (50.0f, 50.0f));
            color.prepare (makeSpec());
            color.setParams (makeColorParams (50.0f, 50.0f));

            const auto m = measure ([&] (juce::AudioBuffer<float>& block) { color.process (block); }, input);

            logMessage ("  Color (heat 50, glue 50): " + describe (m));
            expect (m.allFinite, "color output must be finite");
            expectEquals ((juce::int64) m.allocations, (juce::int64) 0, "color process must not allocate");
        }

        // 整条链（Balance 60 / 60 → Color Heat 50 Glue 50 → Limit Gain +9 dB Ceiling −1 dBTP）。只断言有限值。
        void testChain (const Signal& input)
        {
            CeladonChain chain;
            chain.setParams (makeChainParams(), false);
            chain.prepare (makeSpec());
            chain.setParams (makeChainParams(), false);

            const auto m = measure ([&] (juce::AudioBuffer<float>& block) { chain.process (block); }, input);

            logMessage ("  CeladonChain (balance 60/60, color heat 50 glue 50, limit gain +9 dB): " + describe (m)
                        + "; latency " + juce::String (chain.getLatencySamples()) + " samples, limit GR "
                        + juce::String (chain.getAndResetLimitGainReductionDb(), 2) + " dB, glue GR "
                        + juce::String (chain.getAndResetGlueGainReductionDb(), 2) + " dB");
            expect (m.allFinite, "chain output must be finite");
            expectEquals ((juce::int64) m.allocations, (juce::int64) 0, "chain process must not allocate");
            expect (m.bestLoadPercent < audioThreadBudgetPercent,
                    "chain audio-thread budget " + percent (m.bestLoadPercent) + " exceeds "
                    + percent (audioThreadBudgetPercent));
        }

        //==============================================================================
        // v0.5（研究 P4 R3 / R7）：梳状素材、Recover = Tame = 100、Mid，48k 块 512，每块多遍取最短再取均值。
        // 这是 Balance 最贵的工况（二十多段滤波器同时在工作）；白噪下只有约 5 段在工作，量不出滤波的真实成本。
        // 断言：这时确实有 ≥ 15 段在工作（素材有效），均值 ≤ combLoadLimitPercent（门槛来由见常量处），输出有限、不分配。
        // 另记一笔「像 GLAZE 100」的 Recover = Tame = 40，只记录。
        void testBalanceOnCombMaterial()
        {
            beginTest ("balance on comb material (20+ bands working): per-block best-of-5, mean and worst block");

            const auto input = makeCombSignal (sampleRate, 4.0, 77);

            // 先单独跑一遍：数一数有多少段在工作、有没有分配。
            {
                BalanceModule balance;
                balance.setParams (makeBalanceParams (100.0f, 100.0f));
                balance.prepare (makeSpec());
                balance.setParams (makeBalanceParams (100.0f, 100.0f));

                juce::AudioBuffer<float> block (numChannels, blockSize);
                std::uint64_t allocations = 0;
                int mostActive = 0;

                for (int start = 0; start + blockSize <= (int) input.front().size(); start += blockSize)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, blockSize);

                    const auto before = allocationCountOnThisThread();
                    balance.process (block);
                    allocations += allocationCountOnThisThread() - before;
                    mostActive = juce::jmax (mostActive, balance.getActiveFilterCount());
                }

                logMessage ("  comb material: up to " + juce::String (mostActive) + " of 27 bands active (recover = tame = 100)");
                expect (mostActive >= minimumActiveBandsOnComb, "comb material must keep >= " + juce::String (minimumActiveBandsOnComb)
                                                                    + " bands working, got " + juce::String (mostActive));
                expectEquals ((juce::int64) allocations, (juce::int64) 0, "balance on comb material must not allocate");
            }

            BalanceModule balance;
            auto setUp = [&] (float amount)
            {
                return [&balance, amount]
                {
                    balance.setParams (makeBalanceParams (amount, amount));
                    balance.prepare (makeSpec());
                    balance.setParams (makeBalanceParams (amount, amount));
                };
            };
            auto process = [&] (juce::AudioBuffer<float>& block) { balance.process (block); };

            const auto full = measurePerBlock (input, sampleRate, blockSize, 5, setUp (100.0f), process);
            const auto glaze = measurePerBlock (input, sampleRate, blockSize, 5, setUp (40.0f), process);

            // 两声道打包的直接证据：同一素材只取左声道、按单声道处理（单声道走标量路径）。打包后立体声的成本应接近单声道，
            // 而不是两倍；同一次运行里两者相比，与机器快慢、供电状态无关（门槛来由见常量处）。
            const Signal monoInput { input.front() };
            BalanceModule monoBalance;
            const auto mono = measurePerBlock (monoInput, sampleRate, blockSize, 5,
                                               [&]
                                               {
                                                   monoBalance.setParams (makeBalanceParams (100.0f, 100.0f));
                                                   monoBalance.prepare ({ sampleRate, (juce::uint32) blockSize, 1 });
                                                   monoBalance.setParams (makeBalanceParams (100.0f, 100.0f));
                                               },
                                               [&] (juce::AudioBuffer<float>& block) { monoBalance.process (block); });
            const auto stereoOverMono = full.meanPercent / juce::jmax (1.0e-9, mono.meanPercent);

            logMessage ("  Balance comb, recover = tame = 100 @48k block 512: " + describe (full));
            logMessage ("  Balance comb, recover = tame = 40 (= GLAZE 100 offsets) @48k block 512: " + describe (glaze));
            logMessage ("  Balance comb, recover = tame = 100, mono (left channel only): " + describe (mono)
                        + " | stereo / mono " + juce::String (stereoOverMono, 2));

            expect (full.allFinite && glaze.allFinite && mono.allFinite, "balance output must be finite");
            expect (full.meanPercent <= combLoadLimitPercent,
                    "balance on comb material " + percent (full.meanPercent) + " exceeds " + percent (combLoadLimitPercent));

           #if defined (__ARM_NEON) && defined (__aarch64__)
            expect (stereoOverMono <= stereoOverMonoLimit,
                    "stereo balance costs " + juce::String (stereoOverMono, 2) + " x mono (limit "
                    + juce::String (stereoOverMonoLimit, 2) + "): the packed two-channel path is not doing its job");
           #endif
        }

        // v0.5（研究 P4 R5）：192k 块 32（8192 点 FFT、hop 2048 样本）下，每 hop 一次的分析是 Balance 最坏块的来源。
        // 拆成三片后最坏块应不超过一口气做完（测试开关）的 65%——两次 FFT 分到两块里，实测约 49%。同一次运行里对比两种
        // 做法，与机器快慢、供电状态无关。Recover = Tame = 0（默认参数：只剩分析）与 40 各量一次；另记 44.1k 块 1。
        // T-032 R2-07 起三片落在 hop 后第 0 / 128 / 256 个样本（192k；间隔随 hop 缩放），块 32 下仍各落一块，比值口径不变。
        void testAnalysisSlicingHalvesTheSpike()
        {
            beginTest ("analysis slicing: worst block at 192 kHz / 32 samples <= 65% of the one-shot analysis");

            struct Case { double rate; int block; float amount; bool assert; };
            const Case cases[] = { { 192000.0, 32, 0.0f, true }, { 192000.0, 32, 40.0f, true },
                                   { 192000.0, 64, 0.0f, false }, { 44100.0, 1, 0.0f, false } };

            for (const auto& c : cases)
            {
                const auto input = makeCombSignal (c.rate, c.block == 1 ? 1.0 : 2.0, 91);

                BlockLoad loads[2];

                for (int slicing = 0; slicing < 2; ++slicing)
                {
                    BalanceModule balance;
                    loads[slicing] = measurePerBlock (input, c.rate, c.block, c.block == 1 ? 3 : 5,
                                                      [&]
                                                      {
                                                          balance.setParams (makeBalanceParams (c.amount, c.amount));
                                                          balance.prepare ({ c.rate, 512, (juce::uint32) numChannels });
                                                          balance.setParams (makeBalanceParams (c.amount, c.amount));
                                                          balance.setAnalysisSlicingForTesting (slicing == 1);
                                                      },
                                                      [&] (juce::AudioBuffer<float>& block) { balance.process (block); });
                }

                const auto ratio = loads[1].worstMicroseconds / juce::jmax (1.0e-9, loads[0].worstMicroseconds);
                const auto tag = "@" + juce::String (c.rate, 0) + " block " + juce::String (c.block)
                               + " recover = tame = " + juce::String (c.amount, 0);

                logMessage ("  " + tag + ": one-shot " + describe (loads[0]) + " | sliced " + describe (loads[1])
                            + " | worst-block ratio " + juce::String (ratio, 2));

                expect (loads[0].allFinite && loads[1].allFinite, "balance output must be finite " + tag);

                if (c.assert)
                    expect (ratio <= slicedWorstBlockRatioLimit, "sliced worst block is " + juce::String (ratio, 2)
                                                                     + " of the one-shot analysis (limit " + juce::String (slicedWorstBlockRatioLimit, 2)
                                                                     + ") " + tag);
            }
        }

        // 处理器层（与 PluginProcessor::processChain 同样的工作：输入 NaN 扫描 → setParams → 链；v0.6 起输入表、GLAZE 编排器、
        // 输出 NaN 扫描与输出表都在链的 process 里），默认参数（各量 0、GLAZE 0）与「典型」（GLAZE 30、Balance 30 / 30、Heat 30、
        // Glue 30、Limit Gain +6，同整插件基准），48k 块 512 的均值与块 64 的最坏块。整插件的验收用 scripts/perf-local.sh（ADR-007）；
        // 这里是给无头测试留一个能及早发现回归的对照数字，只对块 512 的均值设宽松上限（见常量处）。
        void testProcessorLayer()
        {
            beginTest ("processor layer (chain + two meters + NaN scans): per-block best-of-5 @48k");

            const auto input = makeCombSignal (sampleRate, 4.0, 101);

            struct Scenario { const char* name; CeladonChain::Params params; };
            CeladonChain::Params typical;
            typical.glazePercent = 30.0f;
            typical.balance = makeBalanceParams (30.0f, 30.0f);
            typical.color = makeColorParams (30.0f, 30.0f);
            typical.limit.gainDb = 6.0f;

            const Scenario scenarios[] = { { "default", CeladonChain::Params {} }, { "typical", typical } };

            for (const auto& scenario : scenarios)
             for (auto blockLength : { 512, 64 })
            {
                CeladonChain chain;
                const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockLength, (juce::uint32) numChannels };

                const auto load = measurePerBlock (input, sampleRate, blockLength, 5,
                                                   [&]
                                                   {
                                                       chain.setParams (scenario.params, false);
                                                       chain.prepare (spec);
                                                   },
                                                   [&] (juce::AudioBuffer<float>& block)
                                                   {
                                                       juce::ScopedNoDenormals noDenormals;

                                                       if (containsNonFinite (block))
                                                           block.clear();

                                                       chain.setParams (scenario.params, false);
                                                       chain.process (block);   // 输入表 → 编排器 → 三模块 → 输出 NaN 扫描 → 输出表
                                                   });

                logMessage ("  processor layer, " + juce::String (scenario.name) + " @48k block " + juce::String (blockLength)
                            + ": " + describe (load));
                expect (load.allFinite, "processor layer output must be finite");

                if (blockLength == 512)
                    expect (load.meanPercent < processorLayerLoadLimitPercent,
                            "processor layer (" + juce::String (scenario.name) + ") " + percent (load.meanPercent)
                            + " exceeds the " + percent (processorLayerLoadLimitPercent) + " sanity limit");

                expect (load.meanPercent < audioThreadBudgetPercent,
                        "processor layer (" + juce::String (scenario.name) + ") block " + juce::String (blockLength)
                        + " audio-thread budget " + percent (load.meanPercent) + " exceeds "
                        + percent (audioThreadBudgetPercent));
            }
        }

        // 48 / 96 kHz、块 64：母带常用的小缓冲。开着 Balance、Glue、Heat 和 Limit，均值必须低于 15%。
        // 192 kHz 只记录。块 64 的单块尖峰含 FFT 落点，不拿最坏块跟 15% 比。
        void testAudioThreadBudget()
        {
            beginTest ("engaged chain stays under 15% of one audio thread at 48 and 96 kHz, block 64");

            CeladonChain::Params engaged;
            engaged.balance = makeBalanceParams (60.0f, 60.0f);
            engaged.color = makeColorParams (50.0f, 100.0f);
            engaged.limit.gainDb = 6.0f;
            engaged.limit.ceilingDb = -1.0f;
            engaged.limit.autoRelease = true;

            for (auto rate : { 48000.0, 96000.0, 192000.0 })
            {
                const auto input = makeCombSignal (rate, 2.0, 20260928);
                CeladonChain chain;
                constexpr int blockLength = 64;

                const auto load = measurePerBlock (input, rate, blockLength, 3,
                                                   [&]
                                                   {
                                                       chain.setParams (engaged, false);
                                                       chain.prepare ({ rate, (juce::uint32) blockLength, (juce::uint32) numChannels });
                                                       chain.setParams (engaged, false);
                                                   },
                                                   [&] (juce::AudioBuffer<float>& block)
                                                   {
                                                       juce::ScopedNoDenormals noDenormals;
                                                       chain.process (block);
                                                   });

                logMessage ("  engaged chain @" + juce::String (rate, 0) + " block " + juce::String (blockLength)
                            + ": " + describe (load));
                expect (load.allFinite, "engaged chain output must be finite");

                // 本机 192 kHz 块 64 的均值大约 13%。按与 M2 Pro 历史读数的速度比，对应到 M2 上大约 5%。
                // 48 / 96 kHz 在本机必须低于 15%。
                if (rate <= 96000.0)
                    expect (load.meanPercent < audioThreadBudgetPercent,
                            "@" + juce::String (rate, 0) + " mean " + percent (load.meanPercent)
                            + " exceeds the " + percent (audioThreadBudgetPercent) + " audio-thread budget");
            }
        }
    };

    static BalancePerformanceTests balancePerformanceTests;
}
