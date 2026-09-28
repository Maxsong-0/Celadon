// T-024 · 新旧 ClayModule 逐位对照（Linux 容器 / 服务器上跑，不进插件）。
//
// 新 = 工作树 Source/dsp/ClayModule.*（celadon::ClayModule）；旧 = 改前提交的同名文件，build.sh 把类名换成 ClayModuleRef 后并排编进来。
// 两个模块吃完全相同的调用序列：prepare / setParams / process / reset / 统计读取，块长随机（含 0 与 1）或固定。逐块比：
//   - 每个输出样本的位（含第 3 个声道：模块不处理，必须原样不动）；
//   - getLastProcessPressure() 两个字段的位；isPassThrough()；getLatencySamples()；
//   - 随机时刻读 getAndResetPeakGuardStats()，两个字段的位。
// 覆盖：4 个采样率 × 立体声 / 单声道 × 8 种素材 × 6 种参数日程 × 2 种块长模式；素材含 NaN / ±Inf / ±1e30 / 非正规数 / −0、
// 已限幅的密素材（守护频繁动作、跨 65536 重算网格）、静音里的突发（守护进出快路）。
// 任何一处不同：打印第一处、继续跑完其余用例，最后以非零退出。
//
// 用法：guard_compare [秒数，默认 3] [只跑名字里含这个子串的用例] [采样率列表，逗号分隔，默认 44100,48000,96000,192000] [ftz]
//   采样率列表另测低 / 怪 / 高采样率（分段长度 S 随 A 变：8 kHz 为 16、100 Hz 为 1）；ftz = 打开 x86 的 FTZ + DAZ
//   （插件在宿主里跑时 JUCE 的 ScopedNoDenormals 会打开，两边都在同一模式下比）。

#include "ClayModule.h"
#include "ClayModuleRef.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#if defined (__SSE__)
 #include <xmmintrin.h>
#endif

namespace
{
    using Signal = std::vector<std::vector<float>>;

    // 自带随机数（xorshift64*）：与编译器 / 标准库无关，任何机器上同一种子同一序列。
    struct Rng
    {
        std::uint64_t state;

        explicit Rng (std::uint64_t seed) : state (seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { next(); next(); }

        std::uint64_t next()
        {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            return state * 0x2545F4914F6CDD1Dull;
        }

        double uniform()                          { return static_cast<double> (next() >> 11) * 0x1.0p-53; }   // [0, 1)
        double between (double lo, double hi)     { return lo + (hi - lo) * uniform(); }
        int    below (int n)                      { return static_cast<int> (next() % static_cast<std::uint64_t> (n)); }
        bool   chance (double p)                  { return uniform() < p; }
        float  bipolar()                          { return static_cast<float> (2.0 * uniform() - 1.0); }
    };

    constexpr double twoPi = 6.283185307179586476925286766559;

    //==============================================================================
    // 素材（都生成 2 声道；单声道用例只取第 0 声道）。

    // 母带式：粉噪底 + 底鼓 / 军鼓 / 镲 + 持续音，峰值约 −1 dBFS，左右部分相关（照 tools/perf/bench.mm 的 makeSignal，随机源换成自带的）。
    Signal makeProgramme (int n, double fs, std::uint64_t seed)
    {
        Signal s (2, std::vector<float> (static_cast<size_t> (n)));
        Rng rng (seed);
        double b[2][7] = {};
        float prevWhite[2] = {};

        for (int i = 0; i < n; ++i)
        {
            const auto t = i / fs;
            const auto common = rng.bipolar();
            const auto kickT = std::fmod (t, 0.5);
            const auto kick = 0.55 * std::sin (twoPi * 55.0 * kickT) * std::exp (-kickT / 0.08);
            const auto snareT = std::fmod (t + 0.75, 1.0);
            const auto snareEnv = 0.30 * std::exp (-snareT / 0.03);
            const auto hatT = std::fmod (t, 0.125);
            const auto hatEnv = 0.12 * std::exp (-hatT / 0.01);
            const auto pad = 0.05 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 330.0 * t + 0.3) + std::sin (twoPi * 440.0 * t + 1.1));

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto white = 0.8f * common + 0.2f * rng.bipolar();
                auto* st = b[ch];
                st[0] = 0.99886 * st[0] + white * 0.0555179;
                st[1] = 0.99332 * st[1] + white * 0.0750759;
                st[2] = 0.96900 * st[2] + white * 0.1538520;
                st[3] = 0.86650 * st[3] + white * 0.3104856;
                st[4] = 0.55000 * st[4] + white * 0.5329522;
                st[5] = -0.7616 * st[5] - white * 0.0168980;
                const auto pink = (st[0] + st[1] + st[2] + st[3] + st[4] + st[5] + st[6] + white * 0.5362) * 0.11;
                st[6] = white * 0.115926;
                const auto hat = static_cast<double> (white - prevWhite[ch]) * 0.5;
                prevWhite[ch] = white;
                const auto v = pink + kick + snareEnv * white + hatEnv * hat + pad * (ch == 0 ? 1.0 : 0.9);
                s[static_cast<size_t> (ch)][static_cast<size_t> (i)] = static_cast<float> (std::fmax (-0.99, std::fmin (0.99, v)));
            }
        }

        return s;
    }

    // 已限幅的密素材：母带式 × 4 再硬削到 ±0.6（低切 / 收单的相位旋转会让峰值冒出来，守护频繁动作）。
    Signal makeDense (int n, double fs, std::uint64_t seed)
    {
        auto s = makeProgramme (n, fs, seed);

        for (auto& ch : s)
            for (auto& v : ch)
                v = std::fmax (-0.6f, std::fmin (0.6f, 4.0f * v));

        return s;
    }

    // 静音里的突发：随机长度的静音与随机电平（−60 ～ 0 dBFS）的噪声 + 低频正弦突发交替（P 的衰减、地板 F、守护进出快路）。
    Signal makeBursts (int n, double fs, std::uint64_t seed)
    {
        Signal s (2, std::vector<float> (static_cast<size_t> (n), 0.0f));
        Rng rng (seed);

        for (int i = 0; i < n;)
        {
            const auto silence = static_cast<int> (rng.between (0.001, 0.4) * fs);
            i += silence;
            const auto length = static_cast<int> (rng.between (0.0005, 0.3) * fs);
            const auto gain = std::pow (10.0, rng.between (-60.0, 0.0) / 20.0);
            const auto freq = rng.between (20.0, 200.0);
            const auto side = rng.uniform();

            for (int k = 0; k < length && i < n; ++k, ++i)
            {
                const auto tone = std::sin (twoPi * freq * k / fs);
                const auto l = gain * (0.6 * tone + 0.4 * rng.bipolar());
                const auto r = gain * ((1.0 - 2.0 * side) * 0.6 * tone + 0.4 * rng.bipolar());
                s[0][static_cast<size_t> (i)] = static_cast<float> (l);
                s[1][static_cast<size_t> (i)] = static_cast<float> (r);
            }
        }

        return s;
    }

    // 稀疏冲激：单样本尖峰（随机幅度与符号、左右独立或相同），其余为 0（E 的窗口边缘、单调队列的进出）。
    Signal makeSparse (int n, double fs, std::uint64_t seed)
    {
        Signal s (2, std::vector<float> (static_cast<size_t> (n), 0.0f));
        Rng rng (seed);

        for (int i = 0; i < n; i += 1 + static_cast<int> (rng.between (0.0, 0.02) * fs))
        {
            const auto a = rng.bipolar();
            s[0][static_cast<size_t> (i)] = a;
            s[1][static_cast<size_t> (i)] = rng.chance (0.5) ? a : rng.bipolar();
        }

        return s;
    }

    // 低电平：−50 dBFS 噪声，偶尔 −35 dBFS 的片段（地板 F = −40 dBFS 上下）。
    Signal makeLowLevel (int n, double fs, std::uint64_t seed)
    {
        Signal s (2, std::vector<float> (static_cast<size_t> (n)));
        Rng rng (seed);
        auto loud = 0;

        for (int i = 0; i < n; ++i)
        {
            if (loud == 0 && rng.chance (2.0 / fs))
                loud = static_cast<int> (rng.between (0.001, 0.05) * fs);

            const auto gain = loud > 0 ? 0.0178 : 0.00316;
            loud = loud > 0 ? loud - 1 : 0;
            const auto tone = std::sin (twoPi * 35.0 * i / fs);
            s[0][static_cast<size_t> (i)] = static_cast<float> (gain * (0.5 * tone + 0.5 * rng.bipolar()));
            s[1][static_cast<size_t> (i)] = static_cast<float> (gain * (-0.5 * tone + 0.5 * rng.bipolar()));
        }

        return s;
    }

    // 母带式 + 撒进去的怪值：NaN、±Inf、±1e30、非正规数、−0、±FLT_MAX。
    Signal makeExtremes (int n, double fs, std::uint64_t seed)
    {
        auto s = makeProgramme (n, fs, seed);
        Rng rng (seed + 99);
        const float specials[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                   -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f, 1.0e-40f, -1.0e-40f, -0.0f,
                                   std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), 0.0f };

        for (int i = 0; i < n; i += 1 + static_cast<int> (rng.between (0.0, 0.25) * fs))
        {
            const auto value = specials[rng.below (static_cast<int> (sizeof (specials) / sizeof (specials[0])))];
            const auto ch = rng.below (3);

            if (ch != 1) s[0][static_cast<size_t> (i)] = value;
            if (ch != 0) s[1][static_cast<size_t> (i)] = value;

            // 偶尔一整段 −0 / 非正规数（「逐位相同」要连符号位一起比）。
            if (rng.chance (0.1))
                for (int k = i; k < std::min (n, i + 200); ++k)
                    s[0][static_cast<size_t> (k)] = s[1][static_cast<size_t> (k)] = (rng.chance (0.5) ? -0.0f : 1.0e-41f);
        }

        return s;
    }

    // 侧声道为主：纯 S 的高频 + 低频（收单与加宽作用最强，加宽抬高 S 的高频 → 守护动作）。
    Signal makeSideHeavy (int n, double fs, std::uint64_t seed)
    {
        Signal s (2, std::vector<float> (static_cast<size_t> (n)));
        Rng rng (seed);

        for (int i = 0; i < n; ++i)
        {
            const auto t = i / fs;
            const auto side = 0.45 * std::sin (twoPi * 9000.0 * t) * (0.5 + 0.5 * std::sin (twoPi * 3.0 * t))
                            + 0.3 * std::sin (twoPi * 45.0 * t) + 0.05 * rng.bipolar();
            const auto mid = 0.2 * std::sin (twoPi * 110.0 * t);
            s[0][static_cast<size_t> (i)] = static_cast<float> (mid + side);
            s[1][static_cast<size_t> (i)] = static_cast<float> (mid - side);
        }

        return s;
    }

    // 正弦扫频 20 Hz → 20 kHz（对数），M 与 S 各一半，0.7 幅度。
    Signal makeSweep (int n, double fs, std::uint64_t)
    {
        Signal s (2, std::vector<float> (static_cast<size_t> (n)));
        const auto top = std::min (20000.0, 0.45 * fs);
        const auto k = std::log (top / 20.0) / (n / fs);
        auto phaseM = 0.0, phaseS = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const auto f = 20.0 * std::exp (k * i / fs);
            phaseM += twoPi * f / fs;
            phaseS += twoPi * 0.73 * f / fs;
            const auto m = 0.35 * std::sin (phaseM), sd = 0.35 * std::sin (phaseS);
            s[0][static_cast<size_t> (i)] = static_cast<float> (m + sd);
            s[1][static_cast<size_t> (i)] = static_cast<float> (m - sd);
        }

        return s;
    }

    struct SignalKind { const char* name; Signal (*make) (int, double, std::uint64_t); };

    const SignalKind signalKinds[] = { { "programme", makeProgramme }, { "dense", makeDense }, { "bursts", makeBursts },
                                       { "sparse", makeSparse }, { "lowlevel", makeLowLevel }, { "extremes", makeExtremes },
                                       { "sideheavy", makeSideHeavy }, { "sweep", makeSweep } };

    //==============================================================================
    // 参数日程：在绝对样本位置上换参数（换参数只发生在块开头；两个模块吃同一串调用，所以块长怎么切都公平）。
    using Params = celadon::ClayModule::Params;

    Params neutral() { return {}; }

    Params make (float lowCut, float monoHz, float mono, float width, bool bypass = false)
    {
        Params p;
        p.lowCutHz = lowCut;
        p.monoHz = monoHz;
        p.monoAmount = mono;
        p.widthDb = width;
        p.bypass = bypass;
        return p;
    }

    Params randomActive (Rng& rng)
    {
        Params p;
        p.lowCutHz   = rng.chance (0.5) ? static_cast<float> (rng.between (10.0, 60.0)) : 0.0f;
        p.monoHz     = static_cast<float> (rng.between (40.0, 300.0));
        p.monoAmount = rng.chance (0.5) ? (rng.chance (0.5) ? 1.0f : static_cast<float> (rng.uniform())) : 0.0f;
        p.widthDb    = rng.chance (0.5) ? static_cast<float> (rng.between (-3.0, 3.0)) : 0.0f;
        p.bypass     = rng.chance (0.1);

        if (p.lowCutHz == 0.0f && p.monoAmount == 0.0f && p.widthDb == 0.0f)
            p.widthDb = 3.0f;

        return p;
    }

    struct Event { std::int64_t at; Params params; };

    std::vector<Event> makeSchedule (const std::string& kind, std::int64_t n, double fs, Rng& rng)
    {
        std::vector<Event> events;

        if (kind == "neutral")
        {
            events.push_back ({ 0, neutral() });
        }
        else if (kind == "static-all")
        {
            events.push_back ({ 0, make (60.0f, 300.0f, 1.0f, 3.0f) });
        }
        else if (kind == "lowcut")
        {
            events.push_back ({ 0, make (static_cast<float> (rng.between (10.0, 60.0)), 80.0f, 0.0f, 0.0f) });
        }
        else if (kind == "width")
        {
            // +3 dB 加宽开着 / 关掉交替（关掉后高架余振归零 → 逐位纯延迟 → 守护走快路；再打开 → 出快路）。
            for (std::int64_t at = 0; at < n; at += static_cast<std::int64_t> (rng.between (0.02, 0.4) * fs))
                events.push_back ({ at, make (0.0f, 80.0f, 0.0f, events.size() % 2 == 0 ? 3.0f : 0.0f) });
        }
        else if (kind == "random")
        {
            for (std::int64_t at = 0; at < n; at += 1 + static_cast<std::int64_t> (rng.between (0.0, 0.3) * fs))
                events.push_back ({ at, rng.chance (0.25) ? neutral() : randomActive (rng) });
        }
        else if (kind == "transitions")
        {
            // 长段中性 ↔ 短段工作（含只开几个样本、淡化没走完就回中性），反复进出快路。
            auto on = false;

            for (std::int64_t at = 0; at < n;)
            {
                events.push_back ({ at, on ? randomActive (rng) : neutral() });
                at += on ? 1 + static_cast<std::int64_t> (rng.between (0.0, 0.3) * fs)
                         : 1 + static_cast<std::int64_t> (rng.between (0.0, 0.5) * fs);
                on = ! on;
            }
        }

        return events;
    }

    const char* scheduleKinds[] = { "neutral", "static-all", "lowcut", "width", "random", "transitions" };

    //==============================================================================
    bool sameBits (float a, float b)
    {
        std::uint32_t x = 0, y = 0;
        std::memcpy (&x, &a, 4);
        std::memcpy (&y, &b, 4);
        return x == y;
    }

    bool sameBits (double a, double b)
    {
        std::uint64_t x = 0, y = 0;
        std::memcpy (&x, &a, 8);
        std::memcpy (&y, &b, 8);
        return x == y;
    }

    struct Totals { std::int64_t cases = 0, failed = 0, samples = 0, blocks = 0, guarded = 0, fastEntries = 0; };

    // 一个用例：两个模块吃同一串调用，逐块比较。返回 true = 全部相同。
    bool runCase (const std::string& name, double fs, int channels, const Signal& input, const std::vector<Event>& events,
                  bool randomBlocks, int fixedBlock, std::uint64_t seed, Totals& totals)
    {
        Rng rng (seed);
        celadon::ClayModule fresh;
        celadon::ClayModuleRef ref;

        auto sampleRate = fs;
        const auto prepareBoth = [&] (double rate, int maxBlock)
        {
            juce::dsp::ProcessSpec spec { rate, static_cast<juce::uint32> (maxBlock), static_cast<juce::uint32> (channels) };
            fresh.prepare (spec);
            ref.prepare (spec);
        };

        const auto toRef = [] (const Params& p)
        {
            celadon::ClayModuleRef::Params q;
            q.lowCutHz = p.lowCutHz;
            q.monoHz = p.monoHz;
            q.monoAmount = p.monoAmount;
            q.widthDb = p.widthDb;
            q.bypass = p.bypass;
            return q;
        };

        // prepare 之前的查询也要一样（模块刚构造出来）。
        if (fresh.isPassThrough() != ref.isPassThrough() || fresh.getLatencySamples() != ref.getLatencySamples())
        {
            std::printf ("MISMATCH %s before prepare: isPassThrough new %d old %d\n", name.c_str(), (int) fresh.isPassThrough(), (int) ref.isPassThrough());
            return false;
        }

        prepareBoth (sampleRate, 512);

        const auto n = static_cast<std::int64_t> (input[0].size());
        size_t nextEvent = 0;
        std::int64_t pos = 0;
        auto blockIndex = 0;
        auto reprepared = false;
        std::int64_t guardedTotal = 0;

        const auto fail = [&] (const std::string& what)
        {
            std::printf ("MISMATCH %s @ block %d (sample %lld): %s\n", name.c_str(), blockIndex, static_cast<long long> (pos), what.c_str());
            return false;
        };

        if (fresh.getLatencySamples() != ref.getLatencySamples())
            return fail ("latency after prepare");

        while (pos < n)
        {
            // 这一块从 pos 开始：先把到 pos 为止的参数事件都交给两个模块（最后一个生效，同宿主每块设一次）。
            while (nextEvent < events.size() && events[nextEvent].at <= pos)
            {
                fresh.setParams (events[nextEvent].params);
                ref.setParams (toRef (events[nextEvent].params));
                ++nextEvent;
            }

            // 偶发：reset（宿主 Reset / NaN 恢复），跑到一半换采样率重新 prepare（只一次）。
            if (rng.chance (0.002))
            {
                fresh.reset();
                ref.reset();
            }

            if (! reprepared && pos > n / 2 && rng.chance (0.05))
            {
                reprepared = true;
                sampleRate = sampleRate == 48000.0 ? 96000.0 : 48000.0;
                prepareBoth (sampleRate, 256);

                if (fresh.getLatencySamples() != ref.getLatencySamples())
                    return fail ("latency after re-prepare");
            }

            auto length = fixedBlock;

            if (randomBlocks)
            {
                const auto r = rng.uniform();
                length = r < 0.03 ? 0 : r < 0.25 ? 1 + rng.below (8) : r < 0.7 ? 1 + rng.below (600) : 1 + rng.below (4096);
            }

            length = static_cast<int> (std::min<std::int64_t> (length, n - pos));

            // 缓冲的声道数：通常 = 模块声道数；偶尔多一个模块不处理的第 3 声道（必须原样不动）。
            // 不测「立体声模块收到单声道缓冲」：改前的守护在那里按 prepare 的 2 声道读写只有 1 个元素的栈数组（越界，未定义行为），
            // 没有可比的参照；插件里走不到（处理器按输出声道数 prepare，进出声道数相同）。改后按调用方的声道数处理，不越界。
            auto bufferChannels = channels;

            if (channels == 2 && rng.chance (0.01))
                bufferChannels = 3;

            juce::AudioBuffer<float> a (bufferChannels, length), b (bufferChannels, length);

            for (int ch = 0; ch < bufferChannels; ++ch)
                for (int i = 0; i < length; ++i)
                {
                    const auto v = ch < 2 ? input[static_cast<size_t> (ch)][static_cast<size_t> (pos + i)] : 0.25f * static_cast<float> (i % 7);
                    a.setSample (ch, i, v);
                    b.setSample (ch, i, v);
                }

            fresh.process (a);
            ref.process (b);

            for (int ch = 0; ch < bufferChannels; ++ch)
                for (int i = 0; i < length; ++i)
                    if (! sameBits (a.getSample (ch, i), b.getSample (ch, i)))
                        return fail ("output ch " + std::to_string (ch) + " sample " + std::to_string (i) + " new "
                                     + std::to_string (a.getSample (ch, i)) + " old " + std::to_string (b.getSample (ch, i)));

            const auto pa = fresh.getLastProcessPressure();
            const auto pb = ref.getLastProcessPressure();

            if (! sameBits (pa.meanDb, pb.meanDb) || ! sameBits (pa.seconds, pb.seconds))
                return fail ("pressure new " + std::to_string (pa.meanDb) + " dB / " + std::to_string (pa.seconds) + " s, old "
                             + std::to_string (pb.meanDb) + " dB / " + std::to_string (pb.seconds) + " s");

            if (fresh.isPassThrough() != ref.isPassThrough())
                return fail (std::string ("isPassThrough new ") + (fresh.isPassThrough() ? "1" : "0"));

            if (fresh.getLatencySamples() != ref.getLatencySamples())
                return fail ("latency");

            if (rng.chance (0.05) || pos + length >= n)
            {
                const auto sa = fresh.getAndResetPeakGuardStats();
                const auto sb = ref.getAndResetPeakGuardStats();

                if (sa.guardedSamples != sb.guardedSamples || ! sameBits (sa.minGain, sb.minGain))
                    return fail ("guard stats new " + std::to_string (sa.guardedSamples) + " / " + std::to_string (sa.minGain) + ", old "
                                 + std::to_string (sb.guardedSamples) + " / " + std::to_string (sb.minGain));

                guardedTotal += sa.guardedSamples;
            }

            pos += length;
            ++blockIndex;
        }

        totals.samples += n;
        totals.blocks += blockIndex;
        totals.guarded += guardedTotal;
        return true;
    }
}

int main (int argc, char** argv)
{
    const auto seconds = argc > 1 ? std::atof (argv[1]) : 3.0;
    const std::string filter = argc > 2 && std::string (argv[2]) != "-" ? argv[2] : "";
    std::vector<double> rates { 44100.0, 48000.0, 96000.0, 192000.0 };

    if (argc > 3)
    {
        rates.clear();
        std::stringstream list (argv[3]);

        for (std::string item; std::getline (list, item, ',');)
            rates.push_back (std::atof (item.c_str()));
    }

    if (argc > 4 && std::string (argv[4]) == "ftz")
    {
       #if defined (__SSE__)
        _mm_setcsr (_mm_getcsr() | 0x8040);   // FTZ | DAZ
        std::printf ("FTZ + DAZ on\n");
       #else
        std::printf ("ftz requested but not available on this target\n");
        return 2;
       #endif
    }

    Totals totals;
    std::uint64_t seed = 20260928;

    for (const auto fs : rates)
    {
        // 高采样率按同样的秒数会很长：样本数封顶在 3 秒 192 kHz 的量。低采样率至少 2 万个样本（跨过好几段、好几格代价格）。
        const auto n = static_cast<int> (std::max (20000.0, std::min (seconds * fs, seconds * 192000.0)));

        for (const auto& kind : signalKinds)
        {
            const auto signal = kind.make (n, fs, seed++);

            for (const auto* schedule : scheduleKinds)
                for (const auto channels : { 2, 1 })
                    for (const auto randomBlocks : { true, false })
                    {
                        const auto fixedBlock = fs >= 96000.0 ? 32 : 512;
                        const auto name = std::string (kind.name) + "/" + schedule + "/" + (channels == 2 ? "stereo" : "mono") + "/"
                                        + (randomBlocks ? "randomBlocks" : "block" + std::to_string (fixedBlock)) + "@" + std::to_string (static_cast<int> (fs));

                        if (! filter.empty() && name.find (filter) == std::string::npos)
                            continue;

                        Rng scheduleRng (seed * 31 + static_cast<std::uint64_t> (channels));
                        const auto events = makeSchedule (schedule, n, fs, scheduleRng);
                        Signal input = signal;

                        if (channels == 1)
                            input.resize (1);

                        ++totals.cases;
                        const auto before = totals.guarded;

                        if (! runCase (name, fs, channels, input, events, randomBlocks, fixedBlock, seed * 7 + (randomBlocks ? 1u : 0u), totals))
                            ++totals.failed;
                        else
                            std::printf ("ok %s guarded=%lld\n", name.c_str(), static_cast<long long> (totals.guarded - before));

                        ++seed;
                    }
        }
    }

    std::printf ("==== guard_compare: %lld cases, %lld failed; %lld samples in %lld blocks compared bit for bit; guarded samples %lld ====\n",
                 static_cast<long long> (totals.cases), static_cast<long long> (totals.failed), static_cast<long long> (totals.samples),
                 static_cast<long long> (totals.blocks), static_cast<long long> (totals.guarded));
    return totals.failed == 0 ? 0 : 1;
}
