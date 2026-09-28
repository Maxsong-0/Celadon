/*
  ==============================================================================

    GlazeBehaviourReport.cpp
    GLAZE 行为记录（v0.6，ADR-008；设计规格-GLAZE-界面与实现第 5 节第 6 条「各风格在几种合成素材上的行为记录」）：
    只记录、不设门槛，给 Max 试听与标定风格常数（L_style、各模块上限）作参考。

    四种可复现的合成素材（48 kHz 立体声、块 512）：
      ① 类音乐：底鼓 + 军鼓噪声包络 + 贝斯正弦 + 三和弦限带锯齿经一阶低通，120 BPM、Am–F–G–Am–Am 循环 20 s，
         用插件自己的 LoudnessMeter 归一到 −18 LUFS，峰均比接近成品混音；
      ② 粉噪 −18 dBFS RMS（Kellet 七极点近似，两声道不相关）；
      ③ 正弦混合 100 Hz + 1 kHz + 5 kHz，各 −18 dBFS；
      ④ 同一段类音乐推到真峰值 −0.3 dBTP（约 −13 LUFS 的热混音，限幅器真正介入）。

    每种素材：先在 GLAZE 0 下按出厂常数学一份画像（带播放位置、素材不循环），存档；再对 自动 / 温润 / 冲击 / 通透 × GLAZE 50 / 100
    从同一份存档起步、离线渲染前 10 s（算法5起，离线与实时均运行相同的学习、校准与安全反馈），
    记录：输出整体响度与方案目标（effectiveTarget，二者之差含尚未收敛的校准与动态保护）、真峰值、PLR（真峰值 − 整体响度）、
    Limit GR 最大 / 平均（每块最大值的均值）、Glue GR 最大、方案的七项增量、是否受约束。另记 GLAZE 0 的一行作对照。

    只断言输出全有限、整链真峰值 ≤ Ceiling + 0.01 dB（ADR-004 口径）；其余一律只记录。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/GlazePlanner.h"
#include "../Source/dsp/GlazeProfile.h"
#include "../Source/dsp/LoudnessMeter.h"
#include "../Source/ui/HelpText.h"   // celadon::utf8：中文字面量必须经它转换

namespace celadon::test
{
    namespace
    {
        constexpr double sampleRate = 48000.0;
        constexpr int numChannels = 2;
        constexpr int blockSize = 512;
        constexpr double materialSeconds = 20.0;
        constexpr int materialSamples = (int) (materialSeconds * sampleRate);
        constexpr double renderSeconds = 10.0;
        constexpr int renderSamples = (int) (renderSeconds * sampleRate);

        constexpr float ceilingDb = -1.0f;
        constexpr float targetLufs = -18.0f;

        juce::dsp::ProcessSpec makeSpec()
        {
            return { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
        }

        CeladonChain::Params makeParams (float glazePercent, int style)
        {
            CeladonChain::Params p;
            p.limit.ceilingDb = ceilingDb;
            p.limit.autoRelease = true;
            p.limit.lookaheadMs = 2.0f;
            p.glazePercent = glazePercent;
            p.glazeStyle = style;
            return p;
        }

        //==============================================================================
        struct Measurement
        {
            float integratedLufs = LoudnessMeter::noReading;
            float truePeakDb = -200.0f;
            float limitGrMaxDb = 0.0f;
            float limitGrMeanDb = 0.0f;
            float glueGrMaxDb = 0.0f;
            bool finite = true;

            float plrDb() const noexcept   { return truePeakDb - integratedLufs; }
        };

        Measurement measureSignal (const Signal& signal)
        {
            Measurement m;
            LoudnessMeter meter;
            meter.prepare (makeSpec());

            const auto numSamples = (int) signal.front().size();
            juce::AudioBuffer<float> block (numChannels, blockSize);

            for (int start = 0; start < numSamples; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, signal[(size_t) ch].data() + start, length);

                meter.process (block);
            }

            m.integratedLufs = meter.getIntegratedLufs();
            m.truePeakDb = toDb (truePeakOf (signal));
            m.finite = allFinite (signal);
            return m;
        }

        // 在 GLAZE 0 下按出厂常数学一份画像并存档。素材 20 s 不够时接着从头再放（播放位置照样往后走：对分析器是新的一段），
        // 最多 40 s——主要特征置信度不够时聆听要到 30 s 上限才结束（时间线第 2 节）。
        std::unique_ptr<juce::XmlElement> learn (const Signal& material)
        {
            CeladonChain chain;
            const auto p = makeParams (0.0f, 0);
            chain.setParams (p, false);
            chain.prepare (makeSpec());

            glaze::HostInfo host;
            host.hasPlayState = host.isPlaying = host.hasPosition = true;
            juce::AudioBuffer<float> block (numChannels, blockSize);

            const auto maxSamples = 2 * materialSamples;

            for (int position = 0; position + blockSize <= maxSamples; position += blockSize)
            {
                const auto start = position % (materialSamples - materialSamples % blockSize);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, material[(size_t) ch].data() + start, blockSize);

                host.positionSamples = position;
                chain.setHostInfo (host);
                chain.setParams (p, false);
                chain.process (block);

                if (chain.getGlazeStatus().profileReady)
                    break;
            }

            return chain.createGlazeStateXml();
        }

        struct Case
        {
            Measurement output;
            glaze::Plan plan;
        };

        // 从存档起步、离线渲染素材的前 renderSeconds（末尾补一段延迟长度的静音让最后一块出来）。
        Case render (const juce::XmlElement& state, const Signal& material, float glazePercent, int style)
        {
            CeladonChain chain;
            const auto p = makeParams (glazePercent, style);
            chain.restoreGlazeStateXml (&state);
            chain.setParams (p, false);
            chain.prepare (makeSpec());

            glaze::HostInfo host;
            host.offline = true;
            host.hasPlayState = host.isPlaying = true;

            const auto latency = chain.getLatencySamples();
            auto output = makeSilence (numChannels, renderSamples + latency);
            juce::AudioBuffer<float> block (numChannels, blockSize);
            Case result;
            double limitGrSum = 0.0;
            int blocks = 0;

            for (int start = 0; start < renderSamples + latency; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, renderSamples + latency - start);
                block.setSize (numChannels, length, false, false, true);
                block.clear();

                for (int ch = 0; ch < numChannels; ++ch)
                    if (start < renderSamples)
                        block.copyFrom (ch, 0, material[(size_t) ch].data() + start, juce::jmin (length, renderSamples - start));

                chain.setHostInfo (host);
                chain.setParams (p, false);
                chain.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, output[(size_t) ch].begin() + start);

                const auto limitGr = chain.getAndResetLimitGainReductionDb();
                result.output.limitGrMaxDb = juce::jmax (result.output.limitGrMaxDb, limitGr);
                result.output.glueGrMaxDb = juce::jmax (result.output.glueGrMaxDb, chain.getAndResetGlueGainReductionDb());
                limitGrSum += limitGr;
                ++blocks;
            }

            const auto measured = measureSignal (output);
            result.output.integratedLufs = measured.integratedLufs;
            result.output.truePeakDb = measured.truePeakDb;
            result.output.finite = measured.finite;
            result.output.limitGrMeanDb = blocks > 0 ? (float) (limitGrSum / blocks) : 0.0f;
            result.plan = chain.getGlazeController().getProbeForTesting().targetPlan;
            return result;
        }

        //==============================================================================
        // ① 类音乐：120 BPM、4/4、10 小节 = 20 s（Am – F – G – Am – Am 两遍）；底鼓在 1、3 拍，军鼓在 2、4 拍；贝斯八分音符跟根音。
        constexpr double bpm = 120.0;
        constexpr double beatSeconds = 60.0 / bpm;
        constexpr int beatsPerBar = 4;
        constexpr int numBars = 10;
        constexpr juce::int64 musicSeed = 20260925;

        struct Chord
        {
            double bassHz;
            double notesHz[3];
        };

        constexpr Chord progression[5] =
        {
            { 55.00, { 220.00, 261.63, 329.63 } },
            { 43.65, { 174.61, 220.00, 261.63 } },
            { 49.00, { 196.00, 246.94, 293.66 } },
            { 55.00, { 220.00, 261.63, 329.63 } },
            { 55.00, { 220.00, 261.63, 329.63 } },
        };

        const Chord& chordOfBar (int bar)   { return progression[(size_t) (bar % 5)]; }

        void addKick (Signal& signal, int startSample)
        {
            const auto n = juce::jmin ((int) (0.35 * sampleRate), materialSamples - startSample);
            double phase = 0.0;

            for (int i = 0; i < n; ++i)
            {
                const auto t = i / sampleRate;
                phase += juce::MathConstants<double>::twoPi * (48.0 + 117.0 * std::exp (-t / 0.025)) / sampleRate;
                const auto value = (float) (std::exp (-t / 0.10) * juce::jmin (1.0, t / 0.002) * std::sin (phase));

                for (auto& channel : signal)
                    channel[(size_t) (startSample + i)] += value;
            }
        }

        void addSnare (Signal& signal, int startSample, int hitIndex)
        {
            const auto n = juce::jmin ((int) (0.30 * sampleRate), materialSamples - startSample);
            const auto lowpassCoeff = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 6000.0 / sampleRate);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                juce::Random random (musicSeed + (juce::int64) hitIndex * 31 + (juce::int64) ch * 7919);
                double lowpassState = 0.0;
                auto& channel = signal[(size_t) ch];

                for (int i = 0; i < n; ++i)
                {
                    const auto t = i / sampleRate;
                    lowpassState += lowpassCoeff * ((random.nextDouble() * 2.0 - 1.0) - lowpassState);
                    const auto noise = 0.6 * lowpassState * std::exp (-t / 0.07);
                    const auto body = 0.5 * std::sin (juce::MathConstants<double>::twoPi * 185.0 * t) * std::exp (-t / 0.04);
                    channel[(size_t) (startSample + i)] += (float) ((noise + body) * juce::jmin (1.0, t / 0.001));
                }
            }
        }

        void addBass (Signal& signal)
        {
            constexpr int eighthsPerBar = beatsPerBar * 2;
            constexpr double noteSeconds = 0.22;
            const auto eighthSamples = (int) (beatSeconds * 0.5 * sampleRate);

            for (int bar = 0; bar < numBars; ++bar)
             for (int eighth = 0; eighth < eighthsPerBar; ++eighth)
            {
                const auto start = (bar * eighthsPerBar + eighth) * eighthSamples;
                const auto frequency = chordOfBar (bar).bassHz * (eighth % 4 == 3 ? 2.0 : 1.0);
                const auto n = juce::jmin ((int) (noteSeconds * sampleRate), materialSamples - start);

                for (int i = 0; i < n; ++i)
                {
                    const auto t = i / sampleRate;
                    const auto envelope = juce::jmin (1.0, t / 0.003) * std::exp (-t / 0.35) * juce::jmin (1.0, (noteSeconds - t) / 0.010);
                    const auto value = (float) (0.45 * envelope * std::sin (juce::MathConstants<double>::twoPi * frequency * t));

                    for (auto& channel : signal)
                        channel[(size_t) (start + i)] += value;
                }
            }
        }

        void addChords (Signal& signal)
        {
            constexpr double highestHarmonicHz = 7000.0;
            constexpr double barSeconds = beatSeconds * beatsPerBar;
            const auto barSamples = (int) (barSeconds * sampleRate);
            std::vector<double> bus ((size_t) materialSamples, 0.0);

            for (int bar = 0; bar < numBars; ++bar)
            {
                const auto start = bar * barSamples;
                const auto n = juce::jmin (barSamples, materialSamples - start);

                for (auto noteHz : chordOfBar (bar).notesHz)
                {
                    const auto numHarmonics = (int) (highestHarmonicHz / noteHz);

                    for (int i = 0; i < n; ++i)
                    {
                        const auto t = i / sampleRate;
                        double saw = 0.0;

                        for (int k = 1; k <= numHarmonics; ++k)
                            saw += (k % 2 == 1 ? 1.0 : -1.0) * std::sin (juce::MathConstants<double>::twoPi * k * noteHz * t) / k;

                        const auto envelope = juce::jmin (1.0, t / 0.04) * juce::jmin (1.0, (barSeconds - t) / 0.06);
                        bus[(size_t) (start + i)] += 0.18 * envelope * saw * 2.0 / juce::MathConstants<double>::pi;
                    }
                }
            }

            const auto lowpassCoeff = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 1200.0 / sampleRate);
            double lowpassState = 0.0;

            for (int i = 0; i < materialSamples; ++i)
            {
                lowpassState += lowpassCoeff * (bus[(size_t) i] - lowpassState);

                for (auto& channel : signal)
                    channel[(size_t) i] += (float) lowpassState;
            }
        }

        Signal makeMusicLike()
        {
            auto signal = makeSilence (numChannels, materialSamples);
            const auto beatSamples = (int) (beatSeconds * sampleRate);

            for (int beat = 0; beat < numBars * beatsPerBar; ++beat)
            {
                if (beat % 2 == 0)
                    addKick (signal, beat * beatSamples);
                else
                    addSnare (signal, beat * beatSamples, beat);
            }

            addBass (signal);
            addChords (signal);

            scale (signal, juce::Decibels::decibelsToGain (targetLufs - measureSignal (signal).integratedLufs));
            return signal;
        }

        Signal pushedToTruePeak (const Signal& source, float truePeakDb)
        {
            auto signal = source;
            scale (signal, juce::Decibels::decibelsToGain (truePeakDb - toDb (truePeakOf (signal))));
            return signal;
        }

        // ② 粉噪：Paul Kellet 的 7 极点近似；两声道不同种子；空跑 8192 样本进稳态，再归一到指定 RMS。
        Signal makePinkNoise (float rmsDbfs, juce::int64 seed)
        {
            constexpr int warmUp = 8192;
            auto signal = makeSilence (numChannels, materialSamples);
            const auto targetRms = (double) juce::Decibels::decibelsToGain (rmsDbfs);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                juce::Random random (seed + (juce::int64) ch * 7919);
                double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;
                auto& channel = signal[(size_t) ch];
                double sumOfSquares = 0.0;

                for (int i = -warmUp; i < materialSamples; ++i)
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

                const auto gain = (float) (targetRms / std::sqrt (sumOfSquares / (double) materialSamples));

                for (auto& sample : channel)
                    sample *= gain;
            }

            return signal;
        }

        // ③ 正弦混合：100 Hz + 1 kHz + 5 kHz，各 −18 dBFS，两声道相同。
        Signal makeSineMix()
        {
            const auto amplitude = juce::Decibels::decibelsToGain (-18.0f);
            auto signal = makeSilence (numChannels, materialSamples);

            for (auto frequency : { 100.0, 1000.0, 5000.0 })
            {
                const auto tone = makeSine (numChannels, sampleRate, frequency, amplitude, materialSeconds);

                for (size_t ch = 0; ch < signal.size(); ++ch)
                    for (size_t i = 0; i < signal[ch].size(); ++i)
                        signal[ch][i] += tone[ch][i];
            }

            return signal;
        }

        //==============================================================================
        // 对齐的表：终端里中日韩字符占两格，按显示宽度补空格。
        int displayWidth (const juce::String& text)
        {
            int width = 0;

            for (auto t = text.getCharPointer(); ! t.isEmpty();)
                width += static_cast<juce::uint32> (t.getAndAdvance()) >= 0x2e80u ? 2 : 1;

            return width;
        }

        juce::String padLeft (const juce::String& text, int width)
        {
            return juce::String::repeatedString (" ", juce::jmax (0, width - displayWidth (text))) + text;
        }

        struct Table
        {
            std::vector<std::vector<juce::String>> rows;

            void add (std::vector<juce::String> row)   { rows.push_back (std::move (row)); }

            std::vector<juce::String> render() const
            {
                std::vector<int> widths (rows.front().size(), 0);

                for (const auto& row : rows)
                    for (size_t c = 0; c < widths.size(); ++c)
                        widths[c] = juce::jmax (widths[c], displayWidth (row[c]));

                std::vector<juce::String> lines;

                for (size_t r = 0; r < rows.size(); ++r)
                {
                    juce::String line ("    ");

                    for (size_t c = 0; c < widths.size(); ++c)
                        line += (c == 0 ? "" : "  ") + padLeft (rows[r][c], widths[c]);

                    lines.push_back (line);

                    if (r == 0)
                        lines.push_back ("    " + juce::String::repeatedString ("-", displayWidth (line) - 4));
                }

                return lines;
            }
        };

        juce::String lufsText (float lufs)
        {
            return lufs <= LoudnessMeter::noReading || ! std::isfinite (lufs) ? utf8 ("—") : juce::String (lufs, 1);
        }

        juce::String signedText (float value, int decimals)
        {
            const auto unit = std::pow (10.0f, (float) decimals);
            const auto shown = std::round (value * unit) / unit;

            if (shown == 0.0f)
                return juce::String (0.0f, decimals);

            return (shown > 0.0f ? "+" : "") + juce::String (shown, decimals);
        }

        juce::String styleName (int style)
        {
            switch (style)
            {
                case 1:  return utf8 ("温润");
                case 2:  return utf8 ("冲击");
                case 3:  return utf8 ("通透");
                default: return utf8 ("自动");
            }
        }
    }

    class GlazeBehaviourReport  : public juce::UnitTest
    {
    public:
        GlazeBehaviourReport() : juce::UnitTest ("GlazeBehaviourReport", "Celadon") {}

        void runTest() override
        {
            beginTest ("per style on music-like / pink noise / sine mix / hot music: loudness, target, true peak, PLR, GR (record only)");

            const auto music = makeMusicLike();
            reportMaterial (utf8 ("类音乐（底鼓 + 军鼓 + 贝斯 + 三和弦，归一到 −18 LUFS）"), music);
            reportMaterial (utf8 ("粉噪（−18 dBFS RMS，两声道不相关）"), makePinkNoise (-18.0f, 20260925));
            reportMaterial (utf8 ("正弦混合（100 Hz + 1 kHz + 5 kHz，各 −18 dBFS）"), makeSineMix());
            reportMaterial (utf8 ("类音乐推热（真峰值 −0.3 dBTP）"), pushedToTruePeak (music, -0.3f));
        }

    private:
        void reportMaterial (const juce::String& materialName, const Signal& material)
        {
            Signal head;
            for (const auto& channel : material)
                head.emplace_back (channel.begin(), channel.begin() + renderSamples);

            const auto input = measureSignal (head);
            const auto state = learn (material);
            glaze::CommittedState committed;
            const auto learned = state != nullptr && glaze::fromXml (*state, committed) && committed.hasProfile;
            expect (learned, "learning must complete on " + materialName);

            logMessage (utf8 ("  素材：") + materialName);
            logMessage (utf8 ("    输入（前 10 s）：整体 ") + lufsText (input.integratedLufs) + utf8 (" LUFS，真峰值 ")
                        + juce::String (input.truePeakDb, 2) + utf8 (" dBTP，PLR ") + juce::String (input.plrDb(), 1) + " dB");

            if (! learned)
                return;

            const auto& p = committed.profile;
            logMessage (utf8 ("    画像：I ") + juce::String (p.integratedLufs.value, 2) + utf8 ("（置信 ") + juce::String (p.integratedLufs.confidence, 2)
                        + utf8 ("）LRA ") + juce::String (p.loudnessRangeLu.value, 1) + utf8 (" PLR ") + juce::String (p.plrDb.value, 1)
                        + utf8 (" 斜率 ") + juce::String (p.spectralSlopeDbPerOct.value, 2) + utf8 (" dB/oct 起音 ")
                        + juce::String (p.onsetDensityPerSecond.value, 2) + utf8 (" 次/s；自动权重 温润 ") + juce::String (committed.lockedWeights.warm, 2)
                        + utf8 (" 冲击 ") + juce::String (committed.lockedWeights.punchy, 2) + utf8 (" 通透 ") + juce::String (committed.lockedWeights.clean, 2));

            Table table;
            table.add ({ utf8 ("风格"), "GLAZE %", utf8 ("输出 LUFS"), utf8 ("目标 LUFS"), utf8 ("前馈误差 LU"), utf8 ("较输入 LU"),
                         utf8 ("真峰值 dBTP"), "PLR dB", utf8 ("Limit GR 最大 / 平均"), utf8 ("Glue GR 最大"),
                         "Recover/Tame", "Tone dB", "Glue/Heat", "Gain dB", "Clip", utf8 ("受约束") });

            auto addRow = [&] (int style, float glaze)
            {
                const auto caseName = materialName + " " + styleName (style) + " GLAZE " + juce::String ((int) glaze);
                const auto r = render (*state, material, glaze, style);

                expect (r.output.finite, "non-finite output: " + caseName);
                const auto overshootDb = r.output.truePeakDb - ceilingDb;
                expect (overshootDb <= 0.01f, "overshoot " + juce::String (overshootDb, 4) + " dB: " + caseName);

                const auto& inc = r.plan.increments;
                const auto target = glaze > 0.0f ? r.plan.effectiveTargetLufs : r.output.integratedLufs;

                table.add ({ glaze > 0.0f ? styleName (style) : utf8 ("—"), juce::String ((int) glaze),
                             lufsText (r.output.integratedLufs), glaze > 0.0f ? lufsText (target) : utf8 ("—"),
                             glaze > 0.0f ? signedText (r.output.integratedLufs - target, 2) : utf8 ("—"),
                             signedText (r.output.integratedLufs - input.integratedLufs, 1),
                             juce::String (r.output.truePeakDb, 2), juce::String (r.output.plrDb(), 1),
                             juce::String (r.output.limitGrMaxDb, 2) + " / " + juce::String (r.output.limitGrMeanDb, 2),
                             juce::String (r.output.glueGrMaxDb, 2),
                             juce::String (inc.balanceRecover, 1) + "/" + juce::String (inc.balanceTame, 1),
                             signedText (inc.colorToneDb, 2),
                             juce::String (inc.colorGlue, 1) + "/" + juce::String (inc.colorHeat, 1),
                             signedText (inc.limitGainDb, 2), juce::String (inc.limitClip, 1),
                             r.plan.limitedByConstraints ? utf8 ("是") : utf8 ("否") });
            };

            addRow (0, 0.0f);

            for (int style = 0; style < 4; ++style)
                for (auto glaze : { 50.0f, 100.0f })
                    addRow (style, glaze);

            for (const auto& line : table.render())
                logMessage (line);
        }
    };

    static GlazeBehaviourReport glazeBehaviourReport;
}
