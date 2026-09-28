// 整插件性能基准（v0.5 起的性能验收工具，ADR-007；源自 v0.5 研究 P1）：无界面直接跑 CeladonAudioProcessor::processBlock（含两块表、参数读取、GLAZE 编排器、整链），
// 另有按模块拆分计时、界面绘制计时。单线程、固定种子素材、先空跑再测、取多遍最短。
//
// 用法：
//   celadon_bench grid    <scenario|all> <srs> <bss> <warmSec> <passSec> <passes>
//   celadon_bench bassgrid <scenario|all> <srs> <bss> <warmSec> <passSec> <passes> （持续低频侧声道压力素材）
//   celadon_bench modules <scenario|all> <srs> <bss> <warmSec> <passSec> <passes>
//   celadon_bench gui
//   celadon_bench rendercheck  （真实处理器实时/离线及执行快照恢复验收）
//   celadon_bench statecheck   （真实处理器默认启动、旧工程缺参、显式零值与状态往返验收）
//   celadon_bench ecore   <scenario|all> <srs> <bss> <warmSec> <passSec> <passes>   （后台 QoS：只在能效核上跑）
//   srs / bss 用逗号分隔，例如 44100,48000,96000,192000  32,64,128,256,512,1024
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/BalanceModule.h"
#include "dsp/ColorModule.h"
#include "dsp/LimiterModule.h"
#include "dsp/LoudnessMeter.h"
#include "dsp/GlazeController.h"
#include "dsp/TruePeak.h"
#include "dsp/Saturator.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
    double nsPerTick = 1.0;
    bool useSustainedBassFixture = false;

    inline uint64_t now() noexcept { return mach_absolute_time(); }
    inline double toNs (uint64_t ticks) noexcept { return (double) ticks * nsPerTick; }

    struct Scenario
    {
        const char* name;
        std::vector<std::pair<const char*, float>> values;
    };

    const std::vector<Scenario> allScenarios
    {
        { "default", {} },
        { "manual", { { "glaze_amount", 0.0f }, { "limit_gain", 9.0f } } },
        { "auto50", { { "glaze_amount", 50.0f } } },
        { "auto100", { { "glaze_amount", 100.0f } } },
        { "typical", { { "glaze_amount", 30.0f }, { "balance_recover", 30.0f }, { "balance_tame", 30.0f },
                       { "color_heat", 30.0f }, { "color_glue", 30.0f }, { "limit_gain", 6.0f } } },
    };

    std::vector<const Scenario*> pickScenarios (const juce::String& which)
    {
        std::vector<const Scenario*> out;
        for (auto& s : allScenarios)
            if (which == "all" || which == s.name)
                out.push_back (&s);
        return out;
    }

    double medianOf (std::vector<double> v)
    {
        if (v.empty()) return 0.0;
        const auto mid = v.begin() + (std::ptrdiff_t) (v.size() / 2);
        std::nth_element (v.begin(), mid, v.end());
        return *mid;
    }

    constexpr long long structuralPeriodSamples = 65536;

    double structuralWorstOf (const std::vector<std::vector<double>>& byPhase, size_t minCount = 1)
    {
        double worst = 0;
        for (auto& v : byPhase)
            if (! v.empty() && v.size() >= minCount)
                worst = juce::jmax (worst, medianOf (v));
        return worst;
    }

    std::vector<double> parseList (const juce::String& s)
    {
        std::vector<double> out;
        for (auto& token : juce::StringArray::fromTokens (s, ",", ""))
            out.push_back (token.getDoubleValue());
        return out;
    }

    // 母带式测试素材：粉噪底 + 底鼓 / 军鼓 / 镲的瞬态 + 三个持续音，峰值约 −1 dBFS，左右部分相关。固定种子。
    juce::AudioBuffer<float> makeSignal (double sr, double seconds)
    {
        const auto n = (int) std::ceil (sr * seconds);
        juce::AudioBuffer<float> sig (2, n);
        if (useSustainedBassFixture)
        {
            for (int i = 0; i < n; ++i)
            {
                const auto t = (double) i / sr;
                const auto bass = 0.72 * std::sin (juce::MathConstants<double>::twoPi * 55.0 * t);
                const auto treble = 0.18 * std::sin (juce::MathConstants<double>::twoPi * 6000.0 * t);
                sig.setSample (0, i, (float) (bass + treble));
                sig.setSample (1, i, (float) (-bass + treble));
            }
            return sig;
        }
        juce::Random rng (20260925);

        double b[2][7] = {};
        auto pink = [&] (int ch, float white)
        {
            auto* s = b[ch];
            s[0] = 0.99886 * s[0] + white * 0.0555179;
            s[1] = 0.99332 * s[1] + white * 0.0750759;
            s[2] = 0.96900 * s[2] + white * 0.1538520;
            s[3] = 0.86650 * s[3] + white * 0.3104856;
            s[4] = 0.55000 * s[4] + white * 0.5329522;
            s[5] = -0.7616 * s[5] - white * 0.0168980;
            const auto out = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + white * 0.5362;
            s[6] = white * 0.115926;
            return out * 0.11;
        };

        const auto twoPi = juce::MathConstants<double>::twoPi;
        float prevWhite[2] = {};

        for (int i = 0; i < n; ++i)
        {
            const auto t = (double) i / sr;
            const auto common = rng.nextFloat() * 2.0f - 1.0f;

            // 底鼓：每 0.5 s，55 Hz 衰减正弦（τ 80 ms）
            const auto kickT = std::fmod (t, 0.5);
            const auto kick = 0.55 * std::sin (twoPi * 55.0 * kickT) * std::exp (-kickT / 0.08);
            // 军鼓：每 1 s（偏 0.25 s），噪声爆发（τ 30 ms）
            const auto snareT = std::fmod (t + 0.75, 1.0);
            const auto snareEnv = 0.30 * std::exp (-snareT / 0.03);
            // 镲：每 0.125 s，一阶差分白噪（τ 10 ms）
            const auto hatT = std::fmod (t, 0.125);
            const auto hatEnv = 0.12 * std::exp (-hatT / 0.01);
            // 持续音
            const auto pad = 0.05 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 330.0 * t + 0.3)
                                     + std::sin (twoPi * 440.0 * t + 1.1));

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto own = rng.nextFloat() * 2.0f - 1.0f;
                const auto white = 0.8f * common + 0.2f * own;
                const auto p = pink (ch, white);
                const auto hat = (double) (white - prevWhite[ch]) * 0.5;
                prevWhite[ch] = white;
                const auto v = p + kick + snareEnv * (double) white + hatEnv * hat + pad * (ch == 0 ? 1.0 : 0.9);
                sig.setSample (ch, i, (float) juce::jlimit (-0.99, 0.99, v));
            }
        }

        return sig;
    }

    struct SignalCache
    {
        const juce::AudioBuffer<float>& get (double sr)
        {
            for (auto& e : entries)
                if (e.first == sr)
                    return *e.second;
            entries.push_back ({ sr, std::make_unique<juce::AudioBuffer<float>> (makeSignal (sr, 8.0)) });
            return *entries.back().second;
        }
        std::vector<std::pair<double, std::unique_ptr<juce::AudioBuffer<float>>>> entries;
    };

    SignalCache signals;

    // 冷缓存模式（环境变量 COLD_MB）：每个计时块之前把一大块内存逐缓存行改写一遍，模拟 DAW 里两次回调之间
    // 别的轨道 / 插件把 L1 / L2 冲掉的情况。不计时。
    std::vector<unsigned char> coldBuffer;
    void thrashCaches() noexcept
    {
        auto* p = coldBuffer.data();
        for (size_t i = 0, n = coldBuffer.size(); i < n; i += 64)
            p[i] = (unsigned char) (p[i] + 1);
    }

    struct Feeder
    {
        const juce::AudioBuffer<float>& sig;
        int pos = 0;

        void fill (juce::AudioBuffer<float>& buf)
        {
            const auto n = buf.getNumSamples();
            const auto total = sig.getNumSamples();
            for (int done = 0; done < n;)
            {
                const auto chunk = juce::jmin (n - done, total - pos);
                for (int ch = 0; ch < 2; ++ch)
                    buf.copyFrom (ch, done, sig, ch, pos, chunk);
                done += chunk;
                pos += chunk;
                if (pos >= total)
                    pos = 0;
            }
        }
    };

    void applyScenario (CeladonAudioProcessor& p, const Scenario& sc)
    {
        for (auto& [id, value] : sc.values)
        {
            auto* param = p.getParameters().getParameter (id);
            jassert (param != nullptr);
            if (param == nullptr) { std::fprintf (stderr, "missing param %s\n", id); std::exit (3); }
            param->setValueNotifyingHost (param->convertTo0to1 (value));
        }
    }

    celadon::CeladonChain::Params chainParamsFor (const Scenario& sc)
    {
        celadon::CeladonChain::Params p;
        for (auto& [id, value] : sc.values)
        {
            const juce::String s (id);
            if (s == "glaze_amount")         p.glazePercent = value;
            else if (s == "balance_recover") p.balance.recover = value;
            else if (s == "balance_tame")    p.balance.tame = value;
            else if (s == "color_heat")      p.color.heat = value;
            else if (s == "color_glue")      p.color.glue = value;
            else if (s == "limit_gain")      p.limit.gainDb = value;
            else { std::fprintf (stderr, "unmapped %s\n", id); std::exit (3); }
        }
        return p;
    }

    struct PassStats
    {
        double sumNs = 0, maxNs = 0, p999Ns = 0, p99Ns = 0;
        int blocks = 0;
    };

    PassStats summarise (std::vector<double>& t)
    {
        PassStats s;
        s.blocks = (int) t.size();
        s.sumNs = std::accumulate (t.begin(), t.end(), 0.0);
        s.maxNs = *std::max_element (t.begin(), t.end());
        std::vector<double> sorted (t);
        std::sort (sorted.begin(), sorted.end());
        s.p999Ns = sorted[(size_t) juce::jmin ((int) sorted.size() - 1, (int) std::floor (0.999 * (double) sorted.size()))];
        s.p99Ns  = sorted[(size_t) juce::jmin ((int) sorted.size() - 1, (int) std::floor (0.99 * (double) sorted.size()))];
        return s;
    }

    //==============================================================================
    void runGrid (const juce::String& label, const std::vector<const Scenario*>& scs, const std::vector<double>& srs,
                  const std::vector<double>& bss, double warmSec, double passSec, int passes)
    {
        for (auto* sc : scs)
            for (auto sr : srs)
                for (auto bsD : bss)
                {
                    const auto bs = (int) bsD;
                    const auto& sig = signals.get (sr);

                    auto proc = std::make_unique<CeladonAudioProcessor>();
                    proc->setPlayConfigDetails (2, 2, sr, bs);
                    applyScenario (*proc, *sc);

                    const auto tp0 = now();
                    proc->prepareToPlay (sr, bs);
                    const auto prepareMs = toNs (now() - tp0) * 1.0e-6;

                    juce::AudioBuffer<float> buf (2, bs);
                    juce::MidiBuffer midi;
                    Feeder feeder { sig };

                    // Balance 的分析 hop（与 BalanceModule::prepare 同一算法）：按「块在 hop 里的相位」分组取中位数，
                    // 周期性的结构尖峰（每 hop 一次的 FFT 分析）留得下来，随机的系统抖动被中位数滤掉。
                    int order = 11;
                    while ((double) (1 << (order - 11)) * 48000.0 < sr)
                        ++order;
                    const long long hop = (1 << order) / 4;
                    const int numPhases = bs >= hop ? 1 : (int) (hop / bs);
                    const int numLongPhases = bs >= structuralPeriodSamples ? 1 : (int) (structuralPeriodSamples / bs);
                    long long samplePos = 0;

                    const auto warmBlocks = (int) std::ceil (warmSec * sr / bs);
                    for (int i = 0; i < warmBlocks; ++i)
                    {
                        feeder.fill (buf);
                        proc->processBlock (buf, midi);
                        samplePos += bs;
                    }

                    const auto warmStatus = proc->getGlazeStatus();
                    std::printf ("WARM,%s,%g,%d,seconds=%.1f,profileReady=%d,calibrationValid=%d,phase=%d\n",
                                 sc->name, sr, bs, warmSec, (int) warmStatus.profileReady,
                                 (int) warmStatus.calibrationValid, (int) warmStatus.phase);

                    const auto blocks = juce::jmax (8, (int) std::ceil (passSec * sr / bs));
                    std::vector<double> times ((size_t) blocks);
                    std::vector<std::vector<double>> byPhase ((size_t) numPhases);
                    std::vector<std::vector<double>> byLongPhase ((size_t) numLongPhases);   // 按 65536 样本的相位（见 structuralPeriodSamples）
                    const auto blockNs = 1.0e9 * bs / sr;

                    double bestAvg = 1e300, bestWorst = 1e300, bestP999 = 1e300, bestP99 = 1e300, worstOfAll = 0;
                    double checksum = 0;

                    for (int pass = 0; pass < passes; ++pass)
                    {
                        for (int i = 0; i < blocks; ++i)
                        {
                            feeder.fill (buf);
                            if (! coldBuffer.empty())
                                thrashCaches();
                            const auto t0 = now();
                            proc->processBlock (buf, midi);
                            times[(size_t) i] = toNs (now() - t0);
                            byPhase[(size_t) ((samplePos % hop) / bs % numPhases)].push_back (times[(size_t) i]);
                            byLongPhase[(size_t) ((samplePos % structuralPeriodSamples) / bs % numLongPhases)].push_back (times[(size_t) i]);
                            samplePos += bs;
                            checksum += (double) std::abs (buf.getSample (0, bs / 2));
                        }

                        auto s = summarise (times);
                        bestAvg   = juce::jmin (bestAvg, 100.0 * s.sumNs / (blocks * blockNs));
                        bestWorst = juce::jmin (bestWorst, 100.0 * s.maxNs / blockNs);
                        bestP999  = juce::jmin (bestP999, 100.0 * s.p999Ns / blockNs);
                        bestP99   = juce::jmin (bestP99, 100.0 * s.p99Ns / blockNs);
                        worstOfAll = juce::jmax (worstOfAll, 100.0 * s.maxNs / blockNs);
                    }

                    // 结构性最坏块：各相位组中位数的最大值；结构性典型块：各相位组中位数的最小值。
                    double structWorst = 0, structTypical = 1e300;
                    for (auto& v : byPhase)
                    {
                        if (v.empty()) continue;
                        std::sort (v.begin(), v.end());
                        const auto med = v[v.size() / 2];
                        structWorst = juce::jmax (structWorst, med);
                        structTypical = juce::jmin (structTypical, med);
                    }

                    std::printf ("GRID,%s,%s,%g,%d,avg%%=%.3f,worst%%=%.3f,p99.9%%=%.3f,p99%%=%.3f,maxAllPasses%%=%.3f,structWorst%%=%.3f,structTypical%%=%.3f,structWorst64k%%=%.3f,prepare_ms=%.3f,latency=%d,nonFinite=%d,blocks=%d,chk=%.6g\n",
                                 label.toRawUTF8(), sc->name, sr, bs, bestAvg, bestWorst, bestP999, bestP99, worstOfAll,
                                 100.0 * structWorst / blockNs, 100.0 * structTypical / blockNs,
                                 100.0 * structuralWorstOf (byLongPhase) / blockNs, prepareMs,
                                 proc->getLatencySamples(), proc->getNonFiniteBlockCount(), blocks, checksum);
                    std::fflush (stdout);
                    proc->releaseResources();
                }
    }

    //==============================================================================
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

    // 与 PluginProcessor::processChain 同样的工作，按阶段计时：表 → 参数 / GLAZE → Balance → Color → Limit → 表 → GLAZE 块末。
    // v0.6：GLAZE 映射换成编排器（与 CeladonChain 同样的接线与顺序）：「params+glaze」= beginBlock + 三个模块 setParams +
    // 接上一块的 Balance 帧（analyzeSpectrum，必须在 IN 表处理本块之前）+ 入口分析（analyzeInput，在 IN 表之后，故计时里 IN 表夹在中间，
    // 两段加在一起记进这一列），新增「glazeEnd」= 拼模块统计 + endBlock（校准、保护层、状态机、状态发布）。IN 表开着响度计量（与插件一致）。
    void runModules (const juce::String& label, const std::vector<const Scenario*>& scs, const std::vector<double>& srs,
                     const std::vector<double>& bss, double warmSec, double passSec, int passes)
    {
        enum { stGuardIn, stMeterIn, stParams, stBalance, stColor, stLimit, stGuardOut, stMeterOut, stGlazeEnd, numStages };
        const char* names[] = { "nanGuardIn", "meterIn", "params+glaze", "balance", "color", "limit", "nanGuardOut", "meterOut", "glazeEnd" };

        for (auto* sc : scs)
            for (auto sr : srs)
                for (auto bsD : bss)
                {
                    const auto bs = (int) bsD;
                    const auto& sig = signals.get (sr);
                    const juce::dsp::ProcessSpec spec { sr, (juce::uint32) bs, 2 };

                    auto balance = std::make_unique<celadon::BalanceModule>();
                    auto color   = std::make_unique<celadon::ColorModule>();
                    auto limiter = std::make_unique<celadon::LimiterModule>();
                    auto meterIn = std::make_unique<celadon::LoudnessMeter>();
                    auto meterOut = std::make_unique<celadon::LoudnessMeter>();
                    auto controller = std::make_unique<celadon::glaze::GlazeController>();

                    const auto user = chainParamsFor (*sc);
                    auto setAll = [&]
                    {
                        const auto& eff = controller->beginBlock (user, false);
                        balance->setParams (eff.balance);
                        color->setParams (eff.color);
                        limiter->setParams (eff.limit);
                    };

                    setAll();
                    balance->prepare (spec); color->prepare (spec); limiter->prepare (spec);
                    meterIn->prepare (spec); meterOut->prepare (spec);
                    controller->prepare (spec);

                    juce::AudioBuffer<float> buf (2, bs);
                    Feeder feeder { sig };

                    const auto blockNs = 1.0e9 * bs / sr;
                    const auto warmBlocks = (int) std::ceil (warmSec * sr / bs);
                    const auto blocks = juce::jmax (8, (int) std::ceil (passSec * sr / bs));

                    std::vector<double> stageSum ((size_t) numStages), stageMax ((size_t) numStages);
                    double bestTotal = 1e300;
                    std::vector<double> bestStageSum ((size_t) numStages), bestStageMax ((size_t) numStages, 1e300);

                    for (int pass = -1; pass < passes; ++pass)
                    {
                        std::fill (stageSum.begin(), stageSum.end(), 0.0);
                        std::fill (stageMax.begin(), stageMax.end(), 0.0);
                        const auto nBlocks = pass < 0 ? warmBlocks : blocks;

                        for (int i = 0; i < nBlocks; ++i)
                        {
                            feeder.fill (buf);
                            uint64_t t[numStages + 1];
                            juce::ScopedNoDenormals noDenormals;
                            t[0] = now();
                            if (containsNonFinite (buf)) buf.clear();
                            t[1] = now();
                            setAll();
                            controller->analyzeSpectrum (*balance);
                            const auto glazeBefore = now();
                            meterIn->process (buf);
                            const auto meterInEnd = now();
                            controller->analyzeInput (buf, *meterIn);
                            t[3] = now();
                            // IN 表那一段单独记进 meterIn，其余（参数 + 编排器的两次调用）记进 params+glaze。
                            t[2] = t[1] + (meterInEnd - glazeBefore);
                            balance->process (buf);
                            t[4] = now();
                            color->process (buf);
                            t[5] = now();
                            limiter->process (buf);
                            t[6] = now();
                            if (containsNonFinite (buf)) buf.clear();
                            t[7] = now();
                            meterOut->process (buf);
                            t[8] = now();
                            {
                                celadon::glaze::ModuleStats stats;
                                stats.numSamples = buf.getNumSamples();
                                stats.glueGrMeanDb = color->getBlockStats().glueGrMeanDb;
                                stats.glueGrMaxDb = color->getBlockStats().glueGrMaxDb;
                                stats.heatDrive = color->getBlockStats().heatDrive;
                                stats.limitGrMeanDb = limiter->getBlockStats().grMeanDb;
                                stats.limitGrMaxDb = limiter->getBlockStats().grMaxDb;
                                stats.clipActiveFraction = limiter->getBlockStats().clipActiveFraction;
                                stats.clipDepthMaxDb = limiter->getBlockStats().clipDepthMaxDb;
                                stats.outputTruePeakDbtp = juce::Decibels::gainToDecibels (meterOut->getLastBlockTruePeakLinear(), -100.0f);
                                controller->endBlock (*balance, *meterIn, *meterOut, stats,
                                                      balance->getLatencySamples() + color->getLatencySamples() + limiter->getLatencySamples());
                            }
                            t[9] = now();

                            for (int s = 0; s < numStages; ++s)
                            {
                                const auto d = toNs (t[s + 1] - t[s]);
                                stageSum[(size_t) s] += d;
                                stageMax[(size_t) s] = juce::jmax (stageMax[(size_t) s], d);
                            }
                        }

                        if (pass < 0)
                            continue;

                        const auto total = std::accumulate (stageSum.begin(), stageSum.end(), 0.0);
                        if (total < bestTotal)
                        {
                            bestTotal = total;
                            bestStageSum = stageSum;
                        }
                        for (int s = 0; s < numStages; ++s)
                            bestStageMax[(size_t) s] = juce::jmin (bestStageMax[(size_t) s], stageMax[(size_t) s]);
                    }

                    std::printf ("MODULES,%s,%s,%g,%d,total_avg%%=%.3f", label.toRawUTF8(), sc->name, sr, bs,
                                 100.0 * bestTotal / (blocks * blockNs));
                    for (int s = 0; s < numStages; ++s)
                        std::printf (",%s_avg%%=%.3f,%s_share=%.1f%%,%s_worst%%=%.3f", names[s],
                                     100.0 * bestStageSum[(size_t) s] / (blocks * blockNs), names[s],
                                     100.0 * bestStageSum[(size_t) s] / bestTotal, names[s],
                                     100.0 * bestStageMax[(size_t) s] / blockNs);
                    std::printf ("\n");
                    std::fflush (stdout);
                }
    }


    //==============================================================================
    // 部件级：同一素材下单独量 8× 真峰值检测器、Limit 的 4× 过采样来回、Color 的 8× 过采样来回、8× 域 Tube 曲线。
    void runParts (const juce::String& label, const std::vector<double>& srs, const std::vector<double>& bss,
                   double passSec, int passes)
    {
        for (auto sr : srs)
            for (auto bsD : bss)
            {
                const auto bs = (int) bsD;
                const auto& sig = signals.get (sr);
                const auto blocks = juce::jmax (8, (int) std::ceil (passSec * sr / bs));
                const auto blockNs = 1.0e9 * bs / sr;
                juce::AudioBuffer<float> buf (2, bs);

                auto measure = [&] (const char* name, auto&& fn)
                {
                    Feeder feeder { sig };
                    double best = 1e300;
                    for (int pass = -1; pass < passes; ++pass)
                    {
                        double sum = 0;
                        for (int i = 0; i < blocks; ++i)
                        {
                            feeder.fill (buf);
                            juce::ScopedNoDenormals nd;
                            const auto t0 = now();
                            fn (buf);
                            sum += toNs (now() - t0);
                        }
                        if (pass >= 0) best = juce::jmin (best, sum);
                    }
                    std::printf ("PARTS,%s,%g,%d,%s,avg%%=%.3f\n", label.toRawUTF8(), sr, bs, name, 100.0 * best / (blocks * blockNs));
                    std::fflush (stdout);
                };

                celadon::TruePeakDetector tp;
                tp.prepare (2);
                volatile float sink = 0;
                // 实时路径是按块接口（v0.5 起，限幅器与两块表都用它）；processSample 只是逐样本参照实现，另记一行对照。
                measure ("truePeak8x_stereo", [&] (juce::AudioBuffer<float>& b)
                {
                    float peak = 0;
                    for (int ch = 0; ch < 2; ++ch)
                        peak = juce::jmax (peak, tp.processBlockMax (ch, b.getReadPointer (ch), b.getNumSamples()));
                    sink = peak;
                });
                measure ("truePeak8x_stereo_reference", [&] (juce::AudioBuffer<float>& b)
                {
                    float peak = 0;
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        for (int ch = 0; ch < 2; ++ch)
                            peak = juce::jmax (peak, tp.processSample (ch, b.getSample (ch, i)));
                    sink = peak;
                });

                using OS = juce::dsp::Oversampling<float>;
                OS os4 (2);
                os4.clearOversamplingStages();
                os4.addOversamplingStage (OS::filterHalfBandFIREquiripple, 0.05f, -90.0f, 0.06f, -75.0f);
                os4.addOversamplingStage (OS::filterHalfBandFIREquiripple, 0.10f, -80.0f, 0.12f, -70.0f);
                os4.initProcessing ((size_t) bs);
                measure ("oversample4x_updown_stereo(Limit Clip)", [&] (juce::AudioBuffer<float>& b)
                {
                    juce::dsp::AudioBlock<float> block (b);
                    os4.processSamplesUp (block);
                    os4.processSamplesDown (block);
                });

                OS os8 (2);
                os8.clearOversamplingStages();
                os8.addOversamplingStage (OS::filterHalfBandFIREquiripple, 0.05f, -90.0f, 0.06f, -75.0f);
                os8.addOversamplingStage (OS::filterHalfBandFIREquiripple, 0.10f, -80.0f, 0.12f, -70.0f);
                os8.addOversamplingStage (OS::filterHalfBandFIREquiripple, 0.10f, -65.0f, 0.15f, -55.0f);
                os8.initProcessing ((size_t) bs);
                measure ("oversample8x_updown_stereo(Color Heat)", [&] (juce::AudioBuffer<float>& b)
                {
                    juce::dsp::AudioBlock<float> block (b);
                    os8.processSamplesUp (block);
                    os8.processSamplesDown (block);
                });

                celadon::Saturator tube;
                tube.setCurve (celadon::Saturator::Curve::Tube);
                tube.setDrive (1.0f);
                tube.setAsymmetry (0.0f);
                std::vector<float> up ((size_t) bs * 8 * 2);
                measure ("tube_tanh_on_8x_stereo(Heat curve only)", [&] (juce::AudioBuffer<float>& b)
                {
                    // 8× 域的样本数：每个输入样本 8 次，两声道。
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < b.getNumSamples(); ++i)
                            for (int k = 0; k < 8; ++k)
                                up[(size_t) ((ch * b.getNumSamples() + i) * 8 + k)] = tube.processSample (b.getSample (ch, i) * (0.9f + 0.01f * (float) k));
                    sink = up[0];
                });
            }
    }

    //==============================================================================
    template <typename T>
    void findAll (juce::Component& root, std::vector<T*>& out)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* t = dynamic_cast<T*> (child))
                out.push_back (t);
            findAll (*child, out);
        }
    }

    double cpuSecondsSelf()
    {
        rusage ru {};
        getrusage (RUSAGE_SELF, &ru);
        return (double) ru.ru_utime.tv_sec + 1e-6 * (double) ru.ru_utime.tv_usec
             + (double) ru.ru_stime.tv_sec + 1e-6 * (double) ru.ru_stime.tv_usec;
    }

    void runGui (const juce::String& label)
    {
        const double sr = 48000.0;
        const int bs = 512;
        CeladonAudioProcessor proc;
        proc.setPlayConfigDetails (2, 2, sr, bs);
        applyScenario (proc, allScenarios[1]);
        proc.prepareToPlay (sr, bs);

        juce::AudioBuffer<float> buf (2, bs);
        juce::MidiBuffer midi;
        Feeder feeder { signals.get (sr) };
        auto processMs = [&] (double ms)
        {
            const auto n = (int) std::ceil (ms * 0.001 * sr / bs);
            for (int i = 0; i < n; ++i) { feeder.fill (buf); proc.processBlock (buf, midi); }
        };
        processMs (3000.0);

        const auto tc0 = now();
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        const auto createMs = toNs (now() - tc0) * 1e-6;
        editor->setVisible (true);

        const auto w = editor->getWidth(), h = editor->getHeight();
        const float scale = 2.0f;

        auto paintOnce = [&] (const juce::RectangleList<int>* clip)
        {
            juce::Image img (juce::Image::ARGB, (int) (w * scale), (int) (h * scale), true);
            juce::Graphics g (img);
            g.addTransform (juce::AffineTransform::scale (scale));
            if (clip != nullptr)
                g.reduceClipRegion (*clip);
            const auto t0 = now();
            editor->paintEntireComponent (g, true);
            return toNs (now() - t0) * 1e-6;
        };

        const auto firstPaintMs = paintOnce (nullptr);

        // 定时器（30 Hz）每次读表 / 喂数；中间处理 33 ms 音频让读数一直在变。
        auto* timer = (juce::Timer*) (CeladonAudioProcessorEditor*) editor.get();
        std::vector<double> tickMs;
        for (int i = 0; i < 60; ++i)
        {
            processMs (33.3);
            const auto t0 = now();
            timer->timerCallback();
            tickMs.push_back (toNs (now() - t0) * 1e-6);
        }

        std::vector<double> fullMs;
        for (int i = 0; i < 20; ++i)
            fullMs.push_back (paintOnce (nullptr));

        // 每 tick 真正会重画的区域：4 根表、响度读数、Balance 曲线（值在变就重画）。
        juce::RectangleList<int> dirty;
        std::vector<celadon::ui::MeterBar*> bars;          findAll (*editor, bars);
        std::vector<celadon::ui::LoudnessReadout*> reads;  findAll (*editor, reads);
        std::vector<celadon::ui::BalanceCurve*> curves;    findAll (*editor, curves);
        for (auto* c : bars)   dirty.add (editor->getLocalArea (c, c->getLocalBounds()));
        for (auto* c : reads)  dirty.add (editor->getLocalArea (c, c->getLocalBounds()));
        for (auto* c : curves) dirty.add (editor->getLocalArea (c, c->getLocalBounds()));

        std::vector<double> clipMs;
        for (int i = 0; i < 30; ++i)
        {
            processMs (33.3);
            timer->timerCallback();
            clipMs.push_back (paintOnce (&dirty));
        }

        auto mean = [] (const std::vector<double>& v) { return std::accumulate (v.begin(), v.end(), 0.0) / (double) v.size(); };
        auto minOf = [] (const std::vector<double>& v) { return *std::min_element (v.begin(), v.end()); };

        // 真实消息循环 3 s：定时器照常跑（离屏没有窗口，重画请求不会真的画），量消息线程的 CPU。
        const auto cpu0 = cpuSecondsSelf();
        const auto wall0 = now();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (3000);
        const auto loopCpuPct = 100.0 * (cpuSecondsSelf() - cpu0) / (toNs (now() - wall0) * 1e-9);

        const auto perTickMs = mean (clipMs) + mean (tickMs);
        std::printf ("GUI,%s,editor=%dx%d@%gx,create_ms=%.2f,first_paint_ms=%.2f,full_paint_mean_ms=%.2f,full_paint_min_ms=%.2f,"
                     "dirty_rects=%d,dirty_area_frac=%.3f,dirty_paint_mean_ms=%.2f,timerCallback_mean_ms=%.3f,"
                     "est_gui_cpu_at_30Hz%%=%.1f,full_repaint_at_30Hz%%=%.1f,idle_loop_cpu%%=%.1f\n",
                     label.toRawUTF8(), w, h, (double) scale, createMs, firstPaintMs, mean (fullMs), minOf (fullMs),
                     dirty.getNumRectangles(),
                     [&] { double a = 0; for (auto& r : dirty) a += (double) r.getWidth() * r.getHeight(); return a / ((double) w * h); }(),
                     mean (clipMs), mean (tickMs), perTickMs * 30.0 / 10.0, mean (fullMs) * 30.0 / 10.0, loopCpuPct);
        std::fflush (stdout);
        editor.reset();
    }
    // Through the actual host-facing processor API: identical input/block timeline,
    // isNonRealtime changes, APVTS state round-trips and both host prepare orders.
    int runRenderCheck()
    {
        constexpr double sr = 48000.0;
        constexpr int bs = 256;
        int failures = 0;
        auto check = [&] (bool ok, const juce::String& name)
        {
            std::printf ("RENDER,%s,%s\n", ok ? "PASS" : "FAIL", name.toRawUTF8());
            if (! ok) ++failures;
        };
        auto material = makeSignal (sr, 8.0);
        juce::MidiBuffer midi;
        auto fill = [&] (juce::AudioBuffer<float>& block, int absolute)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < bs; ++i)
                    block.setSample (ch, i, material.getSample (ch, (absolute + i) % material.getNumSamples())
                                                   * (absolute + i < (int) (35.0 * sr) ? 0.22f : 0.90f));
        };
        auto difference = [&] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            double peak = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < bs; ++i)
                {
                    const auto x = a.getSample (ch, i), y = b.getSample (ch, i);
                    if (! std::isfinite (x) || ! std::isfinite (y)) return 1.0e30;
                    peak = std::max (peak, std::abs ((double) x - y));
                }
            return peak;
        };
        auto parameterError = [] (const celadon::ChainParams& a, const celadon::ChainParams& b)
        {
            return std::max ({ std::abs (a.balance.recover - b.balance.recover), std::abs (a.balance.tame - b.balance.tame),
                               std::abs (a.color.toneDb - b.color.toneDb), std::abs (a.color.glue - b.color.glue),
                               std::abs (a.color.heat - b.color.heat), std::abs (a.limit.gainDb - b.limit.gainDb),
                               std::abs (a.limit.clip - b.limit.clip) });
        };
        auto restorePair = [&] (CeladonAudioProcessor& source, int offset, const char* tag)
        {
            juce::MemoryBlock state;
            source.getStateInformation (state);
            auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
            auto* glaze = xml ? xml->getChildByName ("GLAZE_STATE") : nullptr;
            check (glaze && glaze->getChildByName ("EXECUTION"), juce::String (tag) + " saves execution independently");
            const auto expected = source.getGlazeEffectiveParamsForUi();
            for (bool prepareFirst : { false, true })
            {
                CeladonAudioProcessor rt, offline;
                offline.setNonRealtime (true);
                if (prepareFirst) { rt.prepareToPlay (sr, bs); offline.prepareToPlay (sr, bs); }
                rt.setStateInformation (state.getData(), (int) state.getSize());
                offline.setStateInformation (state.getData(), (int) state.getSize());
                if (! prepareFirst) { rt.prepareToPlay (sr, bs); offline.prepareToPlay (sr, bs); }
                juce::AudioBuffer<float> a (2, bs), b (2, bs);
                double peak = 0.0;
                float firstError = 0.0f;
                for (int pos = 0; pos < (int) (2.0 * sr); pos += bs)
                {
                    fill (a, offset + pos); b.makeCopyOf (a, true);
                    rt.processBlock (a, midi); offline.processBlock (b, midi);
                    peak = std::max (peak, difference (a, b));
                    if (pos == 0) firstError = parameterError (expected, rt.getGlazeEffectiveParamsForUi());
                }
                const auto label = juce::String (tag) + (prepareFirst ? " prepare-before-restore" : " restore-before-prepare");
                std::printf ("RENDER,INFO,%s peakDifference=%.9g firstParameterError=%.9g\n", label.toRawUTF8(), peak, firstError);
                check (peak == 0.0, label + " realtime/offline audio matches exactly");
                check (firstError < 0.002f, label + " retains executed controls on first block");
                rt.releaseResources(); offline.releaseResources();
            }
            // Offline hosts often choose a larger block. Adaptive controls are block
            // quantised: measure audible level and final controls instead of bit identity.
            struct BlockResult { double energy = 0.0; float integrated = 0.0f; std::array<float, 20> shortTerm {}; celadon::ChainParams params; };
            auto changedBlockRender = [&] (int blockSize)
            {
                CeladonAudioProcessor processor;
                processor.setNonRealtime (blockSize > bs);
                processor.setStateInformation (state.getData(), (int) state.getSize());
                processor.prepareToPlay (sr, blockSize);
                juce::AudioBuffer<float> block (2, blockSize);
                BlockResult result;
                constexpr int total = 60 * 48000;
                celadon::LoudnessMeter meter;
                meter.prepare ({ sr, (juce::uint32) blockSize, 2 });
                for (int pos = 0; pos < total; pos += blockSize)
                {
                    const auto count = juce::jmin (blockSize, total - pos);
                    block.setSize (2, count, false, false, true);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < count; ++i)
                            block.setSample (ch, i, material.getSample (ch, (offset + pos + i) % material.getNumSamples())
                                                          * (offset + pos + i < 35 * 48000 ? 0.22f : 0.90f));
                    processor.processBlock (block, midi);
                    for (int done = 0; done < count;)
                    {
                        const auto nextWindow = ((pos + done) / 144000 + 1) * 144000;
                        const auto take = juce::jmin (count - done, nextWindow - pos - done);
                        juce::AudioBuffer<float> slice (block.getArrayOfWritePointers(), 2, done, take);
                        meter.process (slice);
                        done += take;
                        if (pos + done == nextWindow)
                            result.shortTerm[(size_t) (nextWindow / 144000 - 1)] = meter.getShortTermLufs();
                    }
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < count; ++i)
                            if (pos + i >= 48000)
                                result.energy += (double) block.getSample (ch, i) * block.getSample (ch, i);
                }
                result.params = processor.getGlazeEffectiveParamsForUi();
                result.integrated = meter.getIntegratedLufs();
                processor.releaseResources();
                return result;
            };
            const auto small = changedBlockRender (64), large = changedBlockRender (1024);
            const auto levelDelta = 10.0 * std::log10 (large.energy / small.energy);
            const auto gainDelta = large.params.limit.gainDb - small.params.limit.gainDb;
            std::printf ("RENDER,INFO,%s 64-vs-1024 RMSdelta=%.6g dB gainDelta=%.6g dB parameterMaxDelta=%.6g\n",
                         tag, levelDelta, gainDelta, parameterError (small.params, large.params));
            float windowDelta = 0.0f;
            for (size_t i = 0; i < small.shortTerm.size(); ++i)
                windowDelta = std::max (windowDelta, std::abs (large.shortTerm[i] - small.shortTerm[i]));
            const auto integratedDelta = large.integrated - small.integrated;
            std::printf ("RENDER,INFO,%s 60s integratedDelta=%.6g LU max3sDelta=%.6g LU\n", tag, integratedDelta, windowDelta);
            check (std::isfinite (levelDelta) && std::abs (integratedDelta) <= 0.1f && windowDelta <= 0.2f && std::abs (gainDelta) <= 0.25,
                   juce::String (tag) + " changed block size keeps loudness and gain close for 60s");
        };
        CeladonAudioProcessor realtime, alwaysOffline, switchOffline;
        alwaysOffline.setNonRealtime (true);
        realtime.prepareToPlay (sr, bs); alwaysOffline.prepareToPlay (sr, bs); switchOffline.prepareToPlay (sr, bs);
        juce::AudioBuffer<float> a (2, bs), b (2, bs), c (2, bs);
        double fromStartError = 0.0, switchedError = 0.0;
        bool savedLearning = false, savedCalibration = false, savedChorus = false;
        for (int offset = 0; offset < (int) (55.0 * sr); offset += bs)
        {
            if (offset >= (int) (5.0 * sr)) switchOffline.setNonRealtime (true);
            fill (a, offset); b.makeCopyOf (a, true); c.makeCopyOf (a, true);
            realtime.processBlock (a, midi); alwaysOffline.processBlock (b, midi); switchOffline.processBlock (c, midi);
            fromStartError = std::max (fromStartError, difference (a, b));
            switchedError = std::max (switchedError, difference (a, c));
            const auto status = realtime.getGlazeStatus();
            if (! savedLearning && offset >= (int) (5.0 * sr))
            { restorePair (realtime, offset + bs, "learning at 5s"); savedLearning = true; }
            if (! savedCalibration && status.phase == celadon::glaze::Phase::calibrating)
            { restorePair (realtime, offset + bs, "calibration running"); savedCalibration = true; }
            if (! savedChorus && offset >= (int) (44.0 * sr))
            { restorePair (realtime, offset + bs, "loud chorus"); savedChorus = true; }
        }
        std::printf ("RENDER,INFO,fromStartDifference=%.9g switchedDifference=%.9g\n", fromStartError, switchedError);
        check (fromStartError == 0.0, "fresh realtime/offline whole render matches exactly");
        check (switchedError == 0.0, "switching to offline at 5s leaves audio unchanged");
        check (savedCalibration, "fixture actually reaches a running calibration");
        realtime.releaseResources(); alwaysOffline.releaseResources(); switchOffline.releaseResources();
        return failures == 0 ? 0 : 1;
    }

    int runStateCheck()
    {
        int failures = 0;
        auto check = [&] (bool ok, const char* name)
        {
            std::printf ("STATE,%s,%s\n", ok ? "PASS" : "FAIL", name);
            if (! ok) ++failures;
        };
        auto value = [] (CeladonAudioProcessor& processor, const char* id)
        {
            return processor.getParameters().getRawParameterValue (id)->load();
        };
        auto setValue = [] (CeladonAudioProcessor& processor, const char* id, float raw)
        {
            auto* parameter = processor.getParameters().getParameter (id);
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (raw));
        };
        auto fresh = std::make_unique<CeladonAudioProcessor>();
        check (value (*fresh, "glaze_amount") == 35.0f, "fresh instance starts at 35 percent");
        check (value (*fresh, "glaze_style") == 0.0f, "fresh instance uses song-adaptive Auto style");
        auto firstTwoSeconds = [&] (CeladonAudioProcessor& processor, float inputGain)
        {
            processor.prepareToPlay (48000.0, 256);
            auto signal = makeSignal (48000.0, 2.0);
            signal.applyGain (inputGain);
            juce::MidiBuffer midi;
            for (int offset = 0; offset < signal.getNumSamples(); offset += 256)
            {
                const auto count = juce::jmin (256, signal.getNumSamples() - offset);
                juce::AudioBuffer<float> block (signal.getArrayOfWritePointers(), 2, offset, count);
                processor.processBlock (block, midi);
            }
            return processor.getGlazeEffectiveParamsForUi();
        };
        const auto effective = firstTwoSeconds (*fresh, 1.0f);
        std::printf ("STATE,INFO,after two seconds: GLAZE=%.2f Gain=%.3f Tame=%.3f Heat=%.3f\n",
                     effective.glazePercent, effective.limit.gainDb, effective.balance.tame, effective.color.heat);
        // 此基准素材接近满刻度且密集；启动判断不能要求已经很响的输入再被强行增响。
        check (effective.balance.tame > 0.0f && effective.color.heat > 0.0f,
               "fresh instance applies spectral processing and colour within two seconds");
        auto quiet = std::make_unique<CeladonAudioProcessor>();
        const auto quietEffective = firstTwoSeconds (*quiet, 0.25f);
        std::printf ("STATE,INFO,with input headroom: Gain=%.3f Tame=%.3f Heat=%.3f\n",
                     quietEffective.limit.gainDb, quietEffective.balance.tame, quietEffective.color.heat);
        check (quietEffective.limit.gainDb > 0.1f && quietEffective.limit.gainDb > effective.limit.gainDb,
               "fresh instance adds more gain when the same material has headroom");
        quiet->releaseResources();

        setValue (*fresh, "glaze_amount", 0.0f);
        juce::MemoryBlock zeroState;
        fresh->getStateInformation (zeroState);
        auto restored = std::make_unique<CeladonAudioProcessor>();
        restored->setStateInformation (zeroState.getData(), (int) zeroState.getSize());
        check (value (*restored, "glaze_amount") == 0.0f, "explicit saved zero stays manual");

        auto legacyXml = juce::AudioProcessor::getXmlFromBinary (zeroState.getData(), (int) zeroState.getSize());
        for (auto* child = legacyXml->getFirstChildElement(); child != nullptr;)
        {
            auto* next = child->getNextElement();
            if (child->getStringAttribute ("id") == "glaze_amount")
                legacyXml->removeChildElement (child, true);
            child = next;
        }
        juce::MemoryBlock legacyState;
        juce::AudioProcessor::copyXmlToBinary (*legacyXml, legacyState);
        setValue (*restored, "glaze_amount", 83.0f);
        restored->setStateInformation (legacyState.getData(), (int) legacyState.getSize());
        check (value (*restored, "glaze_amount") == 0.0f, "missing legacy GLAZE resets an already-used instance to zero");
        auto legacyFresh = std::make_unique<CeladonAudioProcessor>();
        legacyFresh->setStateInformation (legacyState.getData(), (int) legacyState.getSize());
        check (value (*legacyFresh, "glaze_amount") == 0.0f, "missing legacy GLAZE overrides fresh default safely");

        setValue (*restored, "glaze_amount", 68.0f);
        setValue (*restored, "color_heat", 42.0f);
        juce::MemoryBlock currentState;
        restored->getStateInformation (currentState);
        legacyFresh->setStateInformation (currentState.getData(), (int) currentState.getSize());
        check (std::abs (value (*legacyFresh, "glaze_amount") - 68.0f) < 0.01f
               && std::abs (value (*legacyFresh, "color_heat") - 42.0f) < 0.01f,
               "explicit current parameters survive save and reopen");
        fresh->releaseResources();
        return failures == 0 ? 0 : 1;
    }
}

int main (int argc, char* argv[])
{
    mach_timebase_info_data_t tb {};
    mach_timebase_info (&tb);
    nsPerTick = (double) tb.numer / (double) tb.denom;

    juce::ScopedJuceInitialiser_GUI init;

    const juce::String mode (argc > 1 ? argv[1] : "grid");
    useSustainedBassFixture = mode == "bassgrid";
    if (mode == "statecheck")
        return runStateCheck();
    if (mode == "rendercheck")
        return runRenderCheck();
    const juce::String label (
       #if defined (NDEBUG)
        "release"
       #else
        "debug"
       #endif
    );

    if (auto* cold = std::getenv ("COLD_MB"))
        coldBuffer.assign ((size_t) std::atoi (cold) * 1024u * 1024u, 0);

    if (mode == "ecore")
        pthread_set_qos_class_self_np (QOS_CLASS_BACKGROUND, 0);
    else
        pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);

    if (mode == "gui")
    {
        runGui (label);
        signals.entries.clear();
        return 0;
    }

    if (argc < 8)
    {
        std::fprintf (stderr, "usage: %s grid|bassgrid|modules|ecore <scenario|all> <srs> <bss> <warmSec> <passSec> <passes>\n", argv[0]);
        return 2;
    }

    const auto scs = pickScenarios (argv[2]);
    const auto srs = parseList (argv[3]);
    const auto bss = parseList (argv[4]);
    const auto warm = juce::String (argv[5]).getDoubleValue();
    const auto passSec = juce::String (argv[6]).getDoubleValue();
    const auto passes = juce::String (argv[7]).getIntValue();

    if (mode == "modules")
        runModules (label, scs, srs, bss, warm, passSec, passes);
    else if (mode == "parts")
        runParts (label, srs, bss, passSec, passes);
    else
        runGrid ((mode == "ecore" ? label + "-ecore" : label) + (coldBuffer.empty() ? "" : "-cold"), scs, srs, bss, warm, passSec, passes);

    signals.entries.clear();
    return 0;
}
