// T-027 微基准：整条链 CeladonChain 在 192 kHz / 块 32、GLAZE 60 下每块的耗时（107，与测试同一套 GCC Release 选项）。
// 同一份链源码与目标文件，只换 GlazeController.o（改前 = T-027 的父提交，改后 = T-027），见 run-bench.sh；交替跑多轮取中位数。
//
// 做法：合成一段 8 s 的循环素材（底鼓 / 军鼓 / 镲 / 贝斯 / 铺底 + 粉噪，确定性），按真实常数先播放 warm 秒让 GLAZE 聆听、
// 提交、校准，再计时 timed 秒。模式 reopen：暖机后存档，新链读回存档（执行快照被沿用，beginBlock 每块多比一次参考条件），
// 在新链上计时——makeReference 每块 3 次的情形；默认模式是每块 2 次（captureExecution、publishState）。
//
// 用法：refbench <warm 秒> <timed 秒> [steady|reopen]
#include "CeladonChain.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace celadon;

namespace
{
    constexpr double rate = 192000.0;
    constexpr int block = 32;
    constexpr int channels = 2;

    std::vector<std::vector<float>> makeLoop (double seconds)
    {
        const auto n = (size_t) std::ceil (rate * seconds);
        std::vector<std::vector<float>> s ((size_t) channels, std::vector<float> (n));
        juce::Random random (20260929);
        const auto twoPi = juce::MathConstants<double>::twoPi;
        double pink[2] = {};

        for (size_t i = 0; i < n; ++i)
        {
            const auto t = (double) i / rate;
            const auto kickTime = std::fmod (t, 0.5);
            const auto kick = 0.55 * std::sin (twoPi * 55.0 * kickTime) * std::exp (-kickTime / 0.08);
            const auto snare = 0.30 * std::exp (-std::fmod (t + 0.75, 1.0) / 0.03) * (random.nextFloat() * 2.0 - 1.0);
            const auto hat = 0.12 * std::exp (-std::fmod (t, 0.125) / 0.01) * (random.nextFloat() * 2.0 - 1.0);
            const auto bass = 0.20 * std::sin (twoPi * (t < seconds * 0.5 ? 41.2 : 49.0) * t);
            const auto pad = 0.05 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 330.0 * t + 0.3) + std::sin (twoPi * 440.0 * t + 1.1));

            for (int ch = 0; ch < channels; ++ch)
            {
                pink[ch] = 0.995 * pink[ch] + 0.05 * (random.nextFloat() * 2.0 - 1.0);
                const auto side = ch == 0 ? 0.03 : -0.03;
                s[(size_t) ch][i] = (float) (kick + snare + hat + bass + pad * (1.0 + side) + 0.04 * pink[ch]);
            }
        }

        return s;
    }

    struct Player
    {
        std::unique_ptr<CeladonChain> chain = std::make_unique<CeladonChain>();
        ChainParams params;
        glaze::HostInfo host;
        juce::AudioBuffer<float> buffer { channels, block };
        const std::vector<std::vector<float>>* loop = nullptr;
        size_t index = 0;
        std::int64_t position = 0;

        void prepare()
        {
            params.limit.ceilingDb = -1.0f;
            params.glazePercent = 60.0f;
            host.hasPlayState = true;
            host.isPlaying = true;
            host.hasPosition = true;
            chain->setParams (params, false);
            chain->prepare ({ rate, (juce::uint32) block, (juce::uint32) channels });
        }

        // 与处理器 processBlock 同一顺序：宿主信息 → 参数 → 处理。
        void run (std::int64_t blocks)
        {
            const auto length = loop->front().size();

            for (std::int64_t b = 0; b < blocks; ++b)
            {
                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* out = buffer.getWritePointer (ch);

                    for (int i = 0; i < block; ++i)
                        out[i] = (*loop)[(size_t) ch][(index + (size_t) i) % length];
                }

                index = (index + (size_t) block) % length;
                host.positionSamples = position;
                chain->setHostInfo (host);
                chain->setParams (params, false);
                chain->process (buffer);
                position += block;
            }
        }
    };

    std::int64_t blocksFor (double seconds)   { return (std::int64_t) std::ceil (seconds * rate / block); }

    const char* phaseName (glaze::Phase p)
    {
        switch (p)
        {
            case glaze::Phase::unlearned:      return "unlearned";
            case glaze::Phase::listening:      return "listening";
            case glaze::Phase::planTransition: return "planTransition";
            case glaze::Phase::calibrating:    return "calibrating";
            case glaze::Phase::locked:         return "locked";
            default:                           return "?";
        }
    }
}

int main (int argc, char** argv)
{
    const auto warmSeconds = argc > 1 ? std::atof (argv[1]) : 90.0;
    const auto timedSeconds = argc > 2 ? std::atof (argv[2]) : 60.0;
    const auto reopen = argc > 3 && std::strcmp (argv[3], "reopen") == 0;

    const auto loop = makeLoop (8.0);

    Player player;
    player.loop = &loop;
    player.prepare();
    player.run (blocksFor (warmSeconds));

    Player timed;
    Player* target = &player;

    if (reopen)
    {
        // 存档 → 新链读回（先交参数再 prepare，与处理器一样），接着原来的位置播：执行快照被沿用。
        const auto xml = player.chain->createGlazeStateXml();
        timed.loop = &loop;
        timed.index = player.index;
        timed.position = player.position;
        timed.prepare();
        timed.chain->restoreGlazeStateXml (xml.get());
        target = &timed;
    }

    const auto before = target->chain->getGlazeStatus();
    const auto blocks = blocksFor (timedSeconds);
    const auto t0 = std::chrono::steady_clock::now();
    target->run (blocks);
    const auto t1 = std::chrono::steady_clock::now();
    const auto after = target->chain->getGlazeStatus();

    const auto nsPerBlock = std::chrono::duration<double, std::nano> (t1 - t0).count() / (double) blocks;
    const auto blockNs = block / rate * 1.0e9;

    std::printf ("%s warm=%.0fs timed=%.0fs blocks=%lld phase %s->%s calib %d->%d exec %d  %.1f ns/block  %.3f %% of real time\n",
                 reopen ? "reopen" : "steady", warmSeconds, timedSeconds, (long long) blocks,
                 phaseName (before.phase), phaseName (after.phase), (int) before.calibrationValid, (int) after.calibrationValid,
                 (int) target->chain->getGlazeController().getProbeForTesting().executionSnapshotActive,
                 nsPerBlock, 100.0 * nsPerBlock / blockNs);
    return 0;
}
