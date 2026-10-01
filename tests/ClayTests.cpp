/*
  ==============================================================================

    ClayTests.cpp
    修坯 Clay 的信号处理验收（设计规格-Clay 第 4 节；设计规格-Clay-分析决策与验收 第 6 节第 2 条；v0.1.7.0 阶段 4 起第②级按
    设计规格-Clay-根治修订 第 2 节改为线性相位收单、模块有固定延迟 D，验收见根治修订第 4 节第 3、4 条）：
      - 固定延迟 = D + G：D = 2(L_max − 1)（线性相位收单）、G = W + A（ADR-014 峰值守护前瞻，W = round(0.008 fs)、A = round(0.003 fs)）；
        44.1 / 48 / 96 / 192 kHz = 1013 / 1102 / 2206 / 4414，与参数、旁通、声道数无关；冲激实测（下文的「D」都指整个模块的延迟 D + G）
      - 三项实际值都为 0 → 输出 = 输入逐位延迟 D（含 ±0 与各种极端有限值 / ±Inf，1 ～ 4096 各种块长）；各级回到 0 后重新逐位纯延迟
      - 低切：−3 dB 点与设定频率偏差 ≤ 1 %（10 / 25 / 60 Hz × 44.1 / 48 / 96 / 192 kHz）；截止 25 Hz 时 50 Hz 以上变化 ≤ 0.1 dB
      - 收单：a = 1、分频 120 Hz 时 50 Hz 纯 S 正弦衰减 ≥ 12 dB；a = 1 / 0.5 时与线性相位互补 |1 − a·H₀| 的解析值一致；
        a = 1 时纯 S 的冲激响应以 D 为中心对称（≤ 1e-9：线性相位），D − 2(L − 1) 之前恰为 0
      - 分频点变化：旧 / 新两套低通 20 ms 线性交叉淡化，之后与一开始就设在新分频点的模块一致；拖旋钮（每 64 样本一个新值）无咔哒
      - 开收单（a 0 → 1）：之前逐位纯延迟、20 ms 线性淡化、之后与一直开着的模块逐位相同（低通一直在影子里跑，没有起跳）
      - 纯 M 不产生 S（≤ −120 dBFS）；左右互换对称（逐位）
      - 单声道只做低切（再延迟 D）
      - 加宽：高架增益误差 ±0.1 dB（16 kHz = widthDb、2 kHz = widthDb / 2），M 与低频 S 不受影响
      - 1 / 17 / 333 / 2048 / 4096 块长输出一致：参数静止时逐位；参数在同一样本位置扫动时同样逐位（容差 0）
      - 参数扫动无咔哒：任一参数 20 ms 内从最小扫到最大（及反向），粉噪下输出 4 kHz 以上 5 ms 能量相对静止参数同段不突起 > 1 dB
      - 旁通 = 同样延迟 D 的干路 + 20 ms 交叉淡化，解除旁通后与从未旁通逐位相同（影子运行）；三级全关时切旁通仍是逐位纯延迟
      - 低切关 / 开的 20 ms 输出淡化（低切在延迟之前，淡化晚 D 个样本到输出）与影子运行
      - reset、音频路径 0 分配、输入里的 NaN 不会让影子滤波器卡死
      - 峰值守护（ADR-014；T-032 2026-09-29 修订：只管①②，③ 加宽的增量绕过守护）：①② 的输出逐样本不越界（立体声两组、单声道一组，
        各带「不带守护会越界」的阳性对照）、③ 的增量原样叠在守护输出上（加宽与否守护统计、代价格逐位相同；只开加宽拧回 0 时
        isPassThrough 等 δ 的尾巴走出前瞻环才报真；δ 恰为 0 的样本不做加法，−0 仍是 −0）；稳态低音与纯 S 高频加宽不误动作、谐波对经低切抬高峰值时动作；正常电平（守护
        地板之上）下加宽 +3 / +1.5 / −3 dB 的高频侧声道增益 = widthDb ± 0.3 dB 且守护统计为 0；低切 / 收单在固定种子夹具上的效果钉在
        记录值（回归护栏）；widthDb = 0 的一批场景（怪值、随机块长、参数扫动、四个采样率）的输出摘要——改守护前后逐位对照的依据
        （摘要值记在 T-032 报告里）
    另有测试类 LinearPhaseLowpass（滑动平均级联本身）：长度与延迟公式；冲激响应 = 精确系数、以 2(L − 1) 为中心对称、直流增益 1；
    互补高通在 fc/2、fc、2fc、4fc 的增益；跨重算网格与朴素卷积一致；静音后状态精确归零；预热等价；输入清洗；0 分配。

    只按规格与 ClayModule.h / LinearPhaseLowpass.h 的公开接口写，不依赖实现细节（只用到头文件写明的「分频点切换在 64 样本网格点
    上开始」）；每条断言的数字依据写在对应函数的注释里。解析值（Butterworth 响应、RBJ 高架在 f0 / 平台处的增益、滑动平均级联的
    零相位响应与精确系数、规格的长度 / 延迟公式）在本文件里另行推导，与被测代码不共享任何实现。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/ClayModule.h"
#include "../Source/dsp/LinearPhaseLowpass.h"

#include <cmath>
#include <complex>
#include <cstring>
#include <limits>

namespace celadon::test
{
    namespace
    {
        using ClayParams = ClayModule::Params;

        // prepare 时的块长；测试里也送 2048 / 4096 这样更大的块（头文件「样本数可以超过 prepare 时的块大小」）。
        constexpr int maxBlock = 512;

        // 头文件：「分频点变化……在之后的网格点再处理」，网格 = 从 reset 起每 64 样本。切换最晚在参数改变后 64 个样本内开始。
        constexpr int switchGrid = 64;

        ClayParams makeParams (float lowCutHz, float monoHz, float monoAmount, float widthDb, bool bypass = false)
        {
            ClayParams p;
            p.lowCutHz   = lowCutHz;
            p.monoHz     = monoHz;
            p.monoAmount = monoAmount;
            p.widthDb    = widthDb;
            p.bypass     = bypass;
            return p;
        }

        // 三级全关（monoHz 故意取非默认的 300 Hz：a = 0 时分频点不影响输出）与三级全开的常用参数。
        ClayParams neutralParams()    { return makeParams (0.0f, 300.0f, 0.0f, 0.0f); }
        ClayParams allStagesParams()  { return makeParams (30.0f, 150.0f, 0.8f, 2.0f); }

        // 先 setParams 再 prepare（prepare → reset 把平滑器直接落到目标），prepare 之后再设一次是保险（同 BalanceTests）。
        void prepareClay (ClayModule& clay, double sampleRate, int channels, const ClayParams& p)
        {
            clay.setParams (p);
            clay.prepare ({ sampleRate, static_cast<juce::uint32> (maxBlock), static_cast<juce::uint32> (channels) });
            clay.setParams (p);
        }

        // 参数时间表：在第 atSample 个样本之前调 setParams（按 atSample 升序）。
        struct ParamChange
        {
            int atSample = 0;
            ClayParams params;
        };

        using Schedule = std::vector<ParamChange>;

        // 按块长 blockSize 把整段信号送进模块，返回输出。块在改参数的位置切开：任何块长下参数都在同一个样本位置生效
        // （宿主也只能在块边界改参数），「块长无关」比较的就是模块自己。
        Signal runClay (ClayModule& clay, const Signal& input, int blockSize, const Schedule& changes = {})
        {
            const auto channels   = static_cast<int> (input.size());
            const auto numSamples = static_cast<int> (input.front().size());
            auto output = input;

            juce::AudioBuffer<float> block (channels, blockSize);
            size_t nextChange = 0;

            for (int start = 0; start < numSamples;)
            {
                while (nextChange < changes.size() && changes[nextChange].atSample <= start)
                    clay.setParams (changes[nextChange++].params);

                auto length = juce::jmin (blockSize, numSamples - start);

                if (nextChange < changes.size())
                    length = juce::jmin (length, changes[nextChange].atSample - start);

                block.setSize (channels, length, false, false, true);

                for (int ch = 0; ch < channels; ++ch)
                    block.copyFrom (ch, 0, input[static_cast<size_t> (ch)].data() + start, length);

                clay.process (block);

                for (int ch = 0; ch < channels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                               output[static_cast<size_t> (ch)].begin() + start);

                start += length;
            }

            return output;
        }

        // 逐位比较 [from, to)（to < 0 = 到结尾；memcmp：连 ±0 的符号、NaN 的载荷也比），返回不同的样本数。
        int countDifferentBits (const Signal& a, const Signal& b, int from = 0, int to = -1)
        {
            int different = 0;

            for (size_t ch = 0; ch < a.size(); ++ch)
            {
                const auto end = to < 0 ? static_cast<int> (a[ch].size()) : to;

                for (int i = from; i < end; ++i)
                {
                    const auto index = static_cast<size_t> (i);
                    different += std::memcmp (&a[ch][index], &b[ch][index], sizeof (float)) != 0 ? 1 : 0;
                }
            }

            return different;
        }

        //==============================================================================
        // 规格公式（设计规格-Clay-根治修订 第 2 节、ADR-014），与被测实现分开写一遍：L = max(2, round(0.24 · fs / fc))，群延迟 2(L − 1)；
        // Clay 的固定延迟 = 2(L_max − 1)（L_max 按手动收单下限 40 Hz 算）+ 峰值守护前瞻 round(0.008 fs) + round(0.003 fs)。
        int specLength (double sampleRate, double cutoffHz)
        {
            return juce::jmax (2, static_cast<int> (std::lround (0.24 * sampleRate / cutoffHz)));
        }

        // 峰值守护的前瞻 G = round(0.008 fs) + round(0.003 fs)（ADR-014）：作用在①～③ 输出时刻的量（a、加宽、旁通淡化、分频点切换的
        // 淡化）比原来再晚 G 个样本出现在模块输出上（守护之后的那段 float 环）。宿主按上报的整体延迟补偿后，它们与对齐后的音频的相对
        // 时刻不变。
        int specGuardLookahead (double sampleRate)
        {
            return static_cast<int> (std::lround (0.008 * sampleRate)) + static_cast<int> (std::lround (0.003 * sampleRate));
        }

        int specDelay (double sampleRate)
        {
            return 2 * (specLength (sampleRate, 40.0) - 1)
                 + static_cast<int> (std::lround (0.008 * sampleRate)) + static_cast<int> (std::lround (0.003 * sampleRate));
        }

        // 整段信号延迟 delay 个样本（长度不变，头 delay 个样本为 +0——模块的延迟线初值是 0）：逐位纯延迟时的期望输出。
        Signal delayedBy (const Signal& signal, int delay)
        {
            auto out = makeSilence (static_cast<int> (signal.size()), static_cast<int> (signal.front().size()));

            for (size_t ch = 0; ch < signal.size(); ++ch)
            {
                const auto channelLength = static_cast<int> (signal[ch].size());

                for (int i = delay; i < channelLength; ++i)
                    out[ch][static_cast<size_t> (i)] = signal[ch][static_cast<size_t> (i - delay)];
            }

            return out;
        }

        // 4 个长度 L 的滑动平均级联的零相位响应（实数）：每个矩形窗 = e^{−jω(L−1)/2} · sin(ωL/2) / (L·sin(ω/2))，四个相乘、
        // 去掉线性相位项（它正是 Clay 用来对齐 S_d 的 2(L − 1)）。Clay 对纯 S 正弦的增益 = |1 − a·H₀(f)|。
        double lowpassZeroPhase (double frequency, double sampleRate, int length)
        {
            const auto halfOmega = juce::MathConstants<double>::pi * frequency / sampleRate;
            const auto dirichlet = std::sin (halfOmega * static_cast<double> (length))
                                   / (static_cast<double> (length) * std::sin (halfOmega));
            return std::pow (dirichlet, 4.0);
        }

        //==============================================================================
        // 粉噪：Paul Kellet 的 7 极点近似（同 BalanceTests），9 Hz ～ 0.45 fs 内与 −3 dB/oct 的偏差 ±0.05 dB。
        // 白噪来自固定种子的 juce::Random，各声道种子不同（互不相关）；先空跑 8192 样本进稳态，再把每声道归一到指定 RMS。
        Signal makePinkNoise (int channels, int numSamples, float rmsDbfs, juce::int64 seed)
        {
            constexpr int warmUp = 8192;
            auto signal = makeSilence (channels, numSamples);
            const auto targetRms = static_cast<double> (juce::Decibels::decibelsToGain (rmsDbfs));

            for (int ch = 0; ch < channels; ++ch)
            {
                juce::Random random (seed + static_cast<juce::int64> (ch) * 7919);
                double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;
                auto& channel = signal[static_cast<size_t> (ch)];
                double sumOfSquares = 0.0;

                for (int i = -warmUp; i < numSamples; ++i)
                {
                    const auto white = static_cast<double> (random.nextFloat()) * 2.0 - 1.0;

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
                        channel[static_cast<size_t> (i)] = static_cast<float> (pink);
                        sumOfSquares += pink * pink;
                    }
                }

                const auto gain = static_cast<float> (targetRms / std::sqrt (sumOfSquares / static_cast<double> (numSamples)));

                for (auto& sample : channel)
                    sample *= gain;
            }

            return signal;
        }

        // 给一个声道叠加正弦（样本数精确，不经 seconds × fs 的取整）。
        void addSine (std::vector<float>& channel, double sampleRate, double frequency, float amplitude)
        {
            for (size_t i = 0; i < channel.size(); ++i)
                channel[i] += amplitude * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequency
                                                                        * static_cast<double> (i) / sampleRate));
        }

        // 各声道相同的正弦（纯 M），长度精确为 numSamples。
        Signal makeSineOfLength (int channels, double sampleRate, double frequency, float amplitude, int numSamples)
        {
            auto signal = makeSilence (channels, numSamples);

            for (auto& channel : signal)
                addSine (channel, sampleRate, frequency, amplitude);

            return signal;
        }

        // 粉噪 + 30 Hz 正弦（左 0.3、右 0.15）：低切、收单确实有东西可动，两声道不相关让 S 不为 0。
        Signal makeProgramme (int numSamples, double sampleRate, float pinkRmsDbfs, juce::int64 seed)
        {
            auto signal = makePinkNoise (2, numSamples, pinkRmsDbfs, seed);
            addSine (signal[0], sampleRate, 30.0, 0.3f);
            addSine (signal[1], sampleRate, 30.0, 0.15f);
            return signal;
        }

        // ADR-014 峰值守护的绝对地板 F = −40 dBFS（0.01）：①～③ 的输出都在它之下时守护不介入（界 B ≥ F ≥ |y| → g 精确为 1 →
        // 输出逐位 = ①～③ 的输出）。测①～③核心机制（分频点切换、开收单、旁通、加宽增益）的用例把素材整体压低 quietScale（−44 dB：
        // −18 dBFS 粉噪 + 0.3 / 0.15 的 30 Hz 正弦压低后峰值约 −46 dBFS，全开的加宽 / 收单后仍低于地板），断言保持原强度——逐位的
        // 仍逐位，绝对容差按同一比例缩小。守护本身另见 testPeakGuard*。
        constexpr float quietScale = 0.0063095734f;   // 10^(−44/20)

        Signal quieter (Signal signal)
        {
            for (auto& channel : signal)
                for (auto& sample : channel)
                    sample *= quietScale;

            return signal;
        }

        // 整段乘一个增益。T-032 的守护用例用 2 的幂（2^−10 缩小、2^10 放大回来）：float / double 里都是精确运算（不下溢、不上溢时），
        // ①～③ 又是线性的，所以「缩小 → 过模块 → 放大回来」与不带守护的模块输出逐位同源（守护在缩小后的电平上不介入）。
        Signal scaledBy (Signal signal, float gain)
        {
            for (auto& channel : signal)
                for (auto& sample : channel)
                    sample *= gain;

            return signal;
        }

        // NaN 以外的特殊值：±0、±最小正规数、±最小非正规数、±1e30、±FLT_MAX、±Inf，散布在各处；再加一段 256 个交替的 ±0。
        void sprinkleSpecialValues (Signal& signal)
        {
            using Limits = std::numeric_limits<float>;
            const float specials[] = { 0.0f, -0.0f, Limits::min(), -Limits::min(), Limits::denorm_min(), -Limits::denorm_min(),
                                       1.0e30f, -1.0e30f, Limits::max(), -Limits::max(), Limits::infinity(), -Limits::infinity() };
            constexpr int numSpecials = static_cast<int> (sizeof (specials) / sizeof (specials[0]));

            for (size_t ch = 0; ch < signal.size(); ++ch)
            {
                auto& channel = signal[ch];
                const auto channelLength = static_cast<int> (channel.size());
                const auto channelIndex  = static_cast<int> (ch);

                for (int k = 0; k < 240; ++k)
                {
                    const auto index = (k * 193 + channelIndex * 71 + 11) % channelLength;
                    channel[static_cast<size_t> (index)] = specials[(k + channelIndex) % numSpecials];
                }

                for (int i = 0; i < 256; ++i)
                    channel[static_cast<size_t> (channelLength / 3 + i)] = (i % 2 == 0) ? 0.0f : -0.0f;
            }
        }

        std::vector<double> channelAsDouble (const Signal& signal, size_t channel)
        {
            std::vector<double> out (signal[channel].size());

            for (size_t i = 0; i < out.size(); ++i)
                out[i] = static_cast<double> (signal[channel][i]);

            return out;
        }

        // S = (L − R) / 2（double）。
        std::vector<double> sideOf (const Signal& signal)
        {
            std::vector<double> out (signal[0].size());

            for (size_t i = 0; i < out.size(); ++i)
                out[i] = 0.5 * (static_cast<double> (signal[0][i]) - static_cast<double> (signal[1][i]));

            return out;
        }

        // 已知频率的正弦最小二乘拟合 y ≈ a·cos(ωn) + b·sin(ωn)，返回幅度 √(a² + b²)。对纯正弦在任意窗长、任意起始相位下都精确
        // （不受非整数周期的泄漏影响；输出比输入晚 D 个样本只改变相位，不改变拟合出的幅度）；测增益时输入、输出用同一个窗、
        // 同一个函数，再相除。
        double fittedAmplitude (const std::vector<double>& x, int start, int length, double frequency, double sampleRate)
        {
            const auto w = juce::MathConstants<double>::twoPi * frequency / sampleRate;
            double cc = 0.0, ss = 0.0, cs = 0.0, yc = 0.0, ys = 0.0;

            for (int n = 0; n < length; ++n)
            {
                const auto phase = w * static_cast<double> (n);
                const auto c = std::cos (phase);
                const auto s = std::sin (phase);
                const auto y = x[static_cast<size_t> (start + n)];
                cc += c * c;
                ss += s * s;
                cs += c * s;
                yc += y * c;
                ys += y * s;
            }

            const auto determinant = cc * ss - cs * cs;
            const auto a = (yc * ss - ys * cs) / determinant;
            const auto b = (ys * cc - yc * cs) / determinant;
            return std::sqrt (a * a + b * b);
        }

        // 拟合窗：≥ 20 个周期且 ≥ 0.25 s。
        int fitWindowLength (double frequency, double sampleRate)
        {
            return static_cast<int> (std::ceil (juce::jmax (20.0 / frequency, 0.25) * sampleRate));
        }

        // 纯 M 正弦（各声道相同）过模块，返回第 0 声道的稳态增益（dB）。
        // 稳态：先跑 1 s——最慢的一级（低切第二级 Q = 1.31）在 10 Hz 的时间常数 2Q/ω0 ≈ 42 ms，1 s 后瞬态低于 1e-10；
        // 固定延迟 D（本文件口径 = D + G）≤ 4414 样本（192 kHz 下约 23 ms）远小于 1 s，拟合窗里全是延迟后的稳态正弦。
        double measureMidGainDb (double sampleRate, const ClayParams& p, double frequency, int channels = 2)
        {
            const auto settle = static_cast<int> (1.0 * sampleRate);
            const auto window = fitWindowLength (frequency, sampleRate);
            const auto input  = makeSineOfLength (channels, sampleRate, frequency, 0.25f, settle + window);

            ClayModule clay;
            prepareClay (clay, sampleRate, channels, p);
            const auto output = runClay (clay, input, maxBlock);

            return 20.0 * std::log10 (fittedAmplitude (channelAsDouble (output, 0), settle, window, frequency, sampleRate)
                                      / fittedAmplitude (channelAsDouble (input, 0), settle, window, frequency, sampleRate));
        }

        struct SideMeasurement
        {
            double gainDb = 0.0;      // 输出 S 相对输入 S 的稳态增益
            double maxMidOut = 0.0;   // 输出 M = (L′ + R′)/2 的最大绝对值（纯 S 输入时应恰为 0）
        };

        // 纯 S 正弦（L = x、R = −x，幅度默认 0.25 · quietScale）过模块。稳态：先跑 0.5 s（② 是 FIR：延迟 D 加上低通的 4(L − 1) 个样本后就是稳态，
        // 192 kHz、40 Hz 时合计约 36 ms；③ 的高架时间常数更短）。
        SideMeasurement measureSide (double sampleRate, const ClayParams& p, double frequency, float amplitude = 0.25f * quietScale)
        {
            const auto settle = static_cast<int> (0.5 * sampleRate);
            const auto window = fitWindowLength (frequency, sampleRate);
            // 默认幅度压到守护地板之下（quietScale）：+3 dB 加宽后仍 < −40 dBFS，量到的是 ③ 本身的增益（ADR-014）；
            // T-032 的正常电平用例另给幅度（③ 的增量已不进守护，正常电平也该量到同样的增益）。
            auto input = makeSineOfLength (2, sampleRate, frequency, amplitude, settle + window);

            for (auto& sample : input[1])
                sample = -sample;

            ClayModule clay;
            prepareClay (clay, sampleRate, 2, p);
            const auto output = runClay (clay, input, maxBlock);

            SideMeasurement result;
            result.gainDb = 20.0 * std::log10 (fittedAmplitude (sideOf (output), settle, window, frequency, sampleRate)
                                               / fittedAmplitude (sideOf (input), settle, window, frequency, sampleRate));

            for (size_t i = 0; i < output[0].size(); ++i)
                result.maxMidOut = juce::jmax (result.maxMidOut,
                                               std::abs (0.5 * (static_cast<double> (output[0][i]) + static_cast<double> (output[1][i]))));

            return result;
        }

        // [start, start + rampLength) 内每个样本都落在「from → to」线性交叉淡化的包络里：第 k 个样本的混合比例在 k/N 与
        // (k + 1)/N 之间（允许一个样本的相位差——规格只定「20 ms 线性」），容差 1e-6（float 输出与 double 混合的舍入）。
        bool withinCrossfade (const Signal& output, const Signal& from, const Signal& to, int start, int rampLength,
                              double tolerance = 1.0e-6)
        {
            for (size_t ch = 0; ch < output.size(); ++ch)
            {
                for (int k = 0; k < rampLength; ++k)
                {
                    const auto index = static_cast<size_t> (start + k);
                    const auto a = static_cast<double> (from[ch][index]);
                    const auto b = static_cast<double> (to[ch][index]);
                    const auto early = a + (b - a) * static_cast<double> (k) / static_cast<double> (rampLength);
                    const auto late  = a + (b - a) * static_cast<double> (k + 1) / static_cast<double> (rampLength);
                    const auto value = static_cast<double> (output[ch][index]);

                    if (value < juce::jmin (early, late) - tolerance || value > juce::jmax (early, late) + tolerance)
                        return false;
                }
            }

            return true;
        }

        // [from, to) 内每个样本都落在两份参考同一时刻的值之间（容差 tolerance）：任意比例的交叉淡化都满足，咔哒 / 起跳不满足。
        bool betweenReferences (const Signal& output, const Signal& referenceA, const Signal& referenceB, int from, int to,
                                double tolerance)
        {
            for (size_t ch = 0; ch < output.size(); ++ch)
            {
                for (int i = from; i < to; ++i)
                {
                    const auto index = static_cast<size_t> (i);
                    const auto a = static_cast<double> (referenceA[ch][index]);
                    const auto b = static_cast<double> (referenceB[ch][index]);
                    const auto value = static_cast<double> (output[ch][index]);

                    if (value < juce::jmin (a, b) - tolerance || value > juce::jmax (a, b) + tolerance)
                        return false;
                }
            }

            return true;
        }

        // 各声道 [from, to) 的最大绝对差（signal 与 reference 同一时间轴）。
        float maxDifferenceInRange (const Signal& signal, const Signal& reference, int from, int to)
        {
            float worst = 0.0f;

            for (size_t ch = 0; ch < signal.size(); ++ch)
                for (int i = from; i < to; ++i)
                    worst = juce::jmax (worst, std::abs (signal[ch][static_cast<size_t> (i)] - reference[ch][static_cast<size_t> (i)]));

            return worst;
        }

        // 截取 [from, to)。
        Signal sliceOf (const Signal& signal, int from, int to)
        {
            Signal out;

            for (const auto& channel : signal)
                out.emplace_back (channel.begin() + from, channel.begin() + to);

            return out;
        }

        // 每个 5 ms 窗里 4 kHz 以上的能量：4 阶 Butterworth 高通（两级 juce::dsp::IIR，与被测模块无关的独立实现）。
        std::vector<double> highBandWindowEnergies (const std::vector<float>& x, double sampleRate, int windowLength)
        {
            juce::dsp::IIR::Filter<double> first, second;
            first.coefficients  = juce::dsp::IIR::Coefficients<double>::makeHighPass (sampleRate, 4000.0, 0.54119610014619698);
            second.coefficients = juce::dsp::IIR::Coefficients<double>::makeHighPass (sampleRate, 4000.0, 1.3065629648763766);

            std::vector<double> energies;
            double sum = 0.0;
            int count = 0;

            for (auto sample : x)
            {
                const auto y = second.processSample (first.processSample (static_cast<double> (sample)));
                sum += y * y;

                if (++count == windowLength)
                {
                    energies.push_back (sum);
                    sum = 0.0;
                    count = 0;
                }
            }

            return energies;
        }

        // 两声道各自：每个 5 ms 窗的高频能量相对两份静止参考同窗能量的较大者，最多高出多少 dB（整段所有窗）。
        double worstHighBandRiseDb (const Signal& swept, const Signal& referenceA, const Signal& referenceB,
                                    double sampleRate, int windowLength)
        {
            auto worst = -std::numeric_limits<double>::infinity();

            for (size_t ch = 0; ch < swept.size(); ++ch)
            {
                const auto energies   = highBandWindowEnergies (swept[ch], sampleRate, windowLength);
                const auto energiesA  = highBandWindowEnergies (referenceA[ch], sampleRate, windowLength);
                const auto energiesB  = highBandWindowEnergies (referenceB[ch], sampleRate, windowLength);

                for (size_t w = 0; w < energies.size(); ++w)
                {
                    const auto referenceEnergy = juce::jmax (energiesA[w], energiesB[w]);

                    if (referenceEnergy > 0.0)
                        worst = juce::jmax (worst, 10.0 * std::log10 (energies[w] / referenceEnergy));
                }
            }

            return worst;
        }

        //==============================================================================
        // T-032（2026-09-29）峰值守护正常电平用例的工具：素材在守护地板（−40 dBFS）之上，量①～③经守护后真正剩下的效果。

        // 峰值归一：整段（所有声道）的最大绝对值缩放到 peakDbfs。
        Signal normalisedToPeak (Signal signal, float peakDbfs)
        {
            float peak = 0.0f;

            for (const auto& channel : signal)
                for (auto sample : channel)
                    peak = juce::jmax (peak, std::abs (sample));

            const auto gain = peak > 0.0f ? juce::Decibels::decibelsToGain (peakDbfs) / peak : 1.0f;

            for (auto& channel : signal)
                for (auto& sample : channel)
                    sample *= gain;

            return signal;
        }

        // M = (L + R) / 2（double）。
        std::vector<double> midOf (const Signal& signal)
        {
            std::vector<double> out (signal[0].size());

            for (size_t i = 0; i < out.size(); ++i)
                out[i] = 0.5 * (static_cast<double> (signal[0][i]) + static_cast<double> (signal[1][i]));

            return out;
        }

        // x 过 4 阶 Butterworth（两级 juce::dsp::IIR，Q = 0.5412 / 1.3066，与被测模块无关的独立实现；highPass = true 为高通、否则低通）
        // 之后 [from, to) 的能量 Σy²。滤波器从 x 的开头起跑，from 之前的样本只用来起振。
        double bandEnergy (const std::vector<double>& x, double sampleRate, double cutoffHz, bool highPass, int from, int to)
        {
            using Coefficients = juce::dsp::IIR::Coefficients<double>;
            juce::dsp::IIR::Filter<double> first, second;
            first.coefficients  = highPass ? Coefficients::makeHighPass (sampleRate, cutoffHz, 0.54119610014619698)
                                           : Coefficients::makeLowPass  (sampleRate, cutoffHz, 0.54119610014619698);
            second.coefficients = highPass ? Coefficients::makeHighPass (sampleRate, cutoffHz, 1.3065629648763766)
                                           : Coefficients::makeLowPass  (sampleRate, cutoffHz, 1.3065629648763766);

            double sum = 0.0;

            for (int n = 0; n < to; ++n)
            {
                const auto y = second.processSample (first.processSample (x[static_cast<size_t> (n)]));

                if (n >= from)
                    sum += y * y;
            }

            return sum;
        }

        // 「正常电平、经守护后的效果」：input（峰值在守护地板之上）过参数 p，量输出的 M 或 S 在某频段（cutoffHz 以上 / 以下）
        // 的能量相对输入同频段的增益（dB）。输出比输入晚 D（本文件口径 = D + G），先对齐再比：输出 [D + settle, N) 对输入
        // [settle, N − D)，settle 让测量滤波器与模块起振过去。另量「标称」= 同一素材缩到 2^−10（scaledBy，峰值远在守护地板之下）
        // 量到的增益——①～③ 线性、2 的幂缩放精确，守护不介入时这就是①～③ 本身的效果（参照那次运行的守护统计必须为 0，调用方断言）；
        // actual 与 nominal 的差 = 守护回收掉的量。顺带记下守护统计：g < 1 的输出样本占比与最小 g。
        struct GuardedEffect
        {
            double nominalDb = 0.0;
            double actualDb = 0.0;
            double guardedShare = 0.0;
            float minGain = 1.0f;
            std::int64_t nominalGuardedSamples = 0;   // 标称那次（2^−10）运行里 g < 1 的样本数：必须为 0
        };

        GuardedEffect measureGuardedEffect (double sampleRate, const Signal& input, const ClayParams& p,
                                            bool side, double cutoffHz, bool highPass, double settleSeconds = 1.0)
        {
            const auto numSamples = static_cast<int> (input[0].size());
            const auto delay = specDelay (sampleRate);
            const auto settle = static_cast<int> (settleSeconds * sampleRate);
            const auto component = [side] (const Signal& s) { return side ? sideOf (s) : midOf (s); };

            struct Run
            {
                double gainDb;
                ClayModule::PeakGuardStats stats;
            };

            const auto run = [&] (const Signal& in)
            {
                ClayModule clay;
                prepareClay (clay, sampleRate, 2, p);
                clay.getAndResetPeakGuardStats();
                const auto out = runClay (clay, in, maxBlock);
                const auto stats = clay.getAndResetPeakGuardStats();
                const auto outEnergy = bandEnergy (component (out), sampleRate, cutoffHz, highPass, delay + settle, numSamples);
                const auto inEnergy  = bandEnergy (component (in),  sampleRate, cutoffHz, highPass, settle, numSamples - delay);
                return Run { 10.0 * std::log10 (outEnergy / inEnergy), stats };
            };

            const auto actual = run (input);
            const auto nominal = run (scaledBy (input, 1.0f / 1024.0f));

            GuardedEffect result;
            result.actualDb = actual.gainDb;
            result.guardedShare = static_cast<double> (actual.stats.guardedSamples) / static_cast<double> (numSamples);
            result.minGain = actual.stats.minGain;
            result.nominalDb = nominal.gainDb;
            result.nominalGuardedSamples = nominal.stats.guardedSamples;
            return result;
        }

        juce::String guardedEffectText (const GuardedEffect& e)
        {
            return "nominal " + juce::String (e.nominalDb, 3) + " dB, actual " + juce::String (e.actualDb, 3) + " dB, g < 1 on "
                 + juce::String (100.0 * e.guardedShare, 2) + " % of samples, min g " + juce::String (e.minGain, 4);
        }

        //==============================================================================
        // 输出摘要（FNV-1a 64 位，自带实现、与平台无关）：改守护前后「widthDb = 0 的输出逐位不变」的对照依据（T-032 验收门）。
        // 把每块输出样本的位、每次 process 的代价格报告、末尾的守护统计、isPassThrough 与延迟逐字节喂进去。
        struct Digest
        {
            std::uint64_t hash = 14695981039346656037ull;

            void add (const void* data, size_t size) noexcept
            {
                const auto* bytes = static_cast<const unsigned char*> (data);

                for (size_t i = 0; i < size; ++i)
                {
                    hash ^= bytes[i];
                    hash *= 1099511628211ull;
                }
            }

            template <typename T>
            void addValue (const T& value) noexcept   { add (&value, sizeof (value)); }

            juce::String hex() const
            {
                juce::String text;

                for (int shift = 60; shift >= 0; shift -= 4)
                    text << "0123456789abcdef"[(hash >> shift) & 0xf];

                return text;
            }
        };

        // 按随机块长（固定种子的 juce::Random：通常 1 ～ 600，八分之一的块 1 ～ 4096；在参数改变的位置切开）跑整段并做摘要。
        juce::String digestOfRun (double sampleRate, int channels, const Signal& input, const ClayParams& initial,
                                  const Schedule& changes, juce::int64 blockSeed)
        {
            ClayModule clay;
            prepareClay (clay, sampleRate, channels, initial);
            clay.getAndResetPeakGuardStats();

            juce::Random random (blockSeed);
            Digest digest;
            const auto numSamples = static_cast<int> (input[0].size());
            juce::AudioBuffer<float> block (channels, 4096);
            size_t nextChange = 0;

            for (int start = 0; start < numSamples;)
            {
                while (nextChange < changes.size() && changes[nextChange].atSample <= start)
                    clay.setParams (changes[nextChange++].params);

                auto length = random.nextInt (8) == 0 ? 1 + random.nextInt (4096) : 1 + random.nextInt (600);
                length = juce::jmin (length, numSamples - start);

                if (nextChange < changes.size())
                    length = juce::jmin (length, changes[nextChange].atSample - start);

                block.setSize (channels, length, false, false, true);

                for (int ch = 0; ch < channels; ++ch)
                    block.copyFrom (ch, 0, input[static_cast<size_t> (ch)].data() + start, length);

                clay.process (block);

                for (int ch = 0; ch < channels; ++ch)
                    digest.add (block.getReadPointer (ch), static_cast<size_t> (length) * sizeof (float));

                const auto pressure = clay.getLastProcessPressure();
                digest.addValue (pressure.meanDb);
                digest.addValue (pressure.seconds);
                start += length;
            }

            const auto stats = clay.getAndResetPeakGuardStats();
            digest.addValue (stats.guardedSamples);
            digest.addValue (stats.minGain);
            const auto passThrough = clay.isPassThrough();
            digest.addValue (passThrough);
            const auto latency = clay.getLatencySamples();
            digest.addValue (latency);
            return digest.hex();
        }

        //==============================================================================
        // 低通单元测试用：4 个长度 L 的矩形窗卷积的精确系数。先做整数卷积（系数都是整数，最大约 (2/3)·L³，L = 1152 时约 1e9 < 2⁵³，
        // double 精确），再除以 L⁴（1152⁴ ≈ 1.8e12，也精确）——每个系数只有最后一次除法的舍入（相对 1.1e-16）。
        std::vector<double> exactKernel (int length)
        {
            std::vector<double> kernel { 1.0 };

            for (int stage = 0; stage < 4; ++stage)
            {
                std::vector<double> next (kernel.size() + static_cast<size_t> (length) - 1, 0.0);

                for (size_t i = 0; i < kernel.size(); ++i)
                    for (size_t k = 0; k < static_cast<size_t> (length); ++k)
                        next[i + k] += kernel[i];

                kernel.swap (next);
            }

            const auto l = static_cast<double> (length);
            const auto scale = (l * l) * (l * l);

            for (auto& value : kernel)
                value /= scale;

            return kernel;
        }

        // 朴素卷积：输出第 n 个样本 = Σ kernel[k] · x[n − k]（x 在 0 之前为 0），double 按 k 顺序累加。
        double convolveAt (const std::vector<double>& x, const std::vector<double>& kernel, int n)
        {
            double sum = 0.0;
            const auto taps = juce::jmin (static_cast<int> (kernel.size()), n + 1);

            for (int k = 0; k < taps; ++k)
                sum += kernel[static_cast<size_t> (k)] * x[static_cast<size_t> (n - k)];

            return sum;
        }

        // 整段逐样本送进低通，返回输出。
        std::vector<double> runLowpass (LinearPhaseLowpass& lowpass, const std::vector<double>& x)
        {
            std::vector<double> y (x.size());

            for (size_t i = 0; i < x.size(); ++i)
                y[i] = lowpass.process (x[i]);

            return y;
        }

        // 两段 double 序列逐位不同的样本数（memcmp）。
        int countDifferentDoubles (const std::vector<double>& a, const std::vector<double>& b)
        {
            int different = 0;

            for (size_t i = 0; i < a.size(); ++i)
                different += std::memcmp (&a[i], &b[i], sizeof (double)) != 0 ? 1 : 0;

            return different;
        }

        // 由实测冲激响应算互补高通（对齐到群延迟 c 的纯延迟减去低通）在 f 处的增益（dB）：C(ω) = e^{−jωc} − Σ h[n]·e^{−jωn}。
        double complementGainDb (const std::vector<double>& impulseResponse, int groupDelay, double frequency, double sampleRate)
        {
            const auto omega = juce::MathConstants<double>::twoPi * frequency / sampleRate;
            std::complex<double> response (0.0, 0.0);

            for (size_t n = 0; n < impulseResponse.size(); ++n)
                response += impulseResponse[n] * std::polar (1.0, -omega * static_cast<double> (n));

            const auto complement = std::polar (1.0, -omega * static_cast<double> (groupDelay)) - response;
            return 20.0 * std::log10 (std::abs (complement));
        }

        // 固定种子的均匀噪声 offset ± 1（double），给低通的数值测试用。
        std::vector<double> makeUniformNoise (int numSamples, double offset, juce::int64 seed)
        {
            juce::Random random (seed);
            std::vector<double> x (static_cast<size_t> (numSamples));

            for (auto& value : x)
                value = offset + (static_cast<double> (random.nextFloat()) * 2.0 - 1.0);

            return x;
        }

        // 测试类也放在匿名命名空间里：与其他测试文件（ClayGuardTests / ClayGlazeTests 等并行新建）不会有同名类的 ODR 冲突。
        class ClayModuleTests  : public juce::UnitTest
        {
        public:
            ClayModuleTests() : juce::UnitTest ("ClayModule", "Celadon") {}

            void runTest() override
            {
                testLatencyIsFixedDelay();
                testPassThroughWhenStagesAreZero();
                testReturnToZeroRestoresPassThrough();
                testPassThroughWaitsForTheWidthTail();
                testWidthTailKeepsSignedZeros();
                testLowCutCornerFrequency();
                testMonoCollapsesLowSide();
                testMonoImpulseResponseIsLinearPhase();
                testCrossoverChangeCrossfadesLowpasses();
                testCrossoverDragIsClickFree();
                testMonoTurnOnUsesRunningLowpass();
                testPureMidProducesNoSide();
                testChannelSwapSymmetry();
                testMonoInputOnlyLowCut();
                testWidthShelfGain();
                testBlockSizeIndependence();
                testParameterSweepsAreClickFree();
                testBypassCrossfade();
                testLowCutSwitchFadesAndShadowRuns();
                testResetClearsStateAndLandsSmoothers();
                testAudioPathDoesNotAllocate();
                testNonFiniteInputDoesNotLatch();
                testPeakGuardBoundsEveryOutputSample();
                testPeakGuardLeavesSteadyLowTonesAlone();
                testWidthIsNotRecoveredByPeakGuard();
                testLowCutAndMonoEffectsAtNormalLevel();
                testOutputDigestsForGuardComparison();
            }

        private:
            // 根治修订第 2 节「Clay 固定延迟 D = 2(L_max − 1)，L_max 按手动下限 40 Hz 算……延迟与参数无关、上报一次」+ ADR-014 峰值守护
            // 前瞻 G（44.1 / 48 / 96 / 192 kHz 合计 1013 / 1102 / 2206 / 4414）+ 头文件 getLatencySamples：
            //  - 上报值 = 表中常数 = 规格公式（specDelay，本文件独立算）；换参数、开旁通、reset 之后不变；单声道模块同一个 D。
            //  - 冲激实测（左声道 0.5 的脉冲 = M、S 各 0.25）：
            //      只开低切与加宽（a = 0，两者都是因果 IIR）：position + D 之前输出恰为 0；position + D 处 > 0.4（M 过 30 Hz 低切的
            //      首样本约 0.995 → 0.249，S 再过 +2 dB 高架首样本约 1.23 倍 → 约 0.31，合计约 0.56）；
            //      三级全开（a = 0.8，分频 150 Hz）：线性相位低通有前振，从 position + D − 2(L − 1) 起才有输出、之前恰为 0；
            //      峰值仍在 position + D（前振只有 a·S·h 量级，h ≤ 2/(3L) < 0.01）。
            void testLatencyIsFixedDelay()
            {
                beginTest ("fixed latency 2 (L_max - 1) + peak-guard lookahead: 1013 / 1102 / 2206 / 4414 samples whatever the parameters, confirmed with impulses");

                struct Case { double sampleRate; int expected; };
                const Case cases[] = { { 44100.0, 1013 }, { 48000.0, 1102 }, { 96000.0, 2206 }, { 192000.0, 4414 } };

                for (const auto& c : cases)
                {
                    const auto tag = "@ " + juce::String (c.sampleRate);

                    ClayModule clay;
                    prepareClay (clay, c.sampleRate, 2, allStagesParams());
                    expectEquals (clay.getLatencySamples(), c.expected, "reported latency " + tag);
                    expectEquals (clay.getLatencySamples(), specDelay (c.sampleRate), "latency follows the spec formula " + tag);
                    expectEquals (ClayModule::latencyFor (c.sampleRate), c.expected, "latencyFor agrees " + tag);

                    clay.setParams (makeParams (60.0f, 40.0f, 1.0f, -3.0f, true));
                    expectEquals (clay.getLatencySamples(), c.expected, "latency does not depend on parameters or bypass " + tag);
                    clay.reset();
                    expectEquals (clay.getLatencySamples(), c.expected, "reset keeps the latency " + tag);

                    ClayModule mono;
                    prepareClay (mono, c.sampleRate, 1, neutralParams());
                    expectEquals (mono.getLatencySamples(), c.expected, "a mono module has the same latency " + tag);

                    constexpr int position = 1000;
                    const auto delay = c.expected;

                    // 只开低切与加宽（a = 0）：因果 IIR，整体延迟 D。
                    {
                        ClayModule causal;
                        prepareClay (causal, c.sampleRate, 2, makeParams (30.0f, 150.0f, 0.0f, 2.0f));
                        auto impulse = makeSilence (2, position + delay + 4096);
                        impulse[0][static_cast<size_t> (position)] = 0.5f;
                        const auto output = runClay (causal, impulse, 333);

                        float before = 0.0f;

                        for (int i = 0; i < position + delay; ++i)
                            before = juce::jmax (before, std::abs (output[0][static_cast<size_t> (i)]), std::abs (output[1][static_cast<size_t> (i)]));

                        expectEquals (before, 0.0f, "low cut + width: nothing comes out before position + D " + tag);
                        expectGreaterThan (std::abs (output[0][static_cast<size_t> (position + delay)]), 0.4f,
                                           "low cut + width: the impulse comes out at position + D " + tag);
                    }

                    // 三级全开：线性相位低通的前振从 position + D − 2(L − 1) 起，峰值仍在 position + D。
                    {
                        ClayModule full;
                        prepareClay (full, c.sampleRate, 2, allStagesParams());
                        const auto preRinging = 2 * (specLength (c.sampleRate, 150.0) - 1);
                        auto impulse = makeSilence (2, position + delay + 4096);
                        impulse[0][static_cast<size_t> (position)] = 0.5f;
                        const auto output = runClay (full, impulse, 333);

                        float before = 0.0f;

                        for (int i = 0; i < position + delay - preRinging; ++i)
                            before = juce::jmax (before, std::abs (output[0][static_cast<size_t> (i)]), std::abs (output[1][static_cast<size_t> (i)]));

                        int peak = 0;

                        for (int i = 1; i < static_cast<int> (output[0].size()); ++i)
                            if (std::abs (output[0][static_cast<size_t> (i)]) > std::abs (output[0][static_cast<size_t> (peak)]))
                                peak = i;

                        expectEquals (before, 0.0f, "all stages: nothing comes out before position + D - 2 (L - 1) " + tag);
                        expectEquals (peak - position, delay, "all stages: the impulse peak is at position + D " + tag);
                    }
                }
            }

            // 根治修订第 2 节「逐位：三项实际值都为 0 且静止时，输出 = 输入逐位延迟 D（L、R 各走纯延迟，不做 M/S 往返）」+ 第 4 节第 3 条。
            // 信号：两声道不相关粉噪，散布 ±0、±最小正规数、±非正规数、±1e30、±FLT_MAX、±Inf（NaN 以外的各种值）与一段交替 ±0；
            // 按 1 / 17 / 64 / 333 / 2048 / 4096（大于 prepare 块长 512）送进去，立体声与单声道各一遍，memcmp 与「输入延迟 D」逐位比较
            //（头 D 个样本是延迟线的初值 0）。中途把 monoHz 从 300 改到 40 Hz：a = 0 时分频点不影响输出（低通在影子里换长度也不影响），
            // 不该打破纯延迟。阳性对照：只开 0.5 dB 的加宽，输出就必须与延迟后的输入不同（否则「逐位相同」什么也没证明）。
            void testPassThroughWhenStagesAreZero()
            {
                beginTest ("stages at zero: the output is the input delayed by D, bit for bit, for every block size (+-0 and extreme values too)");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (1.0 * sampleRate);
                const auto delay = specDelay (sampleRate);
                auto input = makePinkNoise (2, numSamples, -18.0f, 101);
                sprinkleSpecialValues (input);

                auto movedCrossover = neutralParams();
                movedCrossover.monoHz = 40.0f;
                const Schedule changes { { numSamples / 2, movedCrossover } };

                for (auto channels : { 2, 1 })
                {
                    const auto in = channels == 2 ? input : Signal { input[0] };
                    const auto expected = delayedBy (in, delay);

                    for (auto blockSize : { 1, 17, 64, 333, 2048, 4096 })
                    {
                        const auto tag = juce::String (channels) + " ch, block " + juce::String (blockSize);

                        ClayModule clay;
                        prepareClay (clay, sampleRate, channels, neutralParams());
                        expect (clay.isPassThrough(), tag + ": pure delay right after prepare");

                        const auto output = runClay (clay, in, blockSize, changes);
                        expectEquals (countDifferentBits (expected, output), 0, tag + ": output must be the input delayed by D, bit for bit");
                        expect (clay.isPassThrough(), tag + ": still a pure delay after the run");
                    }
                }

                const auto clean = makePinkNoise (2, numSamples, -18.0f, 102);
                ClayModule widened;
                prepareClay (widened, sampleRate, 2, makeParams (0.0f, 300.0f, 0.0f, 0.5f));
                expect (! widened.isPassThrough(), "widthDb = 0.5 dB is not a pure delay");
                const auto changed = runClay (widened, clean, maxBlock);
                expect (countDifferentBits (delayedBy (clean, delay), changed) > numSamples / 2, "a 0.5 dB side shelf must change most samples");
            }

            // 各级从非 0 回到 0：输出选择 / a / 增益 20 ms 内落到 0，低切淡化再过 D 个样本走出延迟线，高架余振（48 kHz 极点半径约
            // 0.83 / 样本，192 kHz 约 0.955）几百个样本内衰减到归零门槛——之后必须重新逐位纯延迟（GLAZE 拧回 0 后整链「纯延迟 D」的
            // 前提；Balance 在 v0.4 就栽过「拧回 0 后停不下」）。1 s 三级全开，然后三项同时回 0；100 ms 之后（> D + 20 ms + 余振）输出
            // 逐位等于延迟 D 的输入，isPassThrough() 为真。
            void testReturnToZeroRestoresPassThrough()
            {
                beginTest ("after all stages return to zero the output is the bit-exact delayed input again");

                for (auto sampleRate : { 48000.0, 192000.0 })
                {
                    const auto numSamples = static_cast<int> (1.5 * sampleRate);
                    const auto offAt      = static_cast<int> (1.0 * sampleRate);
                    const auto settledAt  = offAt + static_cast<int> (0.1 * sampleRate);
                    const auto input = makeProgramme (numSamples, sampleRate, -18.0f, 7);
                    const auto expected = delayedBy (input, specDelay (sampleRate));
                    const auto tag = "@ " + juce::String (sampleRate);

                    ClayModule clay;
                    prepareClay (clay, sampleRate, 2, allStagesParams());
                    const auto output = runClay (clay, input, 333, { { offAt, neutralParams() } });

                    expect (countDifferentBits (expected, output, 0, offAt) > offAt, tag + ": all three stages must alter the signal first");
                    expectEquals (countDifferentBits (expected, output, settledAt), 0, tag + ": bit-exact delayed input from 100 ms after returning to zero");
                    expect (clay.isPassThrough(), tag + ": isPassThrough() after settling");
                }
            }

            // T-032：③ 的增量 δ 不进守护——①② 都关着时守护看到的 y 与干路逐位相同，前瞻环里还有 δ ≠ 0 的样本时 isPassThrough() 也必须
            // 为假（头文件：「此刻起输出 = 输入逐位延迟 D + G」，环里 δ ≠ 0 的样本还要带着 δ 输出出去）。只开加宽 +3 dB（①② 关）跑 0.5 s，
            // 再拧回 0；按 16 样本的块跑、每块末查 isPassThrough()：
            //  - 第一次为真的那一刻起，之后的输出逐位 = 延迟 D + G 的输入，且那一刻晚于最后一个与纯延迟不同的输出样本；
            //  - 拧回 0 之后 100 ms 内报真（20 ms 斜坡 + 高架余振按极点衰减到归零门槛 + 再过 G 个样本）；
            //  - 阳性对照：拧回 0 之前输出与纯延迟不同，拧回 0 之后仍有 δ 的尾巴（最后一个不同的样本晚于拧回 0 的时刻）；
            //  - δ 的尾巴一个不丢：同一素材拧到 1e-6 dB（高架永远不会逐位直通，守护一直走带 δ 的那条路）的模块作参照，两者全程之差
            //    ≤ 1e-6（1e-6 dB 的高架与直通只差 1.2e-7 倍的 S''，外加 float 舍入）——守护从带 δ 的那条路切回不带 δ 的那条时，环里
            //    还没输出的 δ 必须照样加回。
            // 反面（T-032 报告的变异实验）：δ ≠ 0 不算「处理过」时，高架一归零就报真，环里剩下的 δ 让随后的输出与纯延迟不同；守护选路
            // 只看本段有没有 δ、不看环里时，环里剩下的 δ 被丢掉，与 1e-6 dB 参照差出 δ 本身的量级。
            void testPassThroughWaitsForTheWidthTail()
            {
                beginTest ("width alone (T-032): isPassThrough() stays false while width increments are still in the lookahead ring");

                constexpr int blockSize = 16;

                for (auto sampleRate : { 48000.0, 192000.0 })
                {
                    const auto tag = "@ " + juce::String (sampleRate);
                    const auto numSamples = static_cast<int> (1.0 * sampleRate);
                    const auto offAt = static_cast<int> (0.5 * sampleRate);
                    const auto input = makeProgramme (numSamples, sampleRate, -18.0f, 17);
                    const auto expected = delayedBy (input, specDelay (sampleRate));

                    ClayModule clay, almostOff;
                    prepareClay (clay, sampleRate, 2, makeParams (0.0f, 120.0f, 0.0f, 3.0f));
                    prepareClay (almostOff, sampleRate, 2, makeParams (0.0f, 120.0f, 0.0f, 3.0f));

                    auto output = input, reference = input;
                    juce::AudioBuffer<float> block (2, blockSize), referenceBlock (2, blockSize);
                    auto passAt = -1;

                    for (int start = 0; start < numSamples; start += blockSize)
                    {
                        if (start == offAt)
                        {
                            clay.setParams (neutralParams());
                            almostOff.setParams (makeParams (0.0f, 300.0f, 0.0f, 1.0e-6f));
                        }

                        const auto length = juce::jmin (blockSize, numSamples - start);
                        block.setSize (2, length, false, false, true);
                        referenceBlock.setSize (2, length, false, false, true);

                        for (int ch = 0; ch < 2; ++ch)
                        {
                            block.copyFrom (ch, 0, input[static_cast<size_t> (ch)].data() + start, length);
                            referenceBlock.copyFrom (ch, 0, input[static_cast<size_t> (ch)].data() + start, length);
                        }

                        clay.process (block);
                        almostOff.process (referenceBlock);

                        for (int ch = 0; ch < 2; ++ch)
                        {
                            std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length,
                                       output[static_cast<size_t> (ch)].begin() + start);
                            std::copy (referenceBlock.getReadPointer (ch), referenceBlock.getReadPointer (ch) + length,
                                       reference[static_cast<size_t> (ch)].begin() + start);
                        }

                        if (passAt < 0 && start >= offAt && clay.isPassThrough())
                            passAt = start + length;
                    }

                    auto lastDifferent = -1;

                    for (int i = 0; i < numSamples; ++i)
                        for (size_t ch = 0; ch < 2; ++ch)
                            if (std::memcmp (&output[ch][static_cast<size_t> (i)], &expected[ch][static_cast<size_t> (i)], sizeof (float)) != 0)
                                lastDifferent = i;

                    const auto tailGap = maxDifferenceInRange (output, reference, 0, numSamples);
                    logMessage ("  " + tag + ": width back to 0 at sample " + juce::String (offAt) + ", last output differing from the pure delay "
                                + juce::String (lastDifferent) + ", isPassThrough() first true at " + juce::String (passAt)
                                + "; largest difference to the 1e-6 dB reference " + juce::String (tailGap, 9));
                    expectLessOrEqual (static_cast<double> (tailGap), 1.0e-6, tag + ": no width increment is dropped on the way back to 0 (1e-6 dB reference)");
                    expect (countDifferentBits (expected, output, 0, offAt) > offAt / 2, tag + ": positive control: +3 dB of width changes the output");
                    expect (lastDifferent >= offAt, tag + ": positive control: the width tail reaches the output after it was turned off");
                    expect (passAt > lastDifferent, tag + ": isPassThrough() turns true only after the last width increment has left the module");
                    expect (passAt >= 0 && passAt - offAt <= static_cast<int> (0.1 * sampleRate), tag + ": a pure delay again within 100 ms");
                    expectEquals (countDifferentBits (expected, output, juce::jmax (0, passAt)), 0,
                                  tag + ": from the moment isPassThrough() is true the output is the bit-exact delayed input");
                }
            }

            // T-032（核验 V4 的第一处细节）：③ 的增量 δ 为 0（含 −0）时守护不做那次加法——(−0) + (+0) = +0，无条件相加会打破 ±0 的逐位。
            // 真实路径上它在加宽开着、δ 恰为 0 的样本上起作用：高架余振衰减到网格点被归零（两个状态分量分别归零时，中间也可能出现单个
            // δ = 0 的样本）之后，守护还在带 δ 的那条路上把前瞻环里剩下的样本输出完。素材 A：两声道独立的宽粉噪 0.25 s（③ 有侧声道可加宽），
            // 之后是交替的 ±0（左 −0 / +0、右 +0 / −0：每个样本总有一个声道是 −0）；素材 B：同一段粉噪，之后全是 +0。加宽 +3 dB、①② 关
            // （守护看到的 y 与干路逐位相同，从不动作）。两份素材的 S 只差零的符号，高架的余振（非零的 δ）逐位相同；所以 ±0 段里逐样本：
            //  - B 的输出不是 +0（δ ≠ 0：输出 = δ）→ A 的输出必须与 B 逐位相同；
            //  - B 的输出是 +0（δ = 0）→ A 的输出必须逐位 = 延迟 D + G 的 A（−0 也是 −0）。
            // 无条件相加时，守护带 δ 那条路上 δ = 0 的样本里 −0 会变成 +0（T-032 报告的变异实验：48 kHz 三种块长各抓到 8 / 48 / 48 个
            // 声道样本）。这种样本在带 δ 那条路的最后一段里、最后一个 δ ≠ 0 的样本之后：余振在网格点归零，最后一个 δ ≠ 0 的样本就在网格点
            // 之前，而最后一段是「它 + G」所在的那一段（段不跨网格点）——48 kHz 的 G = 528 不是 64 的倍数，那一段还要往后走到下一个网格点或块界；
            // 192 kHz 的 G = 2112 = 33 × 64，那一段恰好停在最后一个 δ ≠ 0 的样本上（块长 1 同理），192 kHz 只查没有误报。三种块长各跑一遍
            // （切段不同）。
            // 阳性对照：粉噪段确实被加宽改变；±0 段开头确实有高架的余振（B 的输出非零），并在 50 ms 内精确归零（之后 A 逐位 = 延迟的输入）。
            void testWidthTailKeepsSignedZeros()
            {
                beginTest ("width tail over +-0 (T-032): a width increment of exactly 0 adds nothing, so -0 stays -0 while the guard still carries the tail");

                for (auto sampleRate : { 48000.0, 192000.0 })
                {
                    const auto numSamples = static_cast<int> (0.5 * sampleRate);
                    const auto burst = static_cast<int> (0.25 * sampleRate);
                    auto signedZeros = makePinkNoise (2, numSamples, -18.0f, 41);
                    auto plusZeros = signedZeros;

                    for (int i = burst; i < numSamples; ++i)
                    {
                        const auto index = static_cast<size_t> (i);
                        signedZeros[0][index] = i % 2 == 0 ? -0.0f : 0.0f;
                        signedZeros[1][index] = i % 2 == 0 ? 0.0f : -0.0f;
                        plusZeros[0][index] = 0.0f;
                        plusZeros[1][index] = 0.0f;
                    }

                    const auto expected = delayedBy (signedZeros, specDelay (sampleRate));
                    const auto zerosFrom = burst + specDelay (sampleRate);   // 输出里 ±0 段从这里开始

                    for (auto blockSize : { 17, 333, 4096 })
                    {
                        const auto tag = "@ " + juce::String (sampleRate) + ", block " + juce::String (blockSize);
                        const auto run = [&] (const Signal& input, ClayModule::PeakGuardStats& stats)
                        {
                            ClayModule clay;
                            prepareClay (clay, sampleRate, 2, makeParams (0.0f, 120.0f, 0.0f, 3.0f));
                            clay.getAndResetPeakGuardStats();
                            const auto out = runClay (clay, input, blockSize);
                            stats = clay.getAndResetPeakGuardStats();
                            return out;
                        };

                        ClayModule::PeakGuardStats statsA, statsB;
                        const auto outA = run (signedZeros, statsA);
                        const auto outB = run (plusZeros, statsB);

                        auto violations = 0, zeroIncrementSamples = 0, lastTail = -1;
                        const auto plusZero = 0.0f;

                        for (int i = zerosFrom; i < numSamples; ++i)
                        {
                            for (size_t ch = 0; ch < 2; ++ch)
                            {
                                const auto index = static_cast<size_t> (i);
                                const auto noIncrement = std::memcmp (&outB[ch][index], &plusZero, sizeof (float)) == 0;
                                const auto& want = noIncrement ? expected[ch][index] : outB[ch][index];

                                if (noIncrement)
                                    ++zeroIncrementSamples;
                                else
                                    lastTail = i;

                                violations += std::memcmp (&outA[ch][index], &want, sizeof (float)) != 0 ? 1 : 0;
                            }
                        }

                        logMessage ("  " + tag + ": +-0 section from output sample " + juce::String (zerosFrom) + ", last width increment at "
                                    + juce::String (lastTail) + ", " + juce::String (zeroIncrementSamples) + " channel samples with an increment of exactly 0");
                        expect (countDifferentBits (expected, outA, 0, zerosFrom) > zerosFrom / 2, tag + ": positive control: +3 dB of width changes the noise");
                        expect (lastTail > zerosFrom, tag + ": positive control: the width tail reaches the +-0 section");
                        expect (lastTail - zerosFrom <= static_cast<int> (0.05 * sampleRate), tag + ": the tail is exactly 0 within 50 ms");
                        expectEquals (violations, 0, tag + ": where the increment is exactly 0 the output is the bit-exact delayed input (-0 included), "
                                                           "elsewhere it is the same tail as with +0");
                        expect (statsA.guardedSamples == 0 && statsB.guardedSamples == 0, tag + ": stage 3 alone never engages the guard");
                    }
                }
            }

            // 规格第 4 节 ①：4 阶 Butterworth（两级 RBJ 高通，Q = 0.5412 / 1.3066，两 Q 之积 = 1/√2，设定频率就是半功率点）。
            // 验收「−3 dB 点与设定频率偏差 ≤ 1 %」：4 阶 Butterworth |H|² = r⁸ / (1 + r⁸)，0.99 fc 处 −3.19 dB、1.01 fc 处 −2.84 dB，
            // 所以只要实测增益在 0.99 fc 低于、在 1.01 fc 高于 −3.0103 dB，−3 dB 点就落在 ±1 % 内（响应单调）。
            // 「截止 25 Hz 时 50 Hz 以上变化 ≤ 0.1 dB」：50 Hz 解析值 −0.017 dB，100 Hz 以上 < 0.001 dB。
            // 测法：纯 M 正弦（L = R），拟合 ≥ 20 个周期的稳态幅度，与输入同窗拟合相除（固定延迟 D 只改变相位，拟合幅度不受影响）；
            // 拟合对纯正弦精确，误差远小于 0.17 dB 的判别余量。
            void testLowCutCornerFrequency()
            {
                beginTest ("low cut: -3 dB point within 1 % of the set frequency; 25 Hz leaves 50 Hz and above within 0.1 dB");

                constexpr double halfPowerDb = -3.0103;
                const auto cutOnly = [] (float cutoffHz) { return makeParams (cutoffHz, 120.0f, 0.0f, 0.0f); };

                for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
                {
                    for (auto cutoff : { 10.0f, 25.0f, 60.0f })
                    {
                        const auto cutoffHz = static_cast<double> (cutoff);
                        const auto below = measureMidGainDb (sampleRate, cutOnly (cutoff), 0.99 * cutoffHz);
                        const auto above = measureMidGainDb (sampleRate, cutOnly (cutoff), 1.01 * cutoffHz);
                        const auto tag = juce::String (cutoffHz) + " Hz @ " + juce::String (sampleRate);

                        logMessage ("  low cut " + tag + ": " + juce::String (below, 3) + " dB at 0.99 fc, "
                                    + juce::String (above, 3) + " dB at 1.01 fc");
                        expectLessThan (below, halfPowerDb, tag + ": gain at 0.99 fc must still be below -3 dB");
                        expectGreaterThan (above, halfPowerDb, tag + ": gain at 1.01 fc must already be above -3 dB");
                    }

                    for (auto frequency : { 50.0, 100.0, 1000.0, 10000.0 })
                    {
                        const auto gain = measureMidGainDb (sampleRate, cutOnly (25.0f), frequency);
                        expectWithinAbsoluteError (gain, 0.0, 0.1, "25 Hz low cut at " + juce::String (frequency) + " Hz @ " + juce::String (sampleRate));
                    }
                }

                // 单声道模块同一口径（① 对单声道照做）。
                expectLessThan (measureMidGainDb (48000.0, cutOnly (25.0f), 0.99 * 25.0, 1), halfPowerDb, "mono: gain at 0.99 fc below -3 dB");
                expectGreaterThan (measureMidGainDb (48000.0, cutOnly (25.0f), 1.01 * 25.0, 1), halfPowerDb, "mono: gain at 1.01 fc above -3 dB");
            }

            // 根治修订第 2 节 ②：S' = S_d − a·S_low，S_low = 4 个长度 L 的滑动平均级联（L = round(0.24 fs / fc)）对齐到 S_d。
            // 对纯 S 正弦，增益 = |1 − a·H₀(f)|（H₀ 是级联的零相位响应，实数，见 lowpassZeroPhase）。纯 S 正弦（L = x、R = −x）：
            //  - a = 1、分频 120 Hz：50 Hz 处约 −23.9 dB（旧的 2 阶 Butterworth 是 −15.3 dB）；验收要求衰减 ≥ 12 dB，另与解析值比 ±0.05 dB；
            //  - a = 0.5、分频 120 Hz：240 Hz（2 fc）处 |1 − 0.5·H₀| ≈ −0.88 dB，±0.05 dB——验证两路是相干相减、而且对齐（S_low 与 S_d
            //    若差一个样本的相位，2 fc 处的幅度就会偏开）；
            //  - 阳性对照：a = 0（只开 0.5 dB 加宽让处理路径真正在跑）时 50 Hz 的 S 不衰减（高架在 50 Hz 的解析增益 < 0.0001 dB）；
            //  - ② 只动 S：纯 S 输入时输出 M 恰为 0。
            void testMonoCollapsesLowSide()
            {
                beginTest ("mono: a = 1 at 120 Hz removes >= 12 dB of 50 Hz side, matching |1 - a H0| of the linear-phase complement; mid untouched");

                for (auto sampleRate : { 48000.0, 192000.0 })
                {
                    const auto tag = " @ " + juce::String (sampleRate);
                    const auto length = specLength (sampleRate, 120.0);

                    const auto full = measureSide (sampleRate, makeParams (0.0f, 120.0f, 1.0f, 0.0f), 50.0);
                    const auto fullExpected = 20.0 * std::log10 (std::abs (1.0 - lowpassZeroPhase (50.0, sampleRate, length)));
                    logMessage ("  a = 1, 120 Hz: 50 Hz side " + juce::String (full.gainDb, 3) + " dB (analytic "
                                + juce::String (fullExpected, 3) + " dB)" + tag);
                    expectLessOrEqual (full.gainDb, -12.0, "a = 1 must attenuate the 50 Hz side by at least 12 dB" + tag);
                    expectWithinAbsoluteError (full.gainDb, fullExpected, 0.05, "a = 1 matches the linear-phase complement" + tag);
                    expectEquals (full.maxMidOut, 0.0, "pure side input keeps mid at exactly 0" + tag);

                    const auto half = measureSide (sampleRate, makeParams (0.0f, 120.0f, 0.5f, 0.0f), 240.0);
                    const auto halfExpected = 20.0 * std::log10 (std::abs (1.0 - 0.5 * lowpassZeroPhase (240.0, sampleRate, length)));
                    logMessage ("  a = 0.5, 120 Hz: 240 Hz side " + juce::String (half.gainDb, 3) + " dB (analytic "
                                + juce::String (halfExpected, 3) + " dB)" + tag);
                    expectWithinAbsoluteError (half.gainDb, halfExpected, 0.05, "a = 0.5 is the coherent S_d - a S_low" + tag);

                    const auto none = measureSide (sampleRate, makeParams (0.0f, 120.0f, 0.0f, 0.5f), 50.0);
                    expectWithinAbsoluteError (none.gainDb, 0.0, 0.01, "a = 0 leaves the 50 Hz side alone" + tag);
                }
            }

            // 根治修订第 4 节第 3 条「收单 a = 1 时冲激响应以 D 为中心对称（逐样本 |h[D + k] − h[D − k]| ≤ 1e-9）」。纯 S 冲激（L = +A、
            // R = −A）、低切与加宽关、a = 1：输出 S = δ[n − D]·A − A·h[n − D + 2(L − 1)]（线性相位互补），以 D 为中心对称。
            //  - 四个采样率 × 分频点 40 / 80 / 120 / 300 Hz：|S[D + k] − S[D − k]| ≤ 1e-9（k = 1 … D）；D − 2(L − 1) 之前恰为 0；
            //    D + 2(L − 1) 之后 ≤ 1e-12（滑动和的残差）；中心 > 0.9·A；输出 M 恰为 0。
            //  - A = 2⁻⁶：输出是 float，对称的两个 double 值（只差 1e-18 量级的舍入）偶尔会落在 float 舍入边界两侧、差一个 float ulp；
            //    非中心的值 ≤ A·2/(3L)（L ≥ 35）< 4e-4，一个 ulp < 4e-11，远在 1e-9 以内（A = 1 时 L = 35 的一个 ulp 约 2e-9）。
            //    Python 原型（同一算法、float 输出）实测不对称 ≤ 1.1e-19。
            //  - 阳性对照：同一度量对最小相位的 IIR（低切 60 Hz、a = 0）报出 > 1e-6——它的冲激响应在 D 之前是 0、之后拖尾。
            void testMonoImpulseResponseIsLinearPhase()
            {
                beginTest ("mono a = 1: the pure-side impulse response is symmetric about D (<= 1e-9), linear phase");

                constexpr float amplitude = 1.0f / 64.0f;

                const auto asymmetryAboutDelay = [] (const std::vector<double>& side, int delay)
                {
                    double worst = 0.0;

                    for (int k = 1; k <= delay; ++k)
                        worst = juce::jmax (worst, std::abs (side[static_cast<size_t> (delay + k)] - side[static_cast<size_t> (delay - k)]));

                    return worst;
                };

                for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
                {
                    const auto delay = specDelay (sampleRate);

                    for (auto monoHz : { 40.0f, 80.0f, 120.0f, 300.0f })
                    {
                        const auto tag = juce::String (monoHz) + " Hz @ " + juce::String (sampleRate);
                        const auto halfLength = 2 * (specLength (sampleRate, static_cast<double> (monoHz)) - 1);

                        ClayModule clay;
                        prepareClay (clay, sampleRate, 2, makeParams (0.0f, monoHz, 1.0f, 0.0f));
                        auto impulse = makeSilence (2, 2 * delay + 256);
                        impulse[0][0] = amplitude;
                        impulse[1][0] = -amplitude;
                        const auto output = runClay (clay, impulse, maxBlock);
                        const auto side = sideOf (output);

                        double before = 0.0, after = 0.0, mid = 0.0;

                        for (int n = 0; n < delay - halfLength; ++n)
                            before = juce::jmax (before, std::abs (side[static_cast<size_t> (n)]));

                        for (int n = delay + halfLength + 1; n < static_cast<int> (side.size()); ++n)
                            after = juce::jmax (after, std::abs (side[static_cast<size_t> (n)]));

                        for (size_t i = 0; i < output[0].size(); ++i)
                            mid = juce::jmax (mid, std::abs (0.5 * (static_cast<double> (output[0][i]) + static_cast<double> (output[1][i]))));

                        const auto asymmetry = asymmetryAboutDelay (side, delay);
                        logMessage ("  " + tag + ": asymmetry " + juce::String (asymmetry, 3) + ", centre " + juce::String (side[static_cast<size_t> (delay)], 6));
                        expectLessOrEqual (asymmetry, 1.0e-9, "impulse response symmetric about D " + tag);
                        expectEquals (before, 0.0, "nothing before D - 2 (L - 1) " + tag);
                        expectLessOrEqual (after, 1.0e-12, "nothing after D + 2 (L - 1) " + tag);
                        expectGreaterThan (side[static_cast<size_t> (delay)], 0.9 * static_cast<double> (amplitude), "the centre tap is at D " + tag);
                        expectEquals (mid, 0.0, "pure side input keeps mid at exactly 0 " + tag);
                    }

                    // 阳性对照：最小相位的 IIR 低切。
                    ClayModule minimumPhase;
                    prepareClay (minimumPhase, sampleRate, 2, makeParams (60.0f, 120.0f, 0.0f, 0.0f));
                    auto impulse = makeSilence (2, 2 * delay + 256);
                    impulse[0][0] = amplitude;
                    impulse[1][0] = -amplitude;
                    const auto iirAsymmetry = asymmetryAboutDelay (sideOf (runClay (minimumPhase, impulse, maxBlock)), delay);
                    expectGreaterThan (iirAsymmetry, 1.0e-6, "positive control: the minimum-phase low cut is not symmetric about D @ " + juce::String (sampleRate));
                }
            }

            // 根治修订第 2 节「分频点变化：……用历史环按新 L 重建第二套级联，两套输出 20 ms 交叉淡化」。a = 1，分频点在 changeAt 从
            // 旧值改到新值（立体声粉噪 + 30 Hz 素材），对照两个一直固定在旧 / 新分频点的模块：
            //  - changeAt 之前与旧参考逐位相同；
            //  - [changeAt, changeAt + 64 + 20 ms)：每个样本都在旧、新参考同一时刻的值之间（容差 1e-6，float 舍入）——输出是两套低通的
            //    线性交叉淡化，不会冲出两者之外（咔哒、预热不对的起跳都会冲出去）；切换在参数改变后的第一个 64 样本网格点开始预热；
            //  - 预热分摊在之后的 2(L − 1) 个样本里（T-020），追上以前新低通不进输出：[changeAt, changeAt + 2(L − 1) + G) 与旧参考逐位相同；
            //  - 追上后的网格点起 20 ms 淡化，之后与新参考差 ≤ 1e-6（预热重建的低通与一直在跑的同长度低通只差舍入；Python 原型实测
            //    double 差 ≤ 6e-16、float 输出相同）。
            //  阳性对照：新旧两个参考在同一段上差 > 1e-3（两个分频点确实不同，上面两条不是空话）。
            void testCrossoverChangeCrossfadesLowpasses()
            {
                beginTest ("crossover change: a 20 ms crossfade between the old and new low-pass, then the same as a module set there from the start");

                struct Case { double sampleRate; float fromHz, toHz; };
                const Case cases[] = { { 48000.0, 150.0f, 60.0f }, { 44100.0, 300.0f, 40.0f }, { 192000.0, 80.0f, 160.0f } };

                for (const auto& c : cases)
                {
                    const auto tag = juce::String (c.fromHz) + " -> " + juce::String (c.toHz) + " Hz @ " + juce::String (c.sampleRate);
                    const auto numSamples = static_cast<int> (1.0 * c.sampleRate);
                    const auto changeAt   = static_cast<int> (0.3 * c.sampleRate) + 7;
                    const auto catchUp    = 2 * (specLength (c.sampleRate, c.toHz) - 1);   // 分摊预热：每样本多喂 2 个，欠 4(L − 1) 个
                    const auto fadeFrom   = changeAt + catchUp + specGuardLookahead (c.sampleRate);   // 淡化最早从这里被听到
                    const auto settledAt  = changeAt + switchGrid + catchUp + switchGrid + static_cast<int> (std::lround (0.02 * c.sampleRate))
                                          + specGuardLookahead (c.sampleRate);   // 淡化在①～③ 输出时刻，再过守护的 G
                    const auto input = quieter (makeProgramme (numSamples, c.sampleRate, -18.0f, 141));   // 守护地板之下（ADR-014）

                    const auto before = makeParams (0.0f, c.fromHz, 1.0f, 0.0f);
                    const auto after  = makeParams (0.0f, c.toHz, 1.0f, 0.0f);

                    ClayModule switched, oldReference, newReference;
                    prepareClay (switched, c.sampleRate, 2, before);
                    prepareClay (oldReference, c.sampleRate, 2, before);
                    prepareClay (newReference, c.sampleRate, 2, after);

                    const auto output  = runClay (switched, input, 333, { { changeAt, after } });
                    const auto oldOut  = runClay (oldReference, input, 333);
                    const auto newOut  = runClay (newReference, input, 333);

                    expectEquals (countDifferentBits (oldOut, output, 0, changeAt), 0, "identical to the old setting before the change " + tag);
                    expectEquals (countDifferentBits (oldOut, output, 0, fadeFrom), 0,
                                  "still identical to the old setting while the new low-pass catches up " + tag);
                    expect (betweenReferences (output, oldOut, newOut, changeAt, settledAt, 1.0e-6 * static_cast<double> (quietScale)),
                            "during the switch every sample lies between the old and new low-pass outputs " + tag);
                    const auto residual = maxDifferenceInRange (output, newOut, settledAt, numSamples);
                    logMessage ("  " + tag + ": after the switch, max difference to the new setting " + juce::String (residual, 12));
                    expectLessOrEqual (residual, 1.0e-6f * quietScale, "after the crossfade the output equals a module set to the new crossover " + tag);
                    expectGreaterThan (maxDifferenceInRange (oldOut, newOut, settledAt, numSamples), 1.0e-3f * quietScale,
                                       "positive control: the two crossovers really differ " + tag);
                }
            }

            // 根治修订第 2 节「拖旋钮时是一串 20 ms 淡化，无咔哒」。a = 1，0.2 ～ 1.2 s 里每 64 个样本给一个新分频点（对数 40 → 300 → 40 Hz），
            // 之后停在 40 Hz。用参数扫动同一个咔哒度量（4 kHz 以上每 5 ms 的能量对「静止在 40 Hz」「静止在 300 Hz」两份参考的较大者
            // 不突起 > 1 dB——收单只动低频，一串淡化若有咔哒会在高频冒出来）；最后 0.1 s 与静止在 40 Hz 的模块差 ≤ 1e-6（每次淡化都
            // 走完、最后停在目标上）。阳性对照同参数扫动用例：0.5 的单样本咔哒在同一度量下 > 1 dB。
            void testCrossoverDragIsClickFree()
            {
                beginTest ("dragging the crossover (a new value every 64 samples, a = 1) is click-free and settles on the final low-pass");

                constexpr double sampleRate = 48000.0;
                const auto numSamples   = static_cast<int> (1.4 * sampleRate);
                const auto dragFrom     = static_cast<int> (0.2 * sampleRate);
                const auto dragTo       = static_cast<int> (1.2 * sampleRate);
                const auto windowLength = static_cast<int> (0.005 * sampleRate);
                const auto input = makeProgramme (numSamples, sampleRate, -24.0f, 151);

                Schedule drag;

                for (int at = dragFrom; at < dragTo; at += switchGrid)
                {
                    const auto progress = static_cast<double> (at - dragFrom) / static_cast<double> (dragTo - dragFrom);   // 0 → 1
                    const auto upAndBack = 1.0 - std::abs (2.0 * progress - 1.0);                                          // 0 → 1 → 0
                    drag.push_back ({ at, makeParams (0.0f, static_cast<float> (40.0 * std::pow (300.0 / 40.0, upAndBack)), 1.0f, 0.0f) });
                }

                drag.push_back ({ dragTo, makeParams (0.0f, 40.0f, 1.0f, 0.0f) });

                ClayModule dragged, lowest, highest;
                prepareClay (dragged, sampleRate, 2, makeParams (0.0f, 40.0f, 1.0f, 0.0f));
                prepareClay (lowest, sampleRate, 2, makeParams (0.0f, 40.0f, 1.0f, 0.0f));
                prepareClay (highest, sampleRate, 2, makeParams (0.0f, 300.0f, 1.0f, 0.0f));

                const auto draggedOut = runClay (dragged, input, maxBlock, drag);
                const auto lowestOut  = runClay (lowest, input, maxBlock);
                const auto highestOut = runClay (highest, input, maxBlock);

                const auto rise = worstHighBandRiseDb (draggedOut, lowestOut, highestOut, sampleRate, windowLength);
                logMessage ("  crossover drag: worst 5 ms high-band rise " + juce::String (rise, 3) + " dB");
                expectLessOrEqual (rise, 1.0, "no click above 4 kHz while dragging the crossover");

                const auto settledFrom = numSamples - static_cast<int> (0.1 * sampleRate);
                expectLessOrEqual (maxDifferenceInRange (draggedOut, lowestOut, settledFrom, numSamples), 1.0e-6f,
                                   "after the drag stops the output equals a module left at 40 Hz");

                auto clicked = lowestOut;
                clicked[0][static_cast<size_t> (dragFrom + 480)] += 0.5f;
                expectGreaterThan (worstHighBandRiseDb (clicked, lowestOut, lowestOut, sampleRate, windowLength), 1.0,
                                   "positive control: a 0.5 single-sample click must register as more than 1 dB");
            }

            // 根治修订第 2 节「低通一直在跑（影子运行，旁通时也跑），开收单无起跳」。a 在 onAt 从 0 开到 1（120 Hz），对照一直 a = 1 的模块：
            //  - onAt 之前：输出逐位 = 延迟 D 的输入（a = 0、另两级关 → 纯延迟）；
            //  - [onAt, onAt + 20 ms)：在「延迟的输入 → 一直开着的输出」线性淡化的包络里（a 的 20 ms 斜坡作用在输出时刻）；
            //  - 之后与一直开着的模块逐位相同：两边的低通从一开始就吃同样的 S，a = 0 时也在跑，所以打开那一刻状态现成。
            //  阳性对照：一直开着的输出与延迟的输入差 > 1e-3（收单确实在动这段素材的低频侧声道）。
            void testMonoTurnOnUsesRunningLowpass()
            {
                beginTest ("mono 0 -> 1: pure delay before, 20 ms linear crossfade, then bit-identical to an always-on module (the low-pass kept running)");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (1.0 * sampleRate);
                const auto rampLength = static_cast<int> (0.02 * sampleRate);
                const auto onAt = static_cast<int> (0.5 * sampleRate) + 13;
                const auto input = quieter (makeProgramme (numSamples, sampleRate, -18.0f, 161));   // 守护地板之下（ADR-014）
                const auto dry = delayedBy (input, specDelay (sampleRate));

                const auto off = makeParams (0.0f, 120.0f, 0.0f, 0.0f);
                const auto on  = makeParams (0.0f, 120.0f, 1.0f, 0.0f);

                ClayModule switched, always;
                prepareClay (switched, sampleRate, 2, off);
                prepareClay (always, sampleRate, 2, on);
                const auto output   = runClay (switched, input, 333, { { onAt, on } });
                const auto alwaysOn = runClay (always, input, 333);

                expectEquals (countDifferentBits (dry, output, 0, onAt), 0, "a = 0: bit-exact delayed input before switching on");
                const auto heardAt = onAt + specGuardLookahead (sampleRate);   // a 的斜坡在①～③ 输出时刻，再过守护的 G
                expectEquals (countDifferentBits (dry, output, 0, heardAt), 0, "a = 0: still the bit-exact delayed input until the ramp is heard");
                expect (withinCrossfade (output, dry, alwaysOn, heardAt, rampLength, 1.0e-6 * static_cast<double> (quietScale)),
                        "switching on follows a 20 ms linear crossfade");
                expectEquals (countDifferentBits (alwaysOn, output, heardAt + rampLength), 0,
                              "identical to the always-on module from 20 ms after switching on");
                expectGreaterThan (maxDifferenceInRange (dry, alwaysOn, onAt, numSamples), 1.0e-3f * quietScale,
                                   "positive control: mono really changes this material");
            }

            // 验收「纯 M 信号不产生 S（≤ −120 dBFS）」：L = R 时 ① 两声道同一滤波器、同一输入 → 两路逐位相同 → S 恰为 0；
            // ② ③ 只处理 S，0 进 0 出（低通吃 0 出 0）。三级全开，中途还扫动全部参数（含低切从关到开的淡化、分频点换长度、旁通来回），
            // 测 max |L′ − R′| / 2。阳性对照：同样的参数与时间表下，L、R 不相关的输入 S 明显不为 0（测量本身有效）。
            void testPureMidProducesNoSide()
            {
                beginTest ("pure mid input produces no side (<= -120 dBFS) with every stage active and sweeping");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (2.0 * sampleRate);
                auto mono = makePinkNoise (1, numSamples, -12.0f, 21);
                addSine (mono[0], sampleRate, 30.0, 0.3f);
                addSine (mono[0], sampleRate, 5000.0, 0.1f);
                const Signal input { mono[0], mono[0] };

                const Schedule changes { { numSamples / 5,     makeParams (0.0f, 300.0f, 1.0f, 3.0f) },
                                         { 2 * numSamples / 5, makeParams (60.0f, 40.0f, 0.3f, -3.0f) },
                                         { numSamples / 2,     makeParams (60.0f, 40.0f, 0.3f, -3.0f, true) },
                                         { 3 * numSamples / 5, makeParams (10.0f, 120.0f, 0.0f, 1.0f) },
                                         { 4 * numSamples / 5, allStagesParams() } };

                ClayModule clay;
                prepareClay (clay, sampleRate, 2, allStagesParams());
                const auto output = runClay (clay, input, 333, changes);

                double worstSide = 0.0;

                for (size_t i = 0; i < output[0].size(); ++i)
                    worstSide = juce::jmax (worstSide, std::abs (0.5 * (static_cast<double> (output[0][i]) - static_cast<double> (output[1][i]))));

                logMessage ("  worst side from a pure mid input: " + juce::String (toDb (worstSide), 1) + " dBFS");
                expectLessOrEqual (worstSide, 1.0e-6, "side must stay at or below -120 dBFS");

                ClayModule decorrelated;
                prepareClay (decorrelated, sampleRate, 2, allStagesParams());
                const auto stereoOutput = runClay (decorrelated, makeProgramme (numSamples, sampleRate, -12.0f, 22), 333, changes);
                const auto side = sideOf (stereoOutput);
                double largestSide = 0.0;

                for (auto value : side)
                    largestSide = juce::jmax (largestSide, std::abs (value));

                expectGreaterThan (largestSide, 0.01, "positive control: decorrelated input does carry side");
            }

            // 验收「左右互换对称」：处理 (A, B) 与 (B, A)，输出应正好左右互换。M 不变、S 变号；① 两声道同一滤波器；② ③ 是
            // 线性的、只作用于 S（滑动和、淡化、钳位对取负都精确对称），而 IEEE 运算对取负精确对称（x − y = −(y − x)，就近舍入对称），
            // 所以要求逐位。时间表里有低切关→开的淡化、分频点换长度与 a 的平滑、加宽正负、旁通来回，覆盖所有淡化路径。
            void testChannelSwapSymmetry()
            {
                beginTest ("swapping left and right swaps the output exactly");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (1.5 * sampleRate);
                const auto input = makeProgramme (numSamples, sampleRate, -18.0f, 31);
                const Signal swapped { input[1], input[0] };

                const Schedule changes { { numSamples / 6,     makeParams (0.0f, 60.0f, 1.0f, -3.0f) },
                                         { numSamples / 3,     makeParams (45.0f, 250.0f, 0.4f, 3.0f) },
                                         { numSamples / 2,     makeParams (45.0f, 250.0f, 0.4f, 3.0f, true) },
                                         { 2 * numSamples / 3, makeParams (12.0f, 90.0f, 0.9f, 1.0f) } };

                ClayModule forward, mirrored;
                prepareClay (forward, sampleRate, 2, allStagesParams());
                prepareClay (mirrored, sampleRate, 2, allStagesParams());
                const auto direct = runClay (forward, input, 333, changes);
                const auto reflected = runClay (mirrored, swapped, 333, changes);
                const Signal reflectedBack { reflected[1], reflected[0] };

                expectEquals (countDifferentBits (direct, reflectedBack), 0, "the swapped input must give the exactly swapped output");
                expect (countDifferentBits (Signal { direct[0] }, Signal { direct[1] }) > numSamples / 2,
                        "positive control: the two output channels differ, so the comparison is not trivial");
            }

            // 规格第 4 节「单声道输入：只做 ①，② ③ 跳过」+ 根治修订「单声道 = 低切 + 延迟 D」。单声道模块（prepare 1 声道）开满三级：
            //  - 输出 = 立体声模块在 L = R = 同一信号（纯 M：② ③ 对它是 0 进 0 出）时的 L，数值相同；
            //  - 低切确实在起作用（输出 ≠ 延迟 D 的输入）；
            //  - 立体声模块收到 1 声道的 buffer：同样只做 ①，与单声道模块逐位相同；
            //  - 低切关掉、a 与 widthDb 仍开满：单声道输出逐位等于延迟 D 的输入，isPassThrough() 为真（单声道没有 ② ③）。
            void testMonoInputOnlyLowCut()
            {
                beginTest ("mono input: only the low cut is applied, then the fixed delay");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (1.0 * sampleRate);
                auto mono = makePinkNoise (1, numSamples, -18.0f, 41);
                addSine (mono[0], sampleRate, 20.0, 0.3f);
                const auto everything = makeParams (30.0f, 300.0f, 1.0f, 3.0f);
                const auto delayedMono = delayedBy (mono, specDelay (sampleRate));

                ClayModule monoClay;
                prepareClay (monoClay, sampleRate, 1, everything);
                const auto monoOut = runClay (monoClay, mono, 333);

                ClayModule stereoClay;
                prepareClay (stereoClay, sampleRate, 2, everything);
                const auto stereoOut = runClay (stereoClay, Signal { mono[0], mono[0] }, 333);

                expectEquals (maxAbsDifference (monoOut, Signal { stereoOut[0] }, 0, numSamples), 0.0f,
                              "mono output equals stereo L = R output (only the low cut acts)");
                expect (maxAbsDifference (delayedMono, monoOut, 0, numSamples) > 1.0e-3f, "the low cut must actually act on mono input");

                ClayModule stereoGivenMono;
                prepareClay (stereoGivenMono, sampleRate, 2, everything);
                expectEquals (countDifferentBits (monoOut, runClay (stereoGivenMono, mono, 333)), 0,
                              "a stereo-prepared module given a 1-channel buffer does the same");

                ClayModule noCut;
                prepareClay (noCut, sampleRate, 1, makeParams (0.0f, 300.0f, 1.0f, 3.0f));
                expect (noCut.isPassThrough(), "mono with the low cut off is a pure delay whatever a and widthDb are");
                expectEquals (countDifferentBits (delayedMono, runClay (noCut, mono, 17)), 0, "mono with the low cut off is the bit-exact delayed input");
            }

            // 规格第 4 节 ③：S 的 RBJ 高架，f0 = 2 kHz、斜率 S = 1，增益 widthDb。RBJ 高架在 f0 处增益恰为一半（dB）、
            // 在 Nyquist 恰为全增益；16 kHz 在 44.1 ～ 192 kHz 下离平台 ≤ 0.001 dB，100 Hz 处 ≤ 0.00002 dB（独立解析计算）。
            // 验收「高架增益误差 ±0.1 dB」：纯 S 正弦 16 kHz 增益 = widthDb ± 0.1 dB、2 kHz = widthDb / 2 ± 0.1 dB；
            // 另验 100 Hz 的 S（±0.02 dB）与 16 kHz 的纯 M（±0.001 dB：S = 0，③ 不碰 M）不受影响，纯 S 输入时输出 M 恰为 0。
            void testWidthShelfGain()
            {
                beginTest ("width shelf: 16 kHz side gain = widthDb and 2 kHz = widthDb / 2 (+-0.1 dB); mid and low side untouched");

                for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
                {
                    for (auto width : { -3.0f, -1.5f, 0.5f, 1.5f, 3.0f })
                    {
                        const auto widthDb = static_cast<double> (width);
                        const auto p = makeParams (0.0f, 120.0f, 0.0f, width);
                        const auto tag = juce::String (widthDb) + " dB @ " + juce::String (sampleRate);

                        const auto high   = measureSide (sampleRate, p, 16000.0);
                        const auto corner = measureSide (sampleRate, p, 2000.0);
                        const auto low    = measureSide (sampleRate, p, 100.0);

                        expectWithinAbsoluteError (high.gainDb, widthDb, 0.1, "16 kHz side gain " + tag);
                        expectWithinAbsoluteError (corner.gainDb, 0.5 * widthDb, 0.1, "2 kHz side gain " + tag);
                        expectWithinAbsoluteError (low.gainDb, 0.0, 0.02, "100 Hz side gain " + tag);
                        expectWithinAbsoluteError (measureMidGainDb (sampleRate, p, 16000.0), 0.0, 0.001, "16 kHz mid gain " + tag);
                        expectEquals (high.maxMidOut, 0.0, "pure side input keeps mid at exactly 0 " + tag);
                    }
                }
            }

            // 验收「17 / 333 / 2048 块长输出一致」（根治修订第 4 节第 3 条「块长 1 / 17 / 333 / 4096 逐位相同」）。系数与分频点切换
            // 都在从 reset 起算的固定 64 样本网格上、平滑按样本计数、延迟线是环，与块边界无关：
            //  (a) 参数静止（prepare 前设好，reset 落到目标）：17 / 333 / 2048，再加 1 与 4096（> prepare 块长），全部逐位相同；
            //  (b) 参数扫动：在同样的样本位置改参数（块在那里切开）——每次改动都触发 20 ms 平滑 / 淡化 / 低通切换、跨过多个网格点，
            //      17 / 333 / 2048 同样逐位相同（容差 0）。宿主按块自动化时，块长不同则参数到达的时刻本身不同，那部分差异来自
            //      参数时刻而非模块，这里不测。
            void testBlockSizeIndependence()
            {
                beginTest ("output is identical for block sizes 1 / 17 / 333 / 2048 / 4096 (static and with sweeps at the same sample positions)");

                for (auto sampleRate : { 44100.0, 48000.0 })
                {
                    const auto numSamples = static_cast<int> (2.0 * sampleRate);
                    const auto input = makeProgramme (numSamples, sampleRate, -18.0f, 61);
                    const auto dry = delayedBy (input, specDelay (sampleRate));
                    const auto tag = " @ " + juce::String (sampleRate);

                    Signal reference;

                    for (auto blockSize : { 17, 333, 2048, 1, 4096 })
                    {
                        ClayModule clay;
                        prepareClay (clay, sampleRate, 2, allStagesParams());
                        const auto output = runClay (clay, input, blockSize);

                        if (reference.empty())
                        {
                            reference = output;
                            expect (countDifferentBits (dry, output) > numSamples, "processing must actually change the signal" + tag);
                        }
                        else
                        {
                            expectEquals (countDifferentBits (reference, output), 0, "static parameters, block " + juce::String (blockSize) + tag);
                        }
                    }

                    const Schedule changes { { 7001,                makeParams (0.0f, 150.0f, 0.8f, 2.0f) },
                                             { 7050,                makeParams (0.0f, 150.0f, 0.8f, -3.0f) },
                                             { numSamples / 4 + 3,  makeParams (60.0f, 40.0f, 0.0f, -3.0f) },
                                             { numSamples / 4 + 500, makeParams (60.0f, 40.0f, 1.0f, -3.0f) },
                                             { numSamples / 2 + 11, makeParams (60.0f, 300.0f, 1.0f, 3.0f, true) },
                                             { numSamples / 2 + 900, makeParams (60.0f, 300.0f, 1.0f, 3.0f) },
                                             { numSamples / 2 + 930, makeParams (60.0f, 90.0f, 1.0f, 3.0f) },
                                             { 3 * numSamples / 4 + 29, neutralParams() },
                                             { 7 * numSamples / 8 + 5, allStagesParams() } };

                    Signal sweptReference;

                    // 块长 1：分频点切换的分摊预热欠着的样本数逐样本跨段带着走（T-020），逐样本调用也必须逐位相同。
                    for (auto blockSize : { 17, 333, 2048, 1 })
                    {
                        ClayModule clay;
                        prepareClay (clay, sampleRate, 2, allStagesParams());
                        const auto output = runClay (clay, input, blockSize, changes);

                        if (sweptReference.empty())
                        {
                            sweptReference = output;
                            expect (countDifferentBits (reference, output) > numSamples / 4, "the sweeps must change the output" + tag);
                        }
                        else
                        {
                            expectEquals (countDifferentBits (sweptReference, output), 0, "sweeps, block " + juce::String (blockSize) + tag);
                        }
                    }
                }
            }

            // 验收「参数扫动无咔哒」：任一参数在 20 ms 内从最小扫到最大（目标跳变，由模块自己的 20 ms 平滑 / 分频点切换淡化扫过去）
            // 与反向，粉噪（−24 dBFS）+ 30 Hz 正弦输入下，输出经 4 kHz 4 阶高通后每 5 ms 的能量，相对「静止在起点」「静止在终点」两份
            // 参考同窗能量的较大者不突起 > 1 dB（整段所有窗、两个声道；三份输出同样延迟 D，窗口对齐）。
            // 阳性对照：静止输出里加一个 0.5 的单样本咔哒，同一度量必须报出 > 1 dB（Python 原型估算约 +3.6 dB；扫动本身约 0.001 dB）。
            void testParameterSweepsAreClickFree()
            {
                beginTest ("parameter sweeps (min <-> max within 20 ms) raise no 5 ms high-band energy by more than 1 dB");

                constexpr double sampleRate = 48000.0;
                const auto numSamples   = static_cast<int> (0.6 * sampleRate);
                const auto sweepAt      = static_cast<int> (0.4 * sampleRate);
                const auto windowLength = static_cast<int> (0.005 * sampleRate);
                const auto input = makeProgramme (numSamples, sampleRate, -24.0f, 51);

                struct Sweep
                {
                    const char* label;
                    ClayParams start, end;
                };

                const Sweep sweeps[] = {
                    { "low cut off -> 60 Hz",           makeParams (0.0f, 120.0f, 0.0f, 0.0f),  makeParams (60.0f, 120.0f, 0.0f, 0.0f) },
                    { "low cut 60 Hz -> off",           makeParams (60.0f, 120.0f, 0.0f, 0.0f), makeParams (0.0f, 120.0f, 0.0f, 0.0f) },
                    { "low cut 10 -> 60 Hz",            makeParams (10.0f, 120.0f, 0.0f, 0.0f), makeParams (60.0f, 120.0f, 0.0f, 0.0f) },
                    { "low cut 60 -> 10 Hz",            makeParams (60.0f, 120.0f, 0.0f, 0.0f), makeParams (10.0f, 120.0f, 0.0f, 0.0f) },
                    { "crossover 40 -> 300 Hz (a = 1)", makeParams (0.0f, 40.0f, 1.0f, 0.0f),   makeParams (0.0f, 300.0f, 1.0f, 0.0f) },
                    { "crossover 300 -> 40 Hz (a = 1)", makeParams (0.0f, 300.0f, 1.0f, 0.0f),  makeParams (0.0f, 40.0f, 1.0f, 0.0f) },
                    { "mono amount 0 -> 1 (300 Hz)",    makeParams (0.0f, 300.0f, 0.0f, 0.0f),  makeParams (0.0f, 300.0f, 1.0f, 0.0f) },
                    { "mono amount 1 -> 0 (300 Hz)",    makeParams (0.0f, 300.0f, 1.0f, 0.0f),  makeParams (0.0f, 300.0f, 0.0f, 0.0f) },
                    { "width -3 -> +3 dB",              makeParams (0.0f, 120.0f, 0.0f, -3.0f), makeParams (0.0f, 120.0f, 0.0f, 3.0f) },
                    { "width +3 -> -3 dB",              makeParams (0.0f, 120.0f, 0.0f, 3.0f),  makeParams (0.0f, 120.0f, 0.0f, -3.0f) } };

                for (const auto& sweep : sweeps)
                {
                    ClayModule swept, atStart, atEnd;
                    prepareClay (swept, sampleRate, 2, sweep.start);
                    prepareClay (atStart, sampleRate, 2, sweep.start);
                    prepareClay (atEnd, sampleRate, 2, sweep.end);

                    const auto sweptOut = runClay (swept, input, maxBlock, { { sweepAt, sweep.end } });
                    const auto startOut = runClay (atStart, input, maxBlock);
                    const auto endOut   = runClay (atEnd, input, maxBlock);

                    const auto rise = worstHighBandRiseDb (sweptOut, startOut, endOut, sampleRate, windowLength);
                    logMessage ("  " + juce::String (sweep.label) + ": worst 5 ms high-band rise " + juce::String (rise, 3) + " dB");
                    expectLessOrEqual (rise, 1.0, juce::String (sweep.label) + ": no click above 4 kHz");
                }

                ClayModule still;
                prepareClay (still, sampleRate, 2, sweeps[0].start);
                const auto stillOut = runClay (still, input, maxBlock);
                auto clicked = stillOut;
                clicked[0][static_cast<size_t> (sweepAt + 480)] += 0.5f;
                const auto detected = worstHighBandRiseDb (clicked, stillOut, stillOut, sampleRate, windowLength);
                logMessage ("  positive control, 0.5 single-sample click: " + juce::String (detected, 2) + " dB");
                expectGreaterThan (detected, 1.0, "positive control: a 0.5 single-sample click must register as more than 1 dB");
            }

            // 根治修订第 2 节「旁通 = 同样的纯延迟干路 + 20 ms 交叉淡化（延迟补偿式旁通）」+ 本实现「旁通时滤波器与延迟线照跑」：
            //  (a) 一开始就旁通（三级全开）：逐位等于延迟 D 的输入，isPassThrough() 为真（此刻输出 = 输入逐位延迟 D）；
            //  (b) 中途旁通：之前与从未旁通的参考逐位相同，[t, t + 20 ms) 在「处理后 → 延迟的干路」线性淡化的包络里，之后逐位等于
            //      延迟的输入；
            //  (c) 解除旁通：[t, t + 20 ms) 在「延迟的干路 → 处理后」的包络里，之后与从未旁通的参考逐位相同——滤波器、低通、延迟线在
            //      旁通期间照跑；阳性对照：t 时刻才从零状态起跑的模块，20 ms 后与参考仍差 > 1e-3（30 Hz 分量的低切起跳瞬态），
            //      说明 (c) 的逐位相同确实来自影子运行；
            //  (d) 三级全关时切旁通（GLAZE 0 下宿主旁通）：整段逐位等于延迟的输入。
            void testBypassCrossfade()
            {
                beginTest ("bypass: bit-exact delayed dry when engaged, 20 ms linear crossfade, seamless return thanks to shadow running");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (2.0 * sampleRate);
                const auto rampLength = static_cast<int> (0.02 * sampleRate);
                const auto onAt  = static_cast<int> (0.8 * sampleRate);
                const auto offAt = static_cast<int> (1.3 * sampleRate);
                const auto input = quieter (makeProgramme (numSamples, sampleRate, -18.0f, 81));   // 守护地板之下（ADR-014）
                const auto dry = delayedBy (input, specDelay (sampleRate));

                const auto active = allStagesParams();
                auto bypassed = active;
                bypassed.bypass = true;

                ClayModule engaged;
                prepareClay (engaged, sampleRate, 2, bypassed);
                expect (engaged.isPassThrough(), "fully bypassed: the output is the input delayed by D");
                expectEquals (countDifferentBits (dry, runClay (engaged, input, 333)), 0, "bypass from the start is the bit-exact delayed dry signal");

                ClayModule never;
                prepareClay (never, sampleRate, 2, active);
                const auto processed = runClay (never, input, 333);
                expect (countDifferentBits (dry, processed) > numSamples, "the active stages must change the signal");

                ClayModule toggled;
                prepareClay (toggled, sampleRate, 2, active);
                const auto output = runClay (toggled, input, 333, { { onAt, bypassed }, { offAt, active } });

                // 旁通淡化在①～③ 输出时刻，再过守护的 G 才到模块输出。
                const auto lookahead = specGuardLookahead (sampleRate);
                const auto engagedAt = onAt + lookahead, releasedAt = offAt + lookahead;
                expectEquals (countDifferentBits (processed, output, 0, engagedAt), 0, "identical to the never-bypassed module before engaging");
                expect (withinCrossfade (output, processed, dry, engagedAt, rampLength, 1.0e-6 * static_cast<double> (quietScale)),
                        "engaging follows a 20 ms linear crossfade to the delayed dry signal");
                expectEquals (countDifferentBits (dry, output, engagedAt + rampLength, releasedAt), 0, "bit-exact delayed dry from 20 ms after engaging");
                expect (withinCrossfade (output, dry, processed, releasedAt, rampLength, 1.0e-6 * static_cast<double> (quietScale)),
                        "releasing follows a 20 ms linear crossfade back");
                expectEquals (countDifferentBits (processed, output, releasedAt + rampLength), 0,
                              "identical to the never-bypassed module from 20 ms after releasing (filters and delay lines kept running)");

                ClayModule restarted;
                prepareClay (restarted, sampleRate, 2, active);
                const auto restartedOut = runClay (restarted, sliceOf (input, offAt, numSamples), 333);
                const auto checkFrom = offAt + rampLength;
                const auto checkTo   = checkFrom + static_cast<int> (0.1 * sampleRate);
                const auto restartGap = maxDifferenceInRange (sliceOf (restartedOut, rampLength, checkTo - offAt),
                                                              sliceOf (processed, checkFrom, checkTo), 0, checkTo - checkFrom);
                expectGreaterThan (restartGap, 1.0e-3f * quietScale, "positive control: a module started from zero state at release differs");

                ClayModule idle;
                prepareClay (idle, sampleRate, 2, neutralParams());
                auto idleBypassed = neutralParams();
                idleBypassed.bypass = true;
                const auto idleOut = runClay (idle, input, 333, { { onAt, idleBypassed }, { offAt, neutralParams() } });
                expectEquals (countDifferentBits (dry, idleOut), 0, "toggling bypass with all stages idle stays a bit-exact pure delay");
            }

            // 规格第 4 节「影子运行：…输出端按实际值选择 / 交叉淡化（20 ms）」+ 根治修订「低切在 M/S 之前」：低切的选择 / 淡化作用在
            // 输入时刻，要过 D 个样本才到输出。
            //  - 25 Hz 开 → t1 关：输出 [t1 + D, t1 + D + 20 ms) 在「低切后 → 延迟的干路」的线性淡化包络内，之后逐位等于延迟的输入；
            //  - t2 再开 25 Hz（关着时截止频率停在 25 Hz，不用滑）：输出 [t2 + D, t2 + D + 20 ms) 在「延迟的干路 → 低切后」的包络内，
            //    之后与从未关过的参考逐位相同——滤波器状态一直是现成的，没有起跳瞬态；
            //  - 阳性对照：t2 时刻才从零状态起跑的模块与参考在 t2 + D + 20 ms 之后仍差 > 1e-3。
            void testLowCutSwitchFadesAndShadowRuns()
            {
                beginTest ("low cut off / on: 20 ms output crossfade D samples later, bit-exact delayed dry while off, no restart transient when back on");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (2.0 * sampleRate);
                const auto rampLength = static_cast<int> (0.02 * sampleRate);
                const auto delay = specDelay (sampleRate);
                const auto offAt = static_cast<int> (0.6 * sampleRate);
                const auto onAt  = static_cast<int> (1.2 * sampleRate);
                const auto input = makeProgramme (numSamples, sampleRate, -18.0f, 91);
                const auto dry = delayedBy (input, delay);

                const auto cutOn  = makeParams (25.0f, 120.0f, 0.0f, 0.0f);
                const auto cutOff = makeParams (0.0f, 120.0f, 0.0f, 0.0f);

                ClayModule reference;
                prepareClay (reference, sampleRate, 2, cutOn);
                const auto filtered = runClay (reference, input, 333);
                expect (maxAbsDifference (dry, filtered, 0, numSamples) > 0.05f, "the 25 Hz low cut must visibly act on the 30 Hz tone");

                ClayModule switched;
                prepareClay (switched, sampleRate, 2, cutOn);
                const auto output = runClay (switched, input, 333, { { offAt, cutOff }, { onAt, cutOn } });

                expectEquals (countDifferentBits (filtered, output, 0, offAt + delay), 0, "identical to the reference until the fade reaches the output");
                expect (withinCrossfade (output, filtered, dry, offAt + delay, rampLength), "switching off fades filtered -> delayed dry over 20 ms");
                expectEquals (countDifferentBits (dry, output, offAt + delay + rampLength, onAt + delay), 0, "bit-exact delayed dry while the low cut is off");
                expect (withinCrossfade (output, dry, filtered, onAt + delay, rampLength), "switching on fades delayed dry -> filtered over 20 ms");
                expectEquals (countDifferentBits (filtered, output, onAt + delay + rampLength), 0,
                              "identical to the never-switched reference 20 ms after switching back on (shadow running)");

                ClayModule cold;
                prepareClay (cold, sampleRate, 2, cutOn);
                const auto coldOut = runClay (cold, sliceOf (input, onAt, numSamples), 333);
                const auto checkFrom = onAt + delay + rampLength;
                const auto checkTo   = checkFrom + static_cast<int> (0.1 * sampleRate);
                const auto coldGap = maxDifferenceInRange (sliceOf (coldOut, checkFrom - onAt, checkTo - onAt),
                                                           sliceOf (filtered, checkFrom, checkTo), 0, checkTo - checkFrom);
                expectGreaterThan (coldGap, 1.0e-3f, "positive control: a low cut started from zero state has a restart transient");
            }

            // 头文件：reset 清空全部状态（含低通与延迟线）、平滑器直接落到当前目标。
            //  (a) 三级全开跑过一段别的信号后 reset，再跑测试信号：与新建模块逐位相同（没有残留状态）；
            //  (b) 刚 setParams 新值（平滑刚开始、分频点要换）就 reset：之后与「prepare 前就设好新值」的模块逐位相同（没有斜坡与淡化）；
            //  (c) 三级全开时 setParams 全 0：淡出中不是纯延迟；reset 后立刻是，输出逐位等于延迟 D 的输入。
            void testResetClearsStateAndLandsSmoothers()
            {
                beginTest ("reset clears every filter, low-pass and delay line and lands the smoothers on their targets");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (0.5 * sampleRate);
                const auto input = makeProgramme (numSamples, sampleRate, -18.0f, 111);
                const auto other = makeProgramme (numSamples, sampleRate, -12.0f, 112);

                ClayModule fresh;
                prepareClay (fresh, sampleRate, 2, allStagesParams());
                const auto expected = runClay (fresh, input, 333);

                ClayModule used;
                prepareClay (used, sampleRate, 2, allStagesParams());
                runClay (used, other, 333);
                used.reset();
                expectEquals (countDifferentBits (expected, runClay (used, input, 333)), 0, "after reset the output equals a freshly prepared module");

                const auto target = makeParams (55.0f, 250.0f, 0.4f, -2.0f);
                ClayModule direct;
                prepareClay (direct, sampleRate, 2, target);
                const auto expectedTarget = runClay (direct, input, 333);

                ClayModule landed;
                prepareClay (landed, sampleRate, 2, allStagesParams());
                runClay (landed, other, 333);
                landed.setParams (target);
                landed.reset();
                expectEquals (countDifferentBits (expectedTarget, runClay (landed, input, 333)), 0,
                              "reset right after setParams lands on the new values without a ramp");

                ClayModule toZero;
                prepareClay (toZero, sampleRate, 2, allStagesParams());
                runClay (toZero, other, 333);
                expect (! toZero.isPassThrough(), "active stages are not a pure delay");
                toZero.setParams (neutralParams());
                expect (! toZero.isPassThrough(), "still fading out: not yet a pure delay");
                toZero.reset();
                expect (toZero.isPassThrough(), "reset lands every fade on zero and empties the delay lines: a pure delay immediately");
                expectEquals (countDifferentBits (delayedBy (input, specDelay (sampleRate)), runClay (toZero, input, 17)), 0,
                              "and the output is the bit-exact delayed input");
            }

            // 音频线程 0 分配（TestMain.cpp 的 operator new 计数）：setParams（各种参数、含旁通切换；分频点每次都变，模块在网格点上
            // 重建 / 预热另一套低通）、process（立体声 / 单声道、块长 1 ～ 4096，大于 prepare 的 512）、reset、isPassThrough 都不经
            // operator new 分配。缓冲区在计数区外准备。
            void testAudioPathDoesNotAllocate()
            {
                beginTest ("setParams / process / reset never allocate (stereo and mono, blocks up to 4096, crossover switches)");

                constexpr double sampleRate = 48000.0;
                constexpr int largestBlock = 4096;
                const auto source = makeProgramme (largestBlock, sampleRate, -18.0f, 121);

                juce::AudioBuffer<float> stereo (2, largestBlock), mono (1, largestBlock);

                ClayModule stereoClay, monoClay;
                prepareClay (stereoClay, sampleRate, 2, allStagesParams());
                prepareClay (monoClay, sampleRate, 1, allStagesParams());

                const ClayParams settings[] = { allStagesParams(), neutralParams(), makeParams (60.0f, 40.0f, 1.0f, -3.0f, true),
                                                makeParams (10.0f, 300.0f, 0.5f, 3.0f) };
                constexpr int numSettings = static_cast<int> (sizeof (settings) / sizeof (settings[0]));

                std::uint64_t allocations = 0;
                int round = 0;

                for (auto blockSize : { 1, 17, 64, 333, 512, 2048, largestBlock })
                {
                    for (int repeat = 0; repeat < 8; ++repeat)
                    {
                        stereo.setSize (2, blockSize, false, false, true);
                        mono.setSize (1, blockSize, false, false, true);

                        for (int ch = 0; ch < 2; ++ch)
                            stereo.copyFrom (ch, 0, source[static_cast<size_t> (ch)].data(), blockSize);

                        mono.copyFrom (0, 0, source[0].data(), blockSize);

                        const auto& current = settings[static_cast<size_t> (round++ % numSettings)];
                        const auto before = allocationCountOnThisThread();
                        stereoClay.setParams (current);
                        stereoClay.process (stereo);
                        monoClay.setParams (current);
                        monoClay.process (mono);
                        allocations += allocationCountOnThisThread() - before;
                    }
                }

                const auto before = allocationCountOnThisThread();
                stereoClay.reset();
                monoClay.reset();
                const auto passThrough = stereoClay.isPassThrough() || monoClay.isPassThrough();
                allocations += allocationCountOnThisThread() - before;

                juce::ignoreUnused (passThrough);
                expectEquals (static_cast<juce::int64> (allocations), static_cast<juce::int64> (0), "the audio path never allocates");
            }

            // ADR-014 峰值守护的保证（T-032 2026-09-29 修订：守护只管①②，③ 加宽的增量 δ 不进守护、在守护输出上原样加回）。
            //  (1) ①② 的每个输出样本 |out| ≤ B = max(E, τ·P, F)。E、P 在这里按 ADR 的定义独立算（朴素 O(N·W) 的窗口最大，不用实现里的
            //      分段窗）：E(n) = 延迟后的干路两声道 |x| 在 [n − W, n + W] 的最大，W = round(0.008 fs)；P(n) = max(E(n), P(n − 1)·exp(−1 / fs))；
            //      τ = 10^(−6/20)；F = 0.01。容差：float 输出的舍入（相对 1e-6）。加宽都为 0，块长 333，三组：
            //        a. 立体声「母带式」：−12 dBFS 粉噪 + 30 Hz 正弦（0.3 / 0.15），只开低切 60 Hz；
            //        b. 立体声「谐波对」：纯 M 的 120 + 240 Hz（各 0.25，起始相位 0）+ 两声道独立的 −40 dBFS 粉噪（收单有侧声道可收），
            //           低切 60 Hz + 收单 300 Hz a = 1——4 阶 Butterworth 高通在 2fc / 4fc 处相位超前约 78° / 38°，两者的相对相位转过约
            //           40°，波峰从 0.25 · 1.760 抬到约 0.25 · 1.976（+1.0 dB；推导同 testPeakGuardLeavesSteadyLowTonesAlone）；
            //        c. 单声道（只有低切）：a 的左声道，低切 60 Hz。
            //      阳性对照：同一素材缩到 2^−10 跑完再放大回来 = 不带守护的①② 输出（①② 线性；2 的幂缩放在 float / double 里都精确、
            //      不下溢；那次运行守护统计必须为 0，参照才可信），它必须在某些样本上越过同一个界 0.5 dB 以上（守护确实有活干），并且
            //      模块报告 g < 1 的样本数 > 0。改前 a 组是「低切 60 + 收单 300 + 加宽 +3 dB」，越界几乎全来自加宽——只开①② 时那份素材
            //      上一个样本都不越界（改前 e6b445b 实测 −0.01 dB），所以 T-032 起 a 组只开低切，收单由 b 组提供。
            //  (2) ③ 的拆分（ADR-014 修订后的决定 1「|输出 − ③ 增量| ≤ B」）：两组立体声再各开 +3 dB 加宽，与加宽 0 的模块逐块并排跑。
            //      守护只看①②：两边的守护统计（g < 1 的样本数、最小 g）与每块的代价格逐位相同；输出 = 加宽 0 的守护输出 + δ，
            //      δ = 不带守护的①②③ 输出 − 不带守护的①② 输出（参照同上），逐样本差 ≤ 2e-6（三次 float 舍入）——守护在动作的样本
            //      （加宽 0 的输出 ≠ 不带守护的①② 输出）也一样，且这种样本必须有（g < 1 那一支确实加回了 δ）。改前加宽进守护，这个差
            //      达 0.62（e6b445b 实测）。加宽本身不受界约束：只记录它越过界多少（素材相关，不断言）。
            void testPeakGuardBoundsEveryOutputSample()
            {
                beginTest ("peak guard (ADR-014, T-032): every output sample of stages 1-2 stays within max(local dry peak, -6 dB of the peak envelope, -40 dBFS); the width increment rides on top, unguarded");

                constexpr double sampleRate = 48000.0;
                constexpr int blockSize = 333;
                constexpr double relative = 0.50118723362727224;
                constexpr double floorLevel = 0.01;
                constexpr float referenceScale = 1.0f / 1024.0f;   // 2^−10（−60.2 dB）
                const auto numSamples = static_cast<int> (3.0 * sampleRate);
                const auto delay = specDelay (sampleRate);
                const auto window = static_cast<int> (std::lround (0.008 * sampleRate));
                const auto retain = std::exp (-1.0 / sampleRate);

                // 界 B(n)：输出时刻 n 的干路 = 输入延迟 D（本文件口径 = D + G）。末尾 W 个输出样本的 E 的窗超出了输入末尾（实现在那里
                // 还没看到后面的零），不算——只给前 numSamples − W 个。
                const auto boundsOf = [&] (const Signal& input)
                {
                    const auto dry = delayedBy (input, delay);
                    std::vector<double> magnitude (static_cast<size_t> (numSamples), 0.0);

                    for (const auto& channel : dry)
                        for (int n = 0; n < numSamples; ++n)
                            magnitude[static_cast<size_t> (n)] = juce::jmax (magnitude[static_cast<size_t> (n)],
                                                                             std::abs (static_cast<double> (channel[static_cast<size_t> (n)])));

                    std::vector<double> bounds;
                    auto envelope = 0.0;

                    for (int n = 0; n + window < numSamples; ++n)
                    {
                        auto localPeak = 0.0;

                        for (int k = juce::jmax (0, n - window); k <= n + window; ++k)
                            localPeak = juce::jmax (localPeak, magnitude[static_cast<size_t> (k)]);

                        envelope = juce::jmax (localPeak, envelope * retain);
                        bounds.push_back (juce::jmax (localPeak, juce::jmax (relative * envelope, floorLevel)));
                    }

                    return bounds;
                };

                // 不带守护的参照：素材缩到 2^−10（峰值 < −55 dBFS，①②③ 之后仍远在地板 F 之下）跑完再放大回来。
                const auto unguardedOutput = [&] (const Signal& input, const ClayParams& p, const juce::String& tag)
                {
                    ClayModule clay;
                    prepareClay (clay, sampleRate, static_cast<int> (input.size()), p);
                    clay.getAndResetPeakGuardStats();
                    const auto out = runClay (clay, scaledBy (input, referenceScale), blockSize);
                    expectEquals (static_cast<int> (clay.getAndResetPeakGuardStats().guardedSamples), 0,
                                  "fixture: at 2^-10 the guard never engages, so the reference is the unguarded output: " + tag);
                    return scaledBy (out, 1.0f / referenceScale);
                };

                struct Case
                {
                    juce::String tag;
                    Signal input;
                    ClayParams params;
                    double minIncrement;   // (2) 的阳性对照：+3 dB 加宽在这份素材上至少加出这么多（纯 M 的谐波对只有 −40 dBFS 的侧声道）
                };

                const auto programme = makeProgramme (numSamples, sampleRate, -12.0f, 501);
                auto pair = makePinkNoise (2, numSamples, -40.0f, 503);

                for (auto& channel : pair)
                {
                    addSine (channel, sampleRate, 120.0, 0.25f);
                    addSine (channel, sampleRate, 240.0, 0.25f);
                }

                const Case cases[] = { { "stereo programme, low cut 60 Hz", programme, makeParams (60.0f, 300.0f, 0.0f, 0.0f), 0.01 },
                                       { "stereo 120 + 240 Hz pair, low cut 60 Hz + mono 300 Hz", pair, makeParams (60.0f, 300.0f, 1.0f, 0.0f), 0.0 },
                                       { "mono programme, low cut 60 Hz", Signal { programme[0] }, makeParams (60.0f, 120.0f, 0.0f, 0.0f), 0.0 } };

                for (const auto& c : cases)
                {
                    const auto channels = static_cast<int> (c.input.size());
                    const auto stereo = channels == 2;
                    auto widenedParams = c.params;
                    widenedParams.widthDb = 3.0f;

                    // 加宽 0 与 +3 dB 两个模块逐块并排跑（单声道没有 ③，只跑加宽 0），每块比代价格。
                    ClayModule plain, widened;
                    prepareClay (plain, sampleRate, channels, c.params);
                    prepareClay (widened, sampleRate, channels, widenedParams);
                    plain.getAndResetPeakGuardStats();
                    widened.getAndResetPeakGuardStats();

                    auto output = c.input, widenedOutput = c.input;
                    juce::AudioBuffer<float> block (channels, blockSize), widenedBlock (channels, blockSize);
                    auto samePressure = true;

                    for (int start = 0; start < numSamples; start += blockSize)
                    {
                        const auto length = juce::jmin (blockSize, numSamples - start);
                        block.setSize (channels, length, false, false, true);
                        widenedBlock.setSize (channels, length, false, false, true);

                        for (int ch = 0; ch < channels; ++ch)
                        {
                            block.copyFrom (ch, 0, c.input[static_cast<size_t> (ch)].data() + start, length);
                            widenedBlock.copyFrom (ch, 0, c.input[static_cast<size_t> (ch)].data() + start, length);
                        }

                        plain.process (block);

                        if (stereo)
                        {
                            widened.process (widenedBlock);
                            const auto p = plain.getLastProcessPressure();
                            const auto q = widened.getLastProcessPressure();
                            samePressure = samePressure && std::memcmp (&p.meanDb, &q.meanDb, sizeof (float)) == 0 && p.seconds == q.seconds;
                        }

                        for (int ch = 0; ch < channels; ++ch)
                        {
                            const auto index = static_cast<size_t> (ch);
                            std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, output[index].begin() + start);
                            std::copy (widenedBlock.getReadPointer (ch), widenedBlock.getReadPointer (ch) + length, widenedOutput[index].begin() + start);
                        }
                    }

                    const auto stats = plain.getAndResetPeakGuardStats();
                    const auto unguarded = unguardedOutput (c.input, c.params, c.tag);
                    const auto bounds = boundsOf (c.input);

                    auto violations = 0;
                    auto worstRatio = 0.0, unguardedWorstRatio = 0.0;

                    for (size_t n = 0; n < bounds.size(); ++n)
                    {
                        for (size_t ch = 0; ch < c.input.size(); ++ch)
                        {
                            const auto value = std::abs (static_cast<double> (output[ch][n]));
                            const auto raw = std::abs (static_cast<double> (unguarded[ch][n]));
                            worstRatio = juce::jmax (worstRatio, value / bounds[n]);
                            unguardedWorstRatio = juce::jmax (unguardedWorstRatio, raw / bounds[n]);

                            if (value > bounds[n] * (1.0 + 1.0e-6))
                                ++violations;
                        }
                    }

                    logMessage ("  " + c.tag + ": worst |out| / bound " + juce::String (worstRatio, 7) + "; without the guard "
                                + juce::String (20.0 * std::log10 (unguardedWorstRatio), 2) + " dB over; guarded samples "
                                + juce::String (stats.guardedSamples) + ", min g " + juce::String (stats.minGain, 4));
                    expectEquals (violations, 0, "no output sample of stages 1-2 exceeds the bound: " + c.tag);
                    expectGreaterThan (unguardedWorstRatio, 1.06, "positive control: stages 1-2 alone exceed the bound by > 0.5 dB: " + c.tag);
                    expect (stats.guardedSamples > 0 && stats.minGain < 1.0f, "positive control: the guard engaged: " + c.tag);

                    if (! stereo)
                        continue;

                    // (2) ③ 的拆分。
                    const auto widenedStats = widened.getAndResetPeakGuardStats();
                    expect (widenedStats.guardedSamples == stats.guardedSamples
                              && std::memcmp (&widenedStats.minGain, &stats.minGain, sizeof (float)) == 0 && samePressure,
                            "the width does not enter the guard: identical guard statistics and cost blocks with and without +3 dB: " + c.tag);

                    const auto unguardedWidened = unguardedOutput (c.input, widenedParams, c.tag + ", +3 dB");
                    auto worstSplit = 0.0, worstGuardedSplit = 0.0, largestIncrement = 0.0, widenedWorstRatio = 0.0;
                    auto guardActing = 0;

                    for (size_t n = 0; n < output[0].size(); ++n)
                    {
                        for (size_t ch = 0; ch < 2; ++ch)
                        {
                            const auto increment = static_cast<double> (unguardedWidened[ch][n]) - static_cast<double> (unguarded[ch][n]);
                            const auto split = std::abs (static_cast<double> (widenedOutput[ch][n]) - static_cast<double> (output[ch][n]) - increment);
                            worstSplit = juce::jmax (worstSplit, split);
                            largestIncrement = juce::jmax (largestIncrement, std::abs (increment));

                            if (std::memcmp (&output[ch][n], &unguarded[ch][n], sizeof (float)) != 0)
                            {
                                ++guardActing;
                                worstGuardedSplit = juce::jmax (worstGuardedSplit, split);
                            }

                            if (n < bounds.size())
                                widenedWorstRatio = juce::jmax (widenedWorstRatio, std::abs (static_cast<double> (widenedOutput[ch][n])) / bounds[n]);
                        }
                    }

                    logMessage ("  " + c.tag + ", +3 dB width: worst |out(+3 dB) - out(0 dB) - delta| " + juce::String (worstSplit, 9)
                                + " (" + juce::String (worstGuardedSplit, 9) + " on the " + juce::String (guardActing)
                                + " channel samples the guard pulls back), largest |delta| " + juce::String (largestIncrement, 4)
                                + "; the widened output reaches " + juce::String (20.0 * std::log10 (widenedWorstRatio), 2)
                                + " dB relative to the bound (the increment is not guarded)");
                    expectLessOrEqual (worstSplit, 2.0e-6, "the width increment rides unchanged on the guarded output of stages 1-2: " + c.tag);
                    expect (guardActing > 0, "the split is checked where the guard acts (g < 1): " + c.tag);
                    expectGreaterThan (largestIncrement, c.minIncrement, "positive control: +3 dB of width really adds something: " + c.tag);
                }
            }

            // ADR-014「W = 8 ms 盖住低切在 30 Hz 以上造成的峰值前移，稳态正弦幅度不变时守护不动作」：低切 20 Hz 下 40 Hz、低切 30 Hz
            // 下 60 Hz、低切 60 Hz 下 120 Hz 的 0.5 幅度正弦（纯 M、两声道相同；三处都在 2·fc，4 阶高通相位超前约 78°，前移 5.4 / 3.6 /
            // 1.8 ms），稳态（1 s 之后）的 1 s 里 g < 1 的样本数为 0；收单 120 Hz a = 1 下 50 Hz 的纯 S 正弦（收单只减小 S）同样为 0；
            // T-032 起加宽③不进守护：纯 S 的 16 kHz 正弦 + 3 dB 加宽（幅度真的从 0.5 变到 0.71）守护同样不动（它的增量在守护之外）。
            // 阳性对照（T-032 起改为①路）：60 + 120 Hz 的谐波对（纯 M，各 0.25，二次谐波相位 0）经 30 Hz 低切——4 阶 Butterworth 高通在
            // 2fc / 4fc 处相位超前约 78° / 38°，两者的相对相位转过约 40°，波形峰值从 0.25 · 1.760（sin θ + sin 2θ 的最大）抬到约
            // 0.25 · 1.976（sin θ + sin(2θ − 118°) 的最大），+1.0 dB；E 的窗（±8 ms）几乎盖住整个 16.7 ms 周期，界 ≈ 输入峰值，
            // 所以守护必须动作，峰值处 g_req ≈ (0.440 − 0.287) / (0.494 − 0.287) ≈ 0.74。
            void testPeakGuardLeavesSteadyLowTonesAlone()
            {
                beginTest ("peak guard (ADR-014): steady tones through the low cut / mono / width are left alone; a harmonic pair whose crest the low cut raises is not");

                constexpr double sampleRate = 48000.0;
                const auto settle = static_cast<int> (1.0 * sampleRate);
                const auto numSamples = static_cast<int> (2.0 * sampleRate);

                struct Case { float lowCutHz; float monoAmount; double frequency; bool side; float widthDb; };
                const Case cases[] = { { 20.0f, 0.0f, 40.0, false, 0.0f },
                                       { 30.0f, 0.0f, 60.0, false, 0.0f },
                                       { 60.0f, 0.0f, 120.0, false, 0.0f },
                                       { 0.0f, 1.0f, 50.0, true, 0.0f },
                                       { 0.0f, 0.0f, 16000.0, true, 3.0f } };

                const auto guardStatsFor = [&] (const Signal& input, const ClayParams& p)
                {
                    ClayModule clay;
                    prepareClay (clay, sampleRate, 2, p);
                    runClay (clay, sliceOf (input, 0, settle), 512);
                    clay.getAndResetPeakGuardStats();
                    runClay (clay, sliceOf (input, settle, numSamples), 512);
                    return clay.getAndResetPeakGuardStats();
                };

                for (const auto& c : cases)
                {
                    const auto tag = juce::String (c.frequency) + " Hz " + (c.side ? "side" : "mid") + ", low cut " + juce::String (c.lowCutHz)
                                   + ", mono " + juce::String (c.monoAmount) + ", width " + juce::String (c.widthDb);
                    auto input = makeSineOfLength (2, sampleRate, c.frequency, 0.5f, numSamples);

                    if (c.side)
                        for (auto& sample : input[1])
                            sample = -sample;

                    const auto stats = guardStatsFor (input, makeParams (c.lowCutHz, 120.0f, c.monoAmount, c.widthDb));
                    expectEquals (static_cast<int> (stats.guardedSamples), 0, "steady tone: the guard never engages " + tag);
                }

                auto pair = makeSineOfLength (2, sampleRate, 60.0, 0.25f, numSamples);

                for (auto& channel : pair)
                    addSine (channel, sampleRate, 120.0, 0.25f);

                const auto stats = guardStatsFor (pair, makeParams (30.0f, 120.0f, 0.0f, 0.0f));
                logMessage ("  60 + 120 Hz pair through the 30 Hz low cut: guarded samples " + juce::String (stats.guardedSamples)
                            + " of " + juce::String (numSamples - settle) + ", min g " + juce::String (stats.minGain, 4));
                expect (stats.guardedSamples > 0 && stats.minGain < 0.9f,
                        "positive control: the low cut's phase rotation raises the crest of a 60 + 120 Hz pair and the guard pulls it back");
            }

            // T-032 R4-01（2026-09-29）：加宽③在正常电平下不再被峰值守护回收。修订前，界 B 在干路自己就是 ±8 ms 窗最大值的那些样本上
            // 没有余量（B = E = |x|），加宽在那里同向增加 → g_req 恰为 0 → 100 ms 释放追不上每约 16 ms 一个的局部峰值。修订后③ 的增量
            // 绕过守护：
            //  - 20 s 宽粉噪（两声道独立、峰值归一到 −12 dBFS）加宽 +3 / +1.5 / −3 dB：延迟对齐后侧声道 4 kHz 以上能量增益 = widthDb ± 0.3 dB
            //    （高架 16 kHz = widthDb、2 kHz = widthDb / 2；4 kHz 以上的粉噪按能量加权约 0.99 widthDb，标称实测 2.968 / 1.484 / −2.966 dB），
            //    且与标称（缩到 2^−10 的同一素材，守护不介入）差 ≤ 0.02 dB（δ 只多 float 舍入）；①② 都关着 → 守护看到的 y 与干路逐位相同
            //    → 守护统计恰为 0（加宽不进守护）；
            //  - 纯 S 16 kHz 幅度 0.5 加宽 +3 dB：输出侧声道 +3.0 ± 0.1 dB（拟合幅度）。
            // 阳性对照 = 修订前的版本（e6b445b；107 与云端容器实测相同）在这两条上分别只给 +0.316 / +0.148 / −2.118 dB（g < 1 占 99.9 / 99.9 / 43 % 的样本，
            // 最小 g 为 0）与 0.000 dB，每一条都不过——R4 / V4 各自独立复现的 +0.25 ～ +0.36 dB 与 0.00 dB 同一量级。
            void testWidthIsNotRecoveredByPeakGuard()
            {
                beginTest ("width at normal level (T-032): the side band above 4 kHz gains widthDb +-0.3 dB and the peak guard never touches stage 3");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (20.0 * sampleRate);
                const auto input = normalisedToPeak (makePinkNoise (2, numSamples, -18.0f, 601), -12.0f);

                for (auto width : { 3.0f, 1.5f, -3.0f })
                {
                    const auto tag = "width " + juce::String (width) + " dB";
                    const auto effect = measureGuardedEffect (sampleRate, input, makeParams (0.0f, 120.0f, 0.0f, width), true, 4000.0, true);
                    logMessage ("  " + tag + " on -12 dBFS wide pink noise, side > 4 kHz: " + guardedEffectText (effect));
                    expectEquals (static_cast<int> (effect.nominalGuardedSamples), 0, tag + ": fixture: the 2^-10 reference run is unguarded");
                    expectWithinAbsoluteError (effect.actualDb, static_cast<double> (width), 0.3, tag + ": the side band above 4 kHz gains widthDb");
                    expectWithinAbsoluteError (effect.actualDb, effect.nominalDb, 0.02, tag + ": the guarded output carries the full nominal width");
                    expect (effect.guardedShare == 0.0 && effect.minGain == 1.0f, tag + ": stage 3 alone never engages the guard");
                }

                const auto pure = measureSide (sampleRate, makeParams (0.0f, 120.0f, 0.0f, 3.0f), 16000.0, 0.5f);
                logMessage ("  pure side 16 kHz, amplitude 0.5, width +3 dB: " + juce::String (pure.gainDb, 3) + " dB");
                expectWithinAbsoluteError (pure.gainDb, 3.0, 0.1, "a 0.5 pure side tone at 16 kHz gains the full +3 dB");
            }

            // T-032 R4-02（2026-09-29）：正常电平（守护地板之上）下低切与收单经守护后剩下的效果——以前的机制用例全在 −44 dB 地板之下，
            // 守护对某一级的系统性回收没有任何用例能发现。两个固定种子的夹具，按记录值 ± 余量钉死（回归护栏：守护或①② 的行为一变就报）：
            //  - 夹具 A「密音乐 + 15 Hz 次声」：两声道独立粉噪 + 15 Hz 正弦（M，0.25），峰值归一到 −12 dBFS，低切 25 Hz，量 M 的 20 Hz
            //    以下能量（4 阶 Butterworth 低通 20 Hz）相对干路的增益；
            //  - 夹具 B「中等宽低音」：M 与 S 各自独立的粉噪、S/M = −10 dB（RMS），峰值归一到 −12 dBFS，收单 120 Hz a = 1，量 S 的 100 Hz
            //    以下能量相对干路的增益。
            // 记录值（GCC Release；前任在 107 的 GCC 15.2 上记下，T-032 A8 在云端容器的 GCC 13.3 上改前改后复测，三位小数相同）：
            // A 标称 −17.765、实际 −16.604 dB（g < 1 约 3 %）；B 标称 −22.624、实际 −5.796 dB（g < 1 约 73 %）。
            // 改前（e6b445b）与改后相同：两个夹具加宽都为 0，T-032 不改①② 的路径（widthDb = 0 逐位不变，见 testOutputDigestsForGuardComparison
            // 与 T-032 报告的并排对照）。余量：标称 ± 0.05 dB（线性部分，只差平台间的舍入）、实际 ± 0.25 dB（守护的判定在平台间的舍入下
            // 只会在个别样本上翻转）。
            // 数字是**素材相关**的，换素材就要重新记录，不能写成通用比例（R4 与 V4 用各自的素材量到的收单实际 / 标称比例是 53 % 与 22 %，
            // 这里是 26 %）。夹具 B 说明同一机理（B 在局部峰值样本上零余量）对手动收单一样成立：收单的效果大半被守护回收——T-032 只把
            // ③ 拆出守护，② 要不要也拆出去待 Max 定（T-032 报告「待 Max」）；定了再改这里的记录值。
            void testLowCutAndMonoEffectsAtNormalLevel()
            {
                beginTest ("low cut and mono at normal level (T-032): the guarded effect on fixed-seed fixtures stays at its recorded value");

                constexpr double sampleRate = 48000.0;
                const auto numSamples = static_cast<int> (20.0 * sampleRate);

                struct Recorded { double nominalDb, actualDb; };
                constexpr Recorded recordedLowCut { -17.765, -16.604 };
                constexpr Recorded recordedMono   { -22.624, -5.796 };
                constexpr double nominalTolerance = 0.05, actualTolerance = 0.25;

                {
                    auto material = makePinkNoise (2, numSamples, -18.0f, 611);

                    for (auto& channel : material)
                        addSine (channel, sampleRate, 15.0, 0.25f);

                    material = normalisedToPeak (material, -12.0f);
                    const auto effect = measureGuardedEffect (sampleRate, material, makeParams (25.0f, 120.0f, 0.0f, 0.0f), false, 20.0, false);
                    logMessage ("  low cut 25 Hz on dense pink + 15 Hz infrasound, mid < 20 Hz: " + guardedEffectText (effect));
                    expectEquals (static_cast<int> (effect.nominalGuardedSamples), 0, "fixture A: the 2^-10 reference run is unguarded");
                    expectLessThan (effect.nominalDb, -12.0, "fixture A: the 25 Hz low cut nominally removes the 15 Hz infrasound by more than 12 dB");
                    expect (effect.guardedShare > 0.0, "fixture A: the guard acts on this material (the recorded value measures something)");
                    expectWithinAbsoluteError (effect.nominalDb, recordedLowCut.nominalDb, nominalTolerance, "fixture A: nominal attenuation at its recorded value");
                    expectWithinAbsoluteError (effect.actualDb, recordedLowCut.actualDb, actualTolerance, "fixture A: guarded attenuation at its recorded value");
                }

                {
                    const auto mid  = makePinkNoise (1, numSamples, -18.0f, 621);
                    const auto side = makePinkNoise (1, numSamples, -28.0f, 631);
                    auto material = makeSilence (2, numSamples);

                    for (int i = 0; i < numSamples; ++i)
                    {
                        const auto index = static_cast<size_t> (i);
                        material[0][index] = mid[0][index] + side[0][index];
                        material[1][index] = mid[0][index] - side[0][index];
                    }

                    material = normalisedToPeak (material, -12.0f);
                    const auto effect = measureGuardedEffect (sampleRate, material, makeParams (0.0f, 120.0f, 1.0f, 0.0f), true, 100.0, false);
                    logMessage ("  mono 120 Hz a = 1 on independent mid / side pink (S/M -10 dB), side < 100 Hz: " + guardedEffectText (effect));
                    expectEquals (static_cast<int> (effect.nominalGuardedSamples), 0, "fixture B: the 2^-10 reference run is unguarded");
                    expectLessThan (effect.nominalDb, -15.0, "fixture B: mono at 120 Hz nominally removes the side below 100 Hz by more than 15 dB");
                    expect (effect.guardedShare > 0.0, "fixture B: the guard acts on this material (the recorded value measures something)");
                    expectWithinAbsoluteError (effect.nominalDb, recordedMono.nominalDb, nominalTolerance, "fixture B: nominal attenuation at its recorded value");
                    expectWithinAbsoluteError (effect.actualDb, recordedMono.actualDb, actualTolerance, "fixture B: guarded attenuation at its recorded value");
                }
            }

            // T-032 验收门：widthDb = 0 的全部场景在改守护前后必须逐位相同。这里对一批固定种子的场景（怪值、随机块长、参数扫动、旁通来回、
            // 地板上下的电平、44.1 / 48 / 96 / 192 kHz、立体声与单声道）做输出摘要并打印（FNV-1a 64 位，含每次 process 的代价格、末尾的
            // 守护统计、isPassThrough 与延迟）；同一份测试源码分别对改前（e6b445b）与改后的 ClayModule 编译运行，两边的摘要记在 T-032 报告里
            // 逐行对照（另有 T-024 guard_compare 式的新旧并排逐样本对照，也见报告）。标「positive control」的两个场景带加宽（widthDb ≠ 0），
            // 是摘要机制本身的阳性对照：它们改后必须变。摘要依赖平台的 libm 与编译器（系数里的 cos / sin / pow），所以本用例自身只断言
            // 确定性：同一场景跑两遍摘要相同；对照只在同一台机器、同一套编译配置上做。
            void testOutputDigestsForGuardComparison()
            {
                beginTest ("output digests of widthDb = 0 scenarios (T-032 bit-exact comparison record; deterministic across runs)");

                struct Scenario
                {
                    juce::String name;
                    double sampleRate;
                    int channels;
                    Signal input;
                    ClayParams initial;
                    Schedule changes;
                };

                std::vector<Scenario> scenarios;

                const auto sweeps = [] (double sampleRate)
                {
                    const auto at = [sampleRate] (double seconds, int extra) { return static_cast<int> (seconds * sampleRate) + extra; };
                    return Schedule { { at (0.1, 0),  makeParams (25.0f, 120.0f, 0.0f, 0.0f) },
                                      { at (0.3, 7),  makeParams (25.0f, 120.0f, 1.0f, 0.0f) },
                                      { at (0.5, 0),  makeParams (0.0f, 40.0f, 1.0f, 0.0f) },
                                      { at (0.7, 13), makeParams (0.0f, 300.0f, 0.4f, 0.0f) },
                                      { at (0.9, 0),  makeParams (60.0f, 90.0f, 0.4f, 0.0f, true) },
                                      { at (1.1, 3),  makeParams (60.0f, 90.0f, 0.4f, 0.0f) },
                                      { at (1.3, 0),  neutralParams() },
                                      { at (1.5, 29), makeParams (10.0f, 40.0f, 1.0f, 0.0f) },
                                      { at (1.7, 0),  makeParams (60.0f, 300.0f, 1.0f, 0.0f, true) },
                                      { at (1.9, 5),  neutralParams() } };
                };

                {
                    const auto numSamples = static_cast<int> (1.0 * 48000.0);
                    auto specials = makePinkNoise (2, numSamples, -18.0f, 101);
                    sprinkleSpecialValues (specials);
                    auto movedCrossover = neutralParams();
                    movedCrossover.monoHz = 40.0f;
                    scenarios.push_back ({ "specials / neutral / stereo @ 48k", 48000.0, 2, specials, neutralParams(), { { numSamples / 2, movedCrossover } } });
                    scenarios.push_back ({ "specials / neutral / mono @ 48k", 48000.0, 1, Signal { specials[0] }, neutralParams(), { { numSamples / 2, movedCrossover } } });

                    auto loudSpecials = makeProgramme (numSamples, 48000.0, -12.0f, 502);
                    sprinkleSpecialValues (loudSpecials);
                    scenarios.push_back ({ "specials / low cut 60 + mono 300 / stereo @ 48k", 48000.0, 2, loudSpecials, makeParams (60.0f, 300.0f, 1.0f, 0.0f), {} });
                    scenarios.push_back ({ "specials / low cut 60 / mono @ 48k", 48000.0, 1, Signal { loudSpecials[0] }, makeParams (60.0f, 120.0f, 0.0f, 0.0f), {} });
                }

                {
                    const auto numSamples = static_cast<int> (2.0 * 48000.0);
                    const auto programme = makeProgramme (numSamples, 48000.0, -12.0f, 501);
                    scenarios.push_back ({ "programme -12 dBFS / low cut 60 + mono 300 / stereo @ 48k", 48000.0, 2, programme, makeParams (60.0f, 300.0f, 1.0f, 0.0f), {} });
                    scenarios.push_back ({ "programme -12 dBFS / low cut 60 / mono @ 48k", 48000.0, 1, Signal { programme[0] }, makeParams (60.0f, 120.0f, 0.0f, 0.0f), {} });
                    scenarios.push_back ({ "programme -12 dBFS / sweeps with width 0 / stereo @ 48k", 48000.0, 2, programme, neutralParams(), sweeps (48000.0) });
                    scenarios.push_back ({ "programme -12 dBFS / sweeps with width 0 / mono @ 48k", 48000.0, 1, Signal { programme[0] }, neutralParams(), sweeps (48000.0) });

                    const auto nearFloor = makeProgramme (numSamples, 48000.0, -46.0f, 503);
                    scenarios.push_back ({ "programme -46 dBFS (around the floor) / low cut 25 + mono 120 / stereo @ 48k", 48000.0, 2, nearFloor,
                                           makeParams (25.0f, 120.0f, 1.0f, 0.0f), {} });

                    scenarios.push_back ({ "positive control: programme -12 dBFS / all stages incl. width +2 dB / stereo @ 48k (must change with T-032)",
                                           48000.0, 2, programme, allStagesParams(), {} });
                    scenarios.push_back ({ "positive control: programme -12 dBFS / width +3 dB only / stereo @ 48k (must change with T-032)",
                                           48000.0, 2, programme, makeParams (0.0f, 120.0f, 0.0f, 3.0f), {} });
                }

                for (auto sampleRate : { 44100.0, 192000.0 })
                {
                    const auto numSamples = static_cast<int> (2.0 * sampleRate);
                    const auto programme = makeProgramme (numSamples, sampleRate, -12.0f, 504);
                    scenarios.push_back ({ "programme -12 dBFS / sweeps with width 0 / stereo @ " + juce::String (sampleRate), sampleRate, 2, programme,
                                           neutralParams(), sweeps (sampleRate) });
                }

                {
                    const auto numSamples = static_cast<int> (2.0 * 96000.0);
                    auto specials = makePinkNoise (2, numSamples, -18.0f, 505);
                    sprinkleSpecialValues (specials);
                    scenarios.push_back ({ "specials / sweeps with width 0 / stereo @ 96000", 96000.0, 2, specials, neutralParams(), sweeps (96000.0) });
                    scenarios.push_back ({ "specials / sweeps with width 0 / mono @ 96000", 96000.0, 1, Signal { specials[0] }, neutralParams(), sweeps (96000.0) });
                }

                juce::int64 seed = 7001;

                for (const auto& s : scenarios)
                {
                    const auto first  = digestOfRun (s.sampleRate, s.channels, s.input, s.initial, s.changes, seed);
                    const auto second = digestOfRun (s.sampleRate, s.channels, s.input, s.initial, s.changes, seed);
                    logMessage ("  digest " + first + "  " + s.name);
                    expectEquals (second, first, "deterministic: " + s.name);
                    ++seed;
                }
            }

            // 规格第 4 节「reset / NaN 恢复清滤波器状态」由链负责（链见 NaN 就整链 reset）；模块自己再保一道：IIR 影子滤波器吃到 NaN 后，
            // 状态在下一个 64 样本网格点清零；低通自己清洗输入（NaN 当 0），延迟线里的 NaN 过一次就走；都不会一直卡在 NaN。
            //  - 三级全关时输入里有一个 NaN：输出是逐位延迟 D 的输入（NaN 也原样延迟，交给链处理）；0.1 s 后打开三级，其后输出全部有限。
            //  - 三级全开时吃到 NaN：它在 D 个样本后到输出，之后最多到下一个网格点（≤ 64 样本）为止输出 NaN，再往后（留 128 样本余量）
            //    全部有限。
            void testNonFiniteInputDoesNotLatch()
            {
                beginTest ("a NaN in the input is delayed untouched while idle and never latches the shadow filters or the low-pass");

                constexpr double sampleRate = 48000.0;
                constexpr int nanAt = 1000;
                const auto numSamples = static_cast<int> (0.5 * sampleRate);
                const auto enableAt = static_cast<int> (0.1 * sampleRate);
                const auto delay = specDelay (sampleRate);
                auto input = makeProgramme (numSamples, sampleRate, -18.0f, 131);
                input[0][static_cast<size_t> (nanAt)] = std::numeric_limits<float>::quiet_NaN();

                ClayModule idle;
                prepareClay (idle, sampleRate, 2, neutralParams());
                const auto idleOut = runClay (idle, input, 333, { { enableAt, allStagesParams() } });
                expectEquals (countDifferentBits (delayedBy (input, delay), idleOut, 0, enableAt), 0,
                              "while idle everything, the NaN included, is delayed bit-exact");
                expect (allFinite (sliceOf (idleOut, enableAt, numSamples)), "after enabling the stages every output sample is finite");

                ClayModule active;
                prepareClay (active, sampleRate, 2, allStagesParams());
                const auto activeOut = runClay (active, input, 333);
                expect (allFinite (sliceOf (activeOut, nanAt + delay + 128, numSamples)), "an active module recovers from the NaN by the next grid point");
            }
        };

        static ClayModuleTests clayModuleTests;

        //==============================================================================
        // LinearPhaseLowpass 本身（头文件的公开语义；设计规格-Clay-根治修订 第 2 节「低通 / 实现要求」、第 4 节第 3 条）。
        // Clay 与分析器的「先试后用」收单候选共用它，所以单独验。
        class LinearPhaseLowpassTests  : public juce::UnitTest
        {
        public:
            LinearPhaseLowpassTests() : juce::UnitTest ("LinearPhaseLowpass", "Celadon") {}

            void runTest() override
            {
                testLengthsAndDelays();
                testImpulseResponse();
                testComplementaryResponse();
                testMatchesDirectConvolutionAcrossResyncGrid();
                testSilenceReturnsStateToExactZero();
                testWarmUpReproducesRunningState();
                testResetClampsAndInputIsSanitised();
                testDoesNotAllocate();
            }

        private:
            // 规格：L = max(2, round(0.24 · fs / fc))，群延迟 2(L − 1)；最长一档按 40 Hz：44.1 / 48 / 96 / 192 kHz 为 265 / 288 / 576 / 1152，
            // 延迟 528 / 574 / 1150 / 2302（Clay 的 D）。各分频点与本文件独立的 specLength 一致；最短 2；无效的采样率 / 截止频率给 2。
            void testLengthsAndDelays()
            {
                beginTest ("lengths and delays follow L = max(2, round(0.24 fs / fc)) and 2 (L - 1)");

                struct Case { double sampleRate; int maxLength; int delay; };
                const Case cases[] = { { 44100.0, 265, 528 }, { 48000.0, 288, 574 }, { 96000.0, 576, 1150 }, { 192000.0, 1152, 2302 } };

                for (const auto& c : cases)
                {
                    const auto tag = "@ " + juce::String (c.sampleRate);
                    expectEquals (LinearPhaseLowpass::maxLengthFor (c.sampleRate), c.maxLength, "longest length " + tag);
                    expectEquals (LinearPhaseLowpass::groupDelayFor (LinearPhaseLowpass::maxLengthFor (c.sampleRate)), c.delay, "longest delay " + tag);

                    for (auto cutoff : { 40.0, 60.0, 80.0, 120.0, 160.0, 300.0 })
                        expectEquals (LinearPhaseLowpass::lengthFor (c.sampleRate, cutoff), specLength (c.sampleRate, cutoff),
                                      "length for " + juce::String (cutoff) + " Hz " + tag);
                }

                expectEquals (LinearPhaseLowpass::lengthFor (48000.0, 300.0), 38, "48 kHz / 300 Hz");
                expectEquals (LinearPhaseLowpass::groupDelayFor (2), 2, "the shortest cascade delays by 2");
                expectEquals (LinearPhaseLowpass::lengthFor (48000.0, 20000.0), 2, "never shorter than 2");
                expectEquals (LinearPhaseLowpass::lengthFor (0.0, 80.0), 2, "an invalid sample rate gives the shortest length");
                expectEquals (LinearPhaseLowpass::lengthFor (48000.0, 0.0), 2, "an invalid cutoff gives the shortest length");
                expectEquals (LinearPhaseLowpass::lengthFor (48000.0, std::numeric_limits<double>::quiet_NaN()), 2, "a NaN cutoff gives the shortest length");
            }

            // 头文件：冲激响应 = 4 个长度 L 的矩形窗卷积（三次 B 样条形），长 4(L − 1) + 1、关于中点 2(L − 1) 对称，直流增益 1。
            //  - 与精确系数（exactKernel：整数卷积 / L⁴）逐点比 ≤ 1e-12（Python 原型同一算法实测 ≤ 1.4e-17）；
            //  - |h[c + k] − h[c − k]| ≤ 1e-12（原型 ≤ 3e-18）；Σh = 1 ± 1e-12（原型 ≤ 1.5e-14）；第一个输出就是 h[0] > 0（群延迟恰为 c）；
            //  - 支撑之外（4(L − 1) 之后）|y| ≤ 1e-12：滑动和加减不严格抵消，会留 1e-17 量级的残差，直到重算网格清掉（见静音用例）。
            //  L = 2（最短）、35（44.1 kHz / 300 Hz）、88、288、1152（最长，192 kHz / 40 Hz）；prepare 之后长度就是最大长度。
            void testImpulseResponse()
            {
                beginTest ("impulse response: the exact 4-box B-spline, symmetric about 2 (L - 1), DC gain 1");

                for (auto length : { 2, 35, 88, 288, 1152 })
                {
                    const auto tag = "L = " + juce::String (length);
                    LinearPhaseLowpass lowpass;
                    lowpass.prepare (length);
                    expectEquals (lowpass.getLength(), length, "prepare resets to the maximum length " + tag);

                    const auto support = 4 * (length - 1) + 1;
                    const auto centre = 2 * (length - 1);
                    std::vector<double> impulse (static_cast<size_t> (support + 64), 0.0);
                    impulse[0] = 1.0;
                    const auto response = runLowpass (lowpass, impulse);
                    const auto kernel = exactKernel (length);

                    double kernelError = 0.0, asymmetry = 0.0, dcGain = 0.0, tail = 0.0;

                    for (int n = 0; n < support; ++n)
                    {
                        kernelError = juce::jmax (kernelError, std::abs (response[static_cast<size_t> (n)] - kernel[static_cast<size_t> (n)]));
                        dcGain += response[static_cast<size_t> (n)];
                    }

                    for (int k = 1; k <= centre; ++k)
                        asymmetry = juce::jmax (asymmetry, std::abs (response[static_cast<size_t> (centre + k)] - response[static_cast<size_t> (centre - k)]));

                    for (int n = support; n < support + 64; ++n)
                        tail = juce::jmax (tail, std::abs (response[static_cast<size_t> (n)]));

                    expectEquals (static_cast<int> (kernel.size()), support, "reference kernel length " + tag);
                    expectLessOrEqual (kernelError, 1.0e-12, "matches the exact kernel " + tag);
                    expectLessOrEqual (asymmetry, 1.0e-12, "symmetric about 2 (L - 1) " + tag);
                    expectWithinAbsoluteError (dcGain, 1.0, 1.0e-12, "DC gain 1 " + tag);
                    expectLessOrEqual (tail, 1.0e-12, "nothing after 4 (L - 1) " + tag);
                    expectGreaterThan (response[0], 0.0, "the response starts with the input sample " + tag);
                }
            }

            // 头文件 / 规格：互补高通（1 − LP，对齐群延迟）在 fc/2、fc、2fc、4fc 处约 −20.8 / −9.9 / −1.9 / 0 dB，与采样率无关（±0.3 dB）。
            // 由实测冲激响应直接算频响（complementGainDb，与被测代码无关）。Python 原型在 44.1 ～ 192 kHz × 40 ～ 300 Hz 上偏差最大 0.22 dB
            //（L 取整让实际截止偏开最多半个样本）。
            void testComplementaryResponse()
            {
                beginTest ("complementary high-pass at fc/2, fc, 2fc, 4fc: -20.8 / -9.9 / -1.9 / 0 dB (+-0.3), any sample rate");

                const double multiples[] = { 0.5, 1.0, 2.0, 4.0 };
                const double expectedDb[] = { -20.8, -9.9, -1.9, 0.0 };

                for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
                {
                    for (auto cutoffHz : { 40.0, 80.0, 120.0, 160.0, 300.0 })
                    {
                        const auto length = LinearPhaseLowpass::lengthFor (sampleRate, cutoffHz);
                        LinearPhaseLowpass lowpass;
                        lowpass.prepare (length);

                        std::vector<double> impulse (static_cast<size_t> (4 * (length - 1) + 1), 0.0);
                        impulse[0] = 1.0;
                        const auto response = runLowpass (lowpass, impulse);
                        juce::String summary;

                        for (int m = 0; m < 4; ++m)
                        {
                            const auto gainDb = complementGainDb (response, 2 * (length - 1), multiples[m] * cutoffHz, sampleRate);
                            summary << juce::String (gainDb, 2) << " ";
                            expectWithinAbsoluteError (gainDb, expectedDb[m], 0.3,
                                                       "complement at " + juce::String (multiples[m]) + " fc, " + juce::String (cutoffHz)
                                                           + " Hz @ " + juce::String (sampleRate));
                        }

                        logMessage ("  complement " + juce::String (cutoffHz) + " Hz @ " + juce::String (sampleRate) + " (L " + juce::String (length)
                                    + "): " + summary + "dB");
                    }
                }
            }

            // 头文件：「从 reset 起每 65536 样本按历史精确重算滑动和」。跨过两个网格点（65536、131072）的 2 × 65536 + 4000 个样本，
            // 输入 = 0.5 + 均匀噪声 ±1（带直流，让滑动和的量级与舍入都更大），与朴素卷积（精确系数）比：开头、两个网格点前后各
            // ±1500 个样本的窗里误差 ≤ 1e-12（原型实测 ≤ 2.3e-14）。阳性对照：同样的输出对长度 L + 1 的精确系数差 > 1e-6——这套比较
            // 分得出长度差一。
            void testMatchesDirectConvolutionAcrossResyncGrid()
            {
                beginTest ("running sums match direct convolution before and after the resync grid points (<= 1e-12)");

                constexpr int interval = LinearPhaseLowpass::resyncIntervalSamples;
                const auto numSamples = 2 * interval + 4000;

                struct Case { double sampleRate; double cutoffHz; };
                const Case cases[] = { { 48000.0, 40.0 }, { 192000.0, 40.0 }, { 44100.0, 300.0 } };

                for (const auto& c : cases)
                {
                    const auto length = specLength (c.sampleRate, c.cutoffHz);
                    const auto tag = "L = " + juce::String (length);
                    LinearPhaseLowpass lowpass;
                    lowpass.prepare (length);

                    const auto input = makeUniformNoise (numSamples, 0.5, 211 + length);
                    const auto output = runLowpass (lowpass, input);
                    const auto kernel = exactKernel (length);
                    const auto wrongKernel = exactKernel (length + 1);

                    double worst = 0.0, wrong = 0.0;
                    const std::pair<int, int> windows[] = { { 0, 3000 }, { interval - 1500, interval + 1500 }, { 2 * interval - 1500, 2 * interval + 1500 } };

                    for (const auto& window : windows)
                    {
                        for (int n = window.first; n < window.second; ++n)
                        {
                            const auto y = output[static_cast<size_t> (n)];
                            worst = juce::jmax (worst, std::abs (y - convolveAt (input, kernel, n)));
                            wrong = juce::jmax (wrong, std::abs (y - convolveAt (input, wrongKernel, n)));
                        }
                    }

                    logMessage ("  " + tag + ": worst error vs direct convolution " + juce::String (worst, 3) + ", vs the L + 1 kernel " + juce::String (wrong, 6));
                    expectLessOrEqual (worst, 1.0e-12, "matches direct convolution across the resync grid " + tag);
                    expectGreaterThan (wrong, 1.0e-6, "positive control: a kernel one sample longer is told apart " + tag);
                }
            }

            // 重算网格的另一半作用：滑动和在信号离开后常留 1e-16 量级的残差（加减不严格抵消），不重算就会一直留着（输出永远不是精确的 0）。
            // 第 1 级在「信号离开 4L 个样本之后的第一个网格点」按全 0 的历史重算、精确归零，此后它的输出恰为 0；下一级的历史在 L 个样本内
            // 被 0 填满、下一个网格点归零……4 级在 3 个网格周期后全部精确为 0。输入 = 均匀噪声 0.25 ± 1 共 70000 个样本（在第 1、2 个
            // 网格点之间结束）后全 0，跑到 5 × 65536 + 2000：
            //  - 阳性对照：信号离开之后、归零点之前有非 0 输出（残差确实存在，所以后面的「精确 0」是重算带来的）；
            //  - 归零点（这里 = 131072 + 3 × 65536 = 327680）起输出全部恰为 0。Python 原型（同一算法）：最后一个非 0 输出正是 327679。
            void testSilenceReturnsStateToExactZero()
            {
                beginTest ("after the input goes silent, the resync grid brings the state back to exactly zero");

                constexpr int interval = LinearPhaseLowpass::resyncIntervalSamples;
                constexpr int signalEnd = 70000;
                const auto numSamples = 5 * interval + 2000;

                for (auto length : { 288, 38 })
                {
                    const auto tag = "L = " + juce::String (length);
                    LinearPhaseLowpass lowpass;
                    lowpass.prepare (length);

                    auto input = makeUniformNoise (numSamples, 0.25, 307 + length);
                    std::fill (input.begin() + signalEnd, input.end(), 0.0);
                    const auto output = runLowpass (lowpass, input);

                    const auto firstCleanGrid = ((signalEnd + 4 * length + interval - 1) / interval) * interval;
                    const auto zeroFrom = firstCleanGrid + 3 * interval;

                    int residues = 0, nonZeroAfter = 0;

                    for (int n = signalEnd + 4 * length; n < zeroFrom; ++n)
                        residues += output[static_cast<size_t> (n)] != 0.0 ? 1 : 0;

                    for (int n = zeroFrom; n < numSamples; ++n)
                        nonZeroAfter += output[static_cast<size_t> (n)] != 0.0 ? 1 : 0;

                    logMessage ("  " + tag + ": " + juce::String (residues) + " non-zero residue samples before " + juce::String (zeroFrom)
                                + ", " + juce::String (nonZeroAfter) + " after");
                    expectGreaterThan (residues, 0, "positive control: a rounding residue survives until the grid clears it " + tag);
                    expectEquals (nonZeroAfter, 0, "exactly zero from " + juce::String (zeroFrom) + " on " + tag);
                }
            }

            // Clay 切换分频点的做法（ClayModule.h「用 S 的历史把第二套低通重建成新 L（预热 4(L − 1) 个样本）」）：一套低通 reset 到长度 L
            // 后喂最近 4(L − 1) 个输入，接下来的输出就与一直在跑的同长度低通一样（只差舍入，≤ 1e-12）。被预热的那套事先用别的长度
            // 跑过（历史环里有旧数据），也验了 reset 把要用的格清干净。阳性对照：少喂最老的那一个，第一个输出就差 h[4(L − 1)]·x = x / L⁴
            //（这里把那个输入设成 1，差 1 / L⁴ ≥ 1.4e-10，远大于舍入）——4(L − 1) 这个预热长度一个都不能少。
            void testWarmUpReproducesRunningState()
            {
                beginTest ("reset + the last 4 (L - 1) inputs reproduce a running low-pass of the same length (<= 1e-12)");

                constexpr int longest = 288;
                constexpr int switchAt = 12345;

                for (auto length : { 96, 288, 38 })
                {
                    const auto tag = "L = " + juce::String (length);
                    const auto warmUp = 4 * (length - 1);
                    auto input = makeUniformNoise (30000, 0.1, 401 + length);
                    input[static_cast<size_t> (switchAt - warmUp)] = 1.0;

                    LinearPhaseLowpass running, rebuilt, shortWarm;

                    for (auto* lowpass : { &running, &rebuilt, &shortWarm })
                        lowpass->prepare (longest);

                    running.reset (length);
                    const auto otherLength = length == longest ? 50 : longest;
                    rebuilt.reset (otherLength);
                    shortWarm.reset (otherLength);

                    for (int n = 0; n < switchAt; ++n)
                    {
                        const auto x = input[static_cast<size_t> (n)];
                        running.process (x);
                        rebuilt.process (-0.5 * x);
                        shortWarm.process (0.7 * x);
                    }

                    rebuilt.reset (length);
                    shortWarm.reset (length);

                    for (int n = switchAt - warmUp; n < switchAt; ++n)
                        rebuilt.process (input[static_cast<size_t> (n)]);

                    for (int n = switchAt - warmUp + 1; n < switchAt; ++n)
                        shortWarm.process (input[static_cast<size_t> (n)]);

                    double worst = 0.0, shortDeficit = 0.0;

                    for (int n = switchAt; n < static_cast<int> (input.size()); ++n)
                    {
                        const auto x = input[static_cast<size_t> (n)];
                        const auto expected = running.process (x);
                        worst = juce::jmax (worst, std::abs (rebuilt.process (x) - expected));
                        const auto shortOutput = shortWarm.process (x);

                        if (n == switchAt)
                            shortDeficit = std::abs (shortOutput - expected);
                    }

                    const auto lastTap = 1.0 / std::pow (static_cast<double> (length), 4.0);
                    logMessage ("  " + tag + ": rebuilt vs running " + juce::String (worst, 3) + ", one sample short "
                                + juce::String (shortDeficit, 3) + " (last tap " + juce::String (lastTap, 3) + ")");
                    expectLessOrEqual (worst, 1.0e-12, "a rebuilt low-pass matches the running one " + tag);
                    expectGreaterThan (shortDeficit, 0.5 * lastTap, "positive control: one warm-up sample short is visible " + tag);
                }
            }

            // 头文件：reset「钳到 [2, prepare 的 maxLength]」；实现（头文件私有部分）：输入清洗——非有限值当 0、|x| > 1e4 钳到 ±1e4，
            // 状态永远有限、不卡住。
            //  - reset (1) → 2；reset (maxLength + 100) → maxLength；prepare (1) → 最长 2；没 prepare 的低通 process 返回 0。
            //  - 输入里的 NaN / +Inf / −Inf / 1e30 / −1e30 与分别喂 0 / +1e4 / −1e4 / +1e4 / −1e4 的低通逐位相同，输出全部有限；
            //    阳性对照：这些样本确实进了滤波器（输出与完全不含它们的同一段噪声不同）。
            void testResetClampsAndInputIsSanitised()
            {
                beginTest ("reset clamps the length; NaN / Inf / huge inputs are sanitised and never latch");

                LinearPhaseLowpass unprepared;
                expectEquals (unprepared.process (0.5), 0.0, "an unprepared low-pass outputs 0");

                LinearPhaseLowpass lowpass;
                lowpass.prepare (100);
                lowpass.reset (1);
                expectEquals (lowpass.getLength(), 2, "reset clamps to at least 2");
                lowpass.reset (200);
                expectEquals (lowpass.getLength(), 100, "reset clamps to the prepared maximum");

                LinearPhaseLowpass tiny;
                tiny.prepare (1);
                expectEquals (tiny.getLength(), 2, "prepare never goes below 2");

                using Limits = std::numeric_limits<double>;
                const double specials[]     = { Limits::quiet_NaN(), Limits::infinity(), -Limits::infinity(), 1.0e30, -1.0e30 };
                const double replacements[] = { 0.0, 1.0e4, -1.0e4, 1.0e4, -1.0e4 };

                const auto clean = makeUniformNoise (6000, 0.0, 503);
                auto dirty = clean, substituted = clean;

                for (size_t i = 0; i < 5; ++i)
                {
                    const auto at = 1000 + 700 * i;
                    dirty[at] = specials[i];
                    substituted[at] = replacements[i];
                }

                LinearPhaseLowpass a, b, c;

                for (auto* each : { &a, &b, &c })
                    each->prepare (96);

                const auto dirtyOut = runLowpass (a, dirty);
                const auto substitutedOut = runLowpass (b, substituted);
                const auto cleanOut = runLowpass (c, clean);

                bool finite = true;

                for (auto value : dirtyOut)
                    finite = finite && std::isfinite (value);

                expect (finite, "the output stays finite whatever comes in");
                expectEquals (countDifferentDoubles (dirtyOut, substitutedOut), 0, "NaN -> 0, Inf and huge values -> +-1e4, bit for bit");
                expect (countDifferentDoubles (dirtyOut, cleanOut) > 0, "positive control: the special samples do enter the filter");
            }

            // 头文件：「prepare 按最大长度分配；reset / process 不分配、不加锁」。192 kHz 的最长一档，长度来回换、跨过重算网格点。
            void testDoesNotAllocate()
            {
                beginTest ("reset / process never allocate");

                LinearPhaseLowpass lowpass;
                lowpass.prepare (1152);
                double sink = 0.0;

                const auto before = allocationCountOnThisThread();

                for (int pass = 0; pass < 8; ++pass)
                {
                    lowpass.reset (2 + 150 * pass);

                    for (int n = 0; n < 70000; ++n)
                        sink += lowpass.process (static_cast<double> ((n % 97) - 48) * 0.01);
                }

                const auto allocations = allocationCountOnThisThread() - before;

                expect (std::isfinite (sink), "processed something finite");
                expectEquals (static_cast<juce::int64> (allocations), static_cast<juce::int64> (0), "reset / process never allocate");
            }
        };

        static LinearPhaseLowpassTests linearPhaseLowpassTests;
    }
}
