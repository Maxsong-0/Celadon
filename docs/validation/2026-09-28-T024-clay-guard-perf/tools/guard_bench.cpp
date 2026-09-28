// T-024 · 修坯一级（ClayModule::process）的 x86 微基准：改前（ClayModuleRef）与改后（ClayModule）交替跑。
//
// 只量修坯一级，不是整插件验收（整插件验收只认 Mac 上的 scripts/perf-local.sh，ADR-007）；这里给「改了多少」的方向与量级。
// 口径照 tools/perf/bench.mm：先空跑再测、每遍一个数、多遍取最短的平均；另按 65536 样本的相位分组取中位数再取最大
// （= bench 的 clay_structWorst64k：量得到 65536 样本一次的精确重算那一块），百分比都是「占这一块的实时预算」。
//
// 用法：guard_bench [轮数，默认 3] [每遍秒数，默认 4] [遍数，默认 3] [只跑名字里含这个子串的场景]
// 建议 taskset -c <空闲核> 跑，期间机器上不要有编译。

#include "ClayModule.h"
#include "ClayModuleRef.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr double twoPi = 6.283185307179586476925286766559;

    // 母带式测试素材：照 tools/perf/bench.mm 的 makeSignal（粉噪底 + 底鼓 / 军鼓 / 镲 + 持续音，峰值约 −1 dBFS，固定种子）。
    juce::AudioBuffer<float> makeSignal (double sr, double seconds)
    {
        const auto n = (int) std::ceil (sr * seconds);
        juce::AudioBuffer<float> sig (2, n);
        juce::Random rng (20260925);
        double b[2][7] = {};
        float prevWhite[2] = {};

        for (int i = 0; i < n; ++i)
        {
            const auto t = (double) i / sr;
            const auto common = rng.nextFloat() * 2.0f - 1.0f;
            const auto kickT = std::fmod (t, 0.5);
            const auto kick = 0.55 * std::sin (twoPi * 55.0 * kickT) * std::exp (-kickT / 0.08);
            const auto snareT = std::fmod (t + 0.75, 1.0);
            const auto snareEnv = 0.30 * std::exp (-snareT / 0.03);
            const auto hatT = std::fmod (t, 0.125);
            const auto hatEnv = 0.12 * std::exp (-hatT / 0.01);
            const auto pad = 0.05 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 330.0 * t + 0.3) + std::sin (twoPi * 440.0 * t + 1.1));

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto own = rng.nextFloat() * 2.0f - 1.0f;
                const auto white = 0.8f * common + 0.2f * own;
                auto* s = b[ch];
                s[0] = 0.99886 * s[0] + white * 0.0555179;
                s[1] = 0.99332 * s[1] + white * 0.0750759;
                s[2] = 0.96900 * s[2] + white * 0.1538520;
                s[3] = 0.86650 * s[3] + white * 0.3104856;
                s[4] = 0.55000 * s[4] + white * 0.5329522;
                s[5] = -0.7616 * s[5] - white * 0.0168980;
                const auto pink = (s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + white * 0.5362) * 0.11;
                s[6] = white * 0.115926;
                const auto hat = (double) (white - prevWhite[ch]) * 0.5;
                prevWhite[ch] = white;
                const auto v = pink + kick + snareEnv * (double) white + hatEnv * hat + pad * (ch == 0 ? 1.0 : 0.9);
                sig.setSample (ch, i, (float) juce::jlimit (-0.99, 0.99, v));
            }
        }

        return sig;
    }

    struct Scenario
    {
        const char* name;
        celadon::ClayModule::Params params;
        bool dense;   // 素材 × 4 再硬削到 ±0.6（已限幅的密素材：守护频繁动作）
    };

    celadon::ClayModule::Params make (float lowCut, float monoHz, float mono, float width)
    {
        celadon::ClayModule::Params p;
        p.lowCutHz = lowCut;
        p.monoHz = monoHz;
        p.monoAmount = mono;
        p.widthDb = width;
        return p;
    }

    const Scenario scenarios[] = {
        { "neutral",   make (0.0f, 80.0f, 0.0f, 0.0f),    false },   // GLAZE 0 / 抽屉默认：修坯逐位纯延迟（bench 的 manual）
        { "lowcut30",  make (30.0f, 80.0f, 0.0f, 0.0f),   false },
        { "width+1",   make (0.0f, 80.0f, 0.0f, 1.0f),    false },
        { "all",       make (30.0f, 120.0f, 1.0f, 1.5f),  false },
        { "dense-all", make (60.0f, 300.0f, 1.0f, 3.0f),  true  },   // 最狠的设置 + 密素材：守护经常 g < 1
    };

    struct Result { double avgPct = 0, structWorst64kPct = 0, typicalPct = 0, nsPerSample = 0; std::int64_t guarded = 0; };

    double nowNs()
    {
        return (double) std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    template <typename Module, typename Params>
    Result runOne (const Params& params, const juce::AudioBuffer<float>& signal, double sr, int bs,
                                   double warmSec, double passSec, int passes)
    {
        auto module = std::make_unique<Module>();
        module->setParams (params);
        module->prepare ({ sr, (juce::uint32) bs, 2 });

        juce::AudioBuffer<float> buf (2, bs);
        const auto total = signal.getNumSamples();
        int readPos = 0;
        std::int64_t samplePos = 0;

        const auto fill = [&]
        {
            for (int done = 0; done < bs;)
            {
                const auto chunk = std::min (bs - done, total - readPos);
                for (int ch = 0; ch < 2; ++ch)
                    buf.copyFrom (ch, done, signal, ch, readPos, chunk);
                done += chunk;
                readPos += chunk;
                if (readPos >= total)
                    readPos = 0;
            }
        };

        const auto warmBlocks = (int) (warmSec * sr / bs);
        for (int i = 0; i < warmBlocks; ++i)
        {
            fill();
            module->process (buf);
            samplePos += bs;
        }

        module->getAndResetPeakGuardStats();

        constexpr int longPeriod = 65536;
        const auto numLongPhases = std::max (1, longPeriod / bs);
        std::vector<std::vector<double>> byLongPhase ((size_t) numLongPhases);
        const auto blocks = (int) (passSec * sr / bs);
        const auto blockNs = 1.0e9 * bs / sr;
        std::vector<double> times ((size_t) blocks);
        double bestAvg = 1e300;
        double checksum = 0;

        for (int pass = 0; pass < passes; ++pass)
        {
            double sum = 0;

            for (int i = 0; i < blocks; ++i)
            {
                fill();
                const auto t0 = nowNs();
                module->process (buf);
                const auto dt = nowNs() - t0;
                times[(size_t) i] = dt;
                sum += dt;
                byLongPhase[(size_t) ((samplePos % longPeriod) / bs % numLongPhases)].push_back (dt);
                samplePos += bs;
                checksum += std::abs (buf.getSample (0, bs / 2));
            }

            bestAvg = std::min (bestAvg, sum / blocks);
        }

        double structWorst = 0, typical = 1e300;
        for (auto& v : byLongPhase)
        {
            if (v.empty())
                continue;
            std::nth_element (v.begin(), v.begin() + (long) (v.size() / 2), v.end());
            const auto med = v[v.size() / 2];
            structWorst = std::max (structWorst, med);
            typical = std::min (typical, med);
        }

        Result r;
        r.avgPct = 100.0 * bestAvg / blockNs;
        r.structWorst64kPct = 100.0 * structWorst / blockNs;
        r.typicalPct = 100.0 * typical / blockNs;
        r.nsPerSample = bestAvg / bs;
        r.guarded = module->getAndResetPeakGuardStats().guardedSamples + (checksum < -1.0 ? 1 : 0);
        return r;
    }

    celadon::ClayModuleRef::Params toRef (const celadon::ClayModule::Params& p)
    {
        celadon::ClayModuleRef::Params q;
        q.lowCutHz = p.lowCutHz;
        q.monoHz = p.monoHz;
        q.monoAmount = p.monoAmount;
        q.widthDb = p.widthDb;
        q.bypass = p.bypass;
        return q;
    }
}

int main (int argc, char** argv)
{
    const auto rounds  = argc > 1 ? std::atoi (argv[1]) : 3;
    const auto passSec = argc > 2 ? std::atof (argv[2]) : 4.0;
    const auto passes  = argc > 3 ? std::atoi (argv[3]) : 3;
    const std::string filter = argc > 4 ? argv[4] : "";
    const auto warmSec = 2.0;

    struct Config { double sr; int bs; };
    const Config configs[] = { { 48000.0, 512 }, { 48000.0, 32 }, { 192000.0, 32 }, { 192000.0, 512 } };

    for (const auto& config : configs)
    {
        auto signal = makeSignal (config.sr, 8.0);
        auto dense = signal;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < dense.getNumSamples(); ++i)
                dense.setSample (ch, i, juce::jlimit (-0.6f, 0.6f, 4.0f * dense.getSample (ch, i)));

        for (const auto& sc : scenarios)
        {
            if (! filter.empty() && std::string (sc.name).find (filter) == std::string::npos)
                continue;

            const auto& material = sc.dense ? dense : signal;
            std::vector<double> oldAvg, newAvg, oldSw, newSw;

            for (int round = 0; round < rounds; ++round)
            {
                const auto o = runOne<celadon::ClayModuleRef> (toRef (sc.params), material, config.sr, config.bs, warmSec, passSec, passes);
                const auto n = runOne<celadon::ClayModule> (sc.params, material, config.sr, config.bs, warmSec, passSec, passes);
                std::printf ("BENCH,%s,%g,%d,round=%d,old_avg%%=%.3f,new_avg%%=%.3f,old_ns/smp=%.2f,new_ns/smp=%.2f,"
                             "old_sw64k%%=%.3f,new_sw64k%%=%.3f,old_typ%%=%.3f,new_typ%%=%.3f,guarded_old=%lld,guarded_new=%lld\n",
                             sc.name, config.sr, config.bs, round + 1, o.avgPct, n.avgPct, o.nsPerSample, n.nsPerSample,
                             o.structWorst64kPct, n.structWorst64kPct, o.typicalPct, n.typicalPct,
                             (long long) o.guarded, (long long) n.guarded);
                std::fflush (stdout);
                oldAvg.push_back (o.avgPct);
                newAvg.push_back (n.avgPct);
                oldSw.push_back (o.structWorst64kPct);
                newSw.push_back (n.structWorst64kPct);
            }

            // 各轮之间（共享的云主机上）偶有 ±20 % 的干扰：汇总取各轮的中位数。
            const auto median = [] (std::vector<double> v) { std::sort (v.begin(), v.end()); return v[v.size() / 2]; };
            std::printf ("SUMMARY,%s,%g,%d,old_avg%%=%.3f,new_avg%%=%.3f,ratio=%.2f,old_sw64k%%=%.3f,new_sw64k%%=%.3f\n",
                         sc.name, config.sr, config.bs, median (oldAvg), median (newAvg), median (oldAvg) / median (newAvg),
                         median (oldSw), median (newSw));
            std::fflush (stdout);
        }
    }

    return 0;
}
