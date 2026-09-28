// 性能验收用 VST3 宿主（v0.5 起，ADR-007；源自 v0.5 研究 P1）：直接加载 Xcode 实际编出的 Celadon.vst3（Debug / Release），
// 与 DAW 一样走 VST3 接口调 processBlock，测每块耗时占块预算的百分比。单线程、固定种子素材、先空跑再测、取多遍最短。
// 用法：celadon_host <Celadon.vst3 路径> <tag> <scenario|all> <srs> <bss> <warmSec> <passSec> <passes>
#include <JuceHeader.h>

#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/qos.h>

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
    double nsPerTick = 1.0;
    inline uint64_t now() noexcept { return mach_absolute_time(); }
    inline double toNs (uint64_t t) noexcept { return (double) t * nsPerTick; }

    struct Scenario { const char* name; std::vector<std::pair<const char*, float>> normalised; };

    // 参数按名字找（VST3 宿主看到的是参数名）；这几项都是线性范围：值 / 范围 = 归一值。
    const std::vector<Scenario> allScenarios
    {
        { "default", {} },
        { "auto100", { { "Glaze", 1.0f } } },
        { "typical", { { "Glaze", 30.0f / 100.0f }, { "Balance Recover", 0.30f }, { "Balance Tame", 0.30f },
                       { "Color Heat", 0.30f }, { "Color Glue", 0.30f }, { "Limit Gain", 6.0f / 24.0f } } },
    };

    std::vector<double> parseList (const juce::String& s)
    {
        std::vector<double> out;
        for (auto& token : juce::StringArray::fromTokens (s, ",", ""))
            out.push_back (token.getDoubleValue());
        return out;
    }

    juce::AudioBuffer<float> makeSignal (double sr, double seconds)
    {
        // 与 bench.mm 相同的素材。
        const auto n = (int) std::ceil (sr * seconds);
        juce::AudioBuffer<float> sig (2, n);
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
            const auto kickT = std::fmod (t, 0.5);
            const auto kick = 0.55 * std::sin (twoPi * 55.0 * kickT) * std::exp (-kickT / 0.08);
            const auto snareT = std::fmod (t + 0.75, 1.0);
            const auto snareEnv = 0.30 * std::exp (-snareT / 0.03);
            const auto hatT = std::fmod (t, 0.125);
            const auto hatEnv = 0.12 * std::exp (-hatT / 0.01);
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
}

int main (int argc, char* argv[])
{
    mach_timebase_info_data_t tb {};
    mach_timebase_info (&tb);
    nsPerTick = (double) tb.numer / (double) tb.denom;
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);

    juce::ScopedJuceInitialiser_GUI init;

    if (argc < 9)
    {
        std::fprintf (stderr, "usage: %s <vst3> <tag> <scenario|all> <srs> <bss> <warmSec> <passSec> <passes>\n", argv[0]);
        return 2;
    }

    const juce::String path (argv[1]), tag (argv[2]), which (argv[3]);
    const auto srs = parseList (argv[4]);
    const auto bss = parseList (argv[5]);
    const auto warmSec = juce::String (argv[6]).getDoubleValue();
    const auto passSec = juce::String (argv[7]).getDoubleValue();
    const auto passes = juce::String (argv[8]).getIntValue();

    juce::VST3PluginFormatHeadless format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);

    if (types.isEmpty())
    {
        std::fprintf (stderr, "no plugin found in %s\n", path.toRawUTF8());
        return 3;
    }

    for (auto& sc : allScenarios)
    {
        if (which != "all" && which != sc.name)
            continue;

        for (auto sr : srs)
        {
            const auto sig = makeSignal (sr, 8.0);

            for (auto bsD : bss)
            {
                const auto bs = (int) bsD;
                juce::String error;
                const auto tc0 = now();
                auto inst = format.createInstanceFromDescription (*types[0], sr, bs, error);
                const auto createMs = toNs (now() - tc0) * 1e-6;

                if (inst == nullptr)
                {
                    std::fprintf (stderr, "create failed: %s\n", error.toRawUTF8());
                    return 4;
                }

                inst->enableAllBuses();
                inst->setPlayConfigDetails (2, 2, sr, bs);

                for (auto& [name, value] : sc.normalised)
                {
                    bool found = false;
                    for (auto* p : inst->getParameters())
                        if (p->getName (64) == name)
                        {
                            p->setValueNotifyingHost (value);
                            found = true;
                        }
                    if (! found) { std::fprintf (stderr, "param %s not found\n", name); return 5; }
                }

                const auto tp0 = now();
                inst->prepareToPlay (sr, bs);
                const auto prepareMs = toNs (now() - tp0) * 1e-6;

                juce::AudioBuffer<float> buf (2, bs);
                juce::MidiBuffer midi;
                int pos = 0;
                auto fill = [&]
                {
                    for (int done = 0; done < bs;)
                    {
                        const auto chunk = juce::jmin (bs - done, sig.getNumSamples() - pos);
                        for (int ch = 0; ch < 2; ++ch)
                            buf.copyFrom (ch, done, sig, ch, pos, chunk);
                        done += chunk;
                        pos += chunk;
                        if (pos >= sig.getNumSamples()) pos = 0;
                    }
                };

                for (int i = 0, n = (int) std::ceil (warmSec * sr / bs); i < n; ++i)
                {
                    fill();
                    inst->processBlock (buf, midi);
                }

                const auto blocks = juce::jmax (8, (int) std::ceil (passSec * sr / bs));
                const auto blockNs = 1.0e9 * bs / sr;
                std::vector<double> times ((size_t) blocks);
                double bestAvg = 1e300, bestWorst = 1e300, bestP999 = 1e300;

                for (int pass = 0; pass < passes; ++pass)
                {
                    for (int i = 0; i < blocks; ++i)
                    {
                        fill();
                        const auto t0 = now();
                        inst->processBlock (buf, midi);
                        times[(size_t) i] = toNs (now() - t0);
                    }
                    const auto sum = std::accumulate (times.begin(), times.end(), 0.0);
                    const auto mx = *std::max_element (times.begin(), times.end());
                    auto sorted = times;
                    std::sort (sorted.begin(), sorted.end());
                    const auto p999 = sorted[(size_t) juce::jmin (blocks - 1, (int) std::floor (0.999 * blocks))];
                    bestAvg = juce::jmin (bestAvg, 100.0 * sum / (blocks * blockNs));
                    bestWorst = juce::jmin (bestWorst, 100.0 * mx / blockNs);
                    bestP999 = juce::jmin (bestP999, 100.0 * p999 / blockNs);
                }

                std::printf ("HOST,%s,%s,%g,%d,avg%%=%.3f,worst%%=%.3f,p99.9%%=%.3f,create_ms=%.1f,prepare_ms=%.2f,latency=%d,blocks=%d\n",
                             tag.toRawUTF8(), sc.name, sr, bs, bestAvg, bestWorst, bestP999, createMs, prepareMs,
                             inst->getLatencySamples(), blocks);
                std::fflush (stdout);
                inst->releaseResources();
                inst.reset();
            }
        }
    }

    return 0;
}
