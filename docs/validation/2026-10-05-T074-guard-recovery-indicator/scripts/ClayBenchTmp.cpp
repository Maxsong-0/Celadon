// T-074 临时性能对照（只放进临时副本，不进仓库）：ClayModule::process 每帧（一对立体声样本）的耗时。
// 改前（main）没有 setGuardKeepReporting，改后（T-074）默认关、链里打开；改后副本在 include 前定义 BENCH_KEEP。
#include "TestHelpers.h"

#include "../Source/dsp/ClayModule.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace celadon::test
{
    namespace
    {
        using ClayParams = ClayModule::Params;

        constexpr double rate = 48000.0;
        constexpr int block = 512;

        ClayParams makeParams (float lowCutHz, float monoHz, float monoAmount, float widthDb)
        {
            ClayParams p;
            p.lowCutHz = lowCutHz;
            p.monoHz = monoHz;
            p.monoAmount = monoAmount;
            p.widthDb = widthDb;
            p.bypass = false;
            return p;
        }

        // 「woqu 式」素材：49 Hz 贝斯 0.4 + 98 Hz 二次谐波 0.15 + 4 Hz 次声偏置 0.15（左右相同）——守护大半时间在拉。
        Signal offsetBass (int frames)
        {
            Signal s (2, std::vector<float> ((size_t) frames));

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < frames; ++i)
                {
                    const auto t = (double) i / rate;
                    s[(size_t) ch][(size_t) i] = (float) (0.4 * std::sin (2.0 * M_PI * 49.0 * t) + 0.15 * std::sin (2.0 * M_PI * 98.0 * t)
                                                          + 0.15 * std::sin (2.0 * M_PI * 4.0 * t));
                }

            return s;
        }

        // 节目式素材：宽带噪声 + 30 Hz 低音（左大右小）。
        Signal programme (int frames)
        {
            auto s = makeNoise (2, frames, 0.1f, 4407);

            for (int i = 0; i < frames; ++i)
            {
                const auto t = (double) i / rate;
                s[0][(size_t) i] += (float) (0.3 * std::sin (2.0 * M_PI * 30.0 * t));
                s[1][(size_t) i] += (float) (0.15 * std::sin (2.0 * M_PI * 30.0 * t));
            }

            return s;
        }

        // 每帧纳秒（最好的一遍；第一遍预热）。
        double nsPerFrame (const ClayParams& params, const Signal& input, bool reportKeep)
        {
            ClayModule clay;
            clay.setParams (params);
            clay.prepare ({ rate, (juce::uint32) block, 2u });
            clay.setParams (params);
           #ifdef BENCH_KEEP
            clay.setGuardKeepReporting (reportKeep);
           #else
            juce::ignoreUnused (reportKeep);
           #endif

            const auto frames = (int) input.front().size() / block * block;
            juce::AudioBuffer<float> buffer (2, block);
            auto best = 1.0e30;

            for (int pass = 0; pass < 12; ++pass)
            {
                const auto started = std::chrono::steady_clock::now();

                for (int at = 0; at < frames; at += block)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        std::copy (input[(size_t) ch].begin() + at, input[(size_t) ch].begin() + at + block, buffer.getWritePointer (ch));

                    clay.process (buffer);
                }

                const auto ns = std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - started).count();

                if (pass > 0)
                    best = std::min (best, ns / (double) frames);
            }

            return best;
        }
    }

    class ClayBenchTmp  : public juce::UnitTest
    {
    public:
        ClayBenchTmp() : juce::UnitTest ("ClayBenchTmp", "Celadon") {}

        void runTest() override
        {
            beginTest ("ClayModule::process cost per stereo frame");

            const auto frames = (int) (20.0 * rate);
            const auto bass = offsetBass (frames);
            const auto music = programme (frames);

            struct Case { const char* name; ClayParams params; const Signal* signal; };
            const Case cases[] = {
                { "offset bass, low cut 20 Hz (guard pulls back most of the time)", makeParams (20.0f, 300.0f, 0.0f, 0.0f), &bass },
                { "offset bass, low cut 25 Hz + mono 120 Hz (manual-style, guard heavy)", makeParams (25.0f, 120.0f, 1.0f, 0.0f), &bass },
                { "programme, all stages (30 Hz / 150 Hz x 0.8 / +2 dB)", makeParams (30.0f, 150.0f, 0.8f, 2.0f), &music },
                { "programme, all stages neutral (idle fast path)", makeParams (0.0f, 300.0f, 0.0f, 0.0f), &music },
            };

            for (const auto& c : cases)
            {
                // 交替跑两轮取最小，免得先后顺序（频率爬升、缓存）占便宜。
                auto off = 1.0e30, on = 1.0e30;

                for (int round = 0; round < 3; ++round)
                {
                    off = std::min (off, nsPerFrame (c.params, *c.signal, false));
                   #ifdef BENCH_KEEP
                    on = std::min (on, nsPerFrame (c.params, *c.signal, true));
                   #endif
                }

               #ifdef BENCH_KEEP
                std::cout << "BENCH " << c.name << ": reporting off " << off << " ns/frame, on " << on << " ns/frame (+" << (100.0 * (on - off) / off) << " %)" << std::endl;
               #else
                std::cout << "BENCH " << c.name << ": " << off << " ns/frame" << std::endl;
               #endif
                expect (true);
            }
        }
    };

    static ClayBenchTmp clayBenchTmp;
}
