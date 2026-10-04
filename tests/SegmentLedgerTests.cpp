/*
  ==============================================================================

    SegmentLedgerTests.cpp
    T-038 House 档按小节记账（Source/dsp/SegmentLedger.h，设计规格-曲风-House学习方式 第 2、3 节）的自动测试。

    直接驱动段落账：一个小的「三路数据」模拟（Feeder）按分析器同样的规矩喂——每块先 beginBlock，再把这一块里交齐的 100 ms 子块
    按起点、10 ms 帧按分析窗起点找段累加，最后按两路都越过的时刻 finalizeBefore。子块的 K 加权均方与帧的低频占比由测试按时间给，
    所以每段的段响度与 lowShare 可以精确设计。覆盖：锚点与整数样本边界（从小节起点、从小节中途开始）、按位置覆盖（跳回重放、循环）、
    2 秒退路（有位置按位置分格、没位置顺排）、容量上限、两道门（前奏 → drop、断奏、高频响段挡在低频门外）、新响段、
    「已听」按标称长度的一半、不同宿主分块大小下逐位相同（恒定速度）、播放头小抖动不重新锚定而变速重新锚定、音频线程零分配。
    分析器接上段落账之后的整体行为（三路真实数据、满编段画像）见 MusicAnalyzerTests 的 House 用例。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/SegmentLedger.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace celadon::test
{
    namespace
    {
        using glaze::HostInfo;
        using glaze::SegmentLedger;
        using glaze::SegmentMode;

        constexpr double rate = 48000.0;
        constexpr int subLength = 4800;   // 100 ms
        constexpr int hop = 480;          // 10 ms

        double energyFor (double lufs)   { return std::pow (10.0, 0.1 * (lufs + 0.691)); }

        // 恒定速度、4/4、从样本 startPosition 起在播的宿主（PPQ 与位置严格一致；小节起点按 PPQ ÷ 4 推）。
        HostInfo barHost (std::int64_t position, double bpm, bool withBarStart = true)
        {
            HostInfo h;
            h.hasPlayState = true;
            h.isPlaying = true;
            h.hasPosition = true;
            h.positionSamples = position;
            h.hasTempo = true;
            h.bpm = bpm;
            h.hasPpq = true;
            h.ppqPosition = (double) position * bpm / (60.0 * rate);
            h.hasBarStart = withBarStart;
            h.ppqLastBarStart = std::floor (h.ppqPosition / 4.0) * 4.0;
            h.hasTimeSignature = true;
            h.timeSignatureNumerator = 4;
            h.timeSignatureDenominator = 4;
            return h;
        }

        HostInfo positionOnlyHost (std::int64_t position)
        {
            HostInfo h;
            h.hasPlayState = true;
            h.isPlaying = true;
            h.hasPosition = true;
            h.positionSamples = position;
            return h;
        }

        // 三路数据的最小模拟（见文件头）。loudnessAt / lowShareAt / effectiveAt 按分析器时间给每个子块 / 帧的读数。
        struct Feeder
        {
            Feeder()
            {
                ledger.prepare (rate, subLength);
            }

            void block (int length, const HostInfo& host)
            {
                ledger.beginBlock (now, length, host);
                const auto end = now + (std::int64_t) length;

                for (; subFed + subLength <= end; subFed += subLength)
                {
                    if (auto* s = ledger.openSegmentAt (subFed, subHint))
                    {
                        ++s->subBlocks;

                        if (effectiveAt (subFed))
                        {
                            ++s->effectiveSubBlocks;
                            s->effectiveEnergy += energyFor (loudnessAt (subFed));
                        }
                    }
                }

                for (; frameFed + hop <= end; frameFed += hop)
                {
                    if (auto* s = ledger.openSegmentAt (frameFed, frameHint))
                    {
                        const auto share = lowShareAt (frameFed);
                        ++s->frames;
                        s->frameSeconds += (double) hop / rate;
                        s->powerSum[0] += share;
                        s->powerSum[12] += 1.0 - share;
                    }
                }

                now = end;
                ledger.finalizeBefore (juce::jmin (subFed, frameFed));
            }

            // 从宿主位置 position 起播 seconds 秒（块长 blockSize；≤ 0 = 随机 1 ～ 4096，种子 seed）。host 按位置给宿主信息。
            void play (std::int64_t& position, double seconds, int blockSize, const std::function<HostInfo (std::int64_t)>& host,
                       juce::int64 seed = 1)
            {
                juce::Random random (seed);
                const auto total = (std::int64_t) std::llround (seconds * rate);

                for (std::int64_t done = 0; done < total;)
                {
                    const auto length = (int) juce::jmin ((std::int64_t) (blockSize > 0 ? blockSize : 1 + random.nextInt (4096)), total - done);
                    block (length, host (position));
                    position += length;
                    done += length;
                }
            }

            // 跳播 / 回绕：分析器在这里断开窗口，段落账就地定稿（与 MusicAnalyzer::beginDiscontinuity 同一调用）。
            void jump()
            {
                ledger.endEpoch (now);
                subFed = ((now + subLength - 1) / subLength) * subLength;   // 跨越不连续点的子块分析器会丢掉
                frameFed = ((now + hop - 1) / hop) * hop;
            }

            SegmentLedger ledger;
            std::int64_t now = 0, subFed = 0, frameFed = 0;
            int subHint = -1, frameHint = -1;
            std::function<double (std::int64_t)> loudnessAt = [] (std::int64_t) { return -12.0; };
            std::function<double (std::int64_t)> lowShareAt = [] (std::int64_t) { return 0.4; };
            std::function<bool (std::int64_t)> effectiveAt = [] (std::int64_t) { return true; };
        };

        bool sameDouble (double a, double b)   { return std::memcmp (&a, &b, sizeof (double)) == 0; }
        bool sameFloat (float a, float b)      { return std::memcmp (&a, &b, sizeof (float)) == 0; }

        bool sameSegment (const SegmentLedger::Segment& a, const SegmentLedger::Segment& b)
        {
            return a.mode == b.mode && a.positioned == b.positioned && a.key == b.key && a.start == b.start && a.end == b.end
                && a.nominalLength == b.nominalLength && a.subBlocks == b.subBlocks && a.effectiveSubBlocks == b.effectiveSubBlocks
                && sameDouble (a.effectiveEnergy, b.effectiveEnergy) && a.frames == b.frames && sameDouble (a.frameSeconds, b.frameSeconds)
                && sameDouble (a.powerSum[0], b.powerSum[0]) && sameDouble (a.powerSum[12], b.powerSum[12])
                && a.heard == b.heard && a.full == b.full && sameFloat (a.loudnessLufs, b.loudnessLufs) && sameFloat (a.lowShare, b.lowShare);
        }

        bool sameSummary (const glaze::HouseGateSummary& a, const glaze::HouseGateSummary& b)
        {
            return a.valid == b.valid && a.mode == b.mode && sameFloat (a.referenceLufs, b.referenceLufs)
                && sameFloat (a.lowShareReference, b.lowShareReference) && a.heardSegments == b.heardSegments
                && a.fullSegments == b.fullSegments && sameDouble (a.heardSeconds, b.heardSeconds) && sameDouble (a.fullSeconds, b.fullSeconds);
        }

        constexpr double houseBpm = 124.0;
        const double samplesPerBar = 4.0 * rate * 60.0 / houseBpm;   // 92903.2258…（不是整数）
    }

    class SegmentLedgerTests  : public juce::UnitTest
    {
    public:
        SegmentLedgerTests() : juce::UnitTest ("SegmentLedger", "Celadon") {}

        void runTest() override
        {
            testEmpty();
            testIntegerBarBoundaries();
            testMidBarStart();
            testOverwriteByPosition();
            testFallbackSegments();
            testCapacity();
            testGates();
            testHeardNeedsHalfABar();
            testQuietSegmentIsNotInherited();
            testBlockSizeIndependence();
            testReanchoring();
            testNoAllocation();
            testStartJustBeforeBarLine();   // T-048 阶段 1 修订（代理 B2）
        }

    private:
        void testEmpty()
        {
            beginTest ("empty after prepare and reset; mode none, no gate result");

            SegmentLedger ledger;
            ledger.prepare (rate, subLength);
            expect (ledger.getMode() == SegmentMode::none);
            expectEquals (ledger.getHeardSegments(), 0);
            expectEquals (ledger.getNumStored(), 0);
            expect (! ledger.getSummary().valid);
            expect (! ledger.isCurrentSegmentFull());

            ledger.reset();
            expectEquals (ledger.getHeardSegments(), 0);
            expect (ledger.getMode() == SegmentMode::none);
        }

        // 依据：设计规格第 2.1 节「锚点样本 + round(n × 每小节样本数)」。124 BPM 4/4 一小节 92903.2258 样本，从 PPQ 0 起播：
        // 第 n 段 = [round(n·L), round((n + 1)·L))，键 = 小节起点 PPQ × 16 = 64 n，标称长度逐段是 92903 或 92904。
        void testIntegerBarBoundaries()
        {
            beginTest ("bars: one segment per bar at anchor + round(n x samples per bar), keyed by the bar's PPQ");

            Feeder f;
            std::int64_t position = 0;
            f.play (position, 12.0 * samplesPerBar / rate + 0.5, 512, [] (std::int64_t p) { return barHost (p, houseBpm); });

            expect (f.ledger.getMode() == SegmentMode::bars);
            expectEquals (f.ledger.getNumStored(), 12, "12 complete bars finalized");

            auto exact = true;

            for (int n = 0; n < f.ledger.getNumStored(); ++n)
            {
                const auto& s = f.ledger.getStored (n);
                const auto start = (std::int64_t) std::llround ((double) n * samplesPerBar);
                const auto end = (std::int64_t) std::llround ((double) (n + 1) * samplesPerBar);
                exact = exact && s.start == start && s.end == end && s.nominalLength == end - start && s.key == 64 * n
                     && s.mode == SegmentMode::bars && s.positioned;
            }

            expect (exact, "integer boundaries follow round(n * 92903.2258), keys 64 n");
            expect (f.ledger.getStored (0).heard && std::abs (f.ledger.getStored (0).effectiveSeconds - 1.9) < 0.11,
                    "a bar holds ~19 sub-block starts (1.9 s of effective audio)");
        }

        // 依据：第 2.1 节「getPpqPositionOfLastBarStart 优先」。从第 1 小节的第 2.5 拍起播：这一次实例从开始播放的样本起，
        // 段尾仍在小节线上（锚点 = 此刻 − 2.5 拍）；没有 ppqLastBarStart 时按 PPQ ÷ 4 推，结果相同。
        void testMidBarStart()
        {
            beginTest ("bars: playback from mid-bar - the first instance starts at the play position, the bar line stays on the grid");

            const auto beat = rate * 60.0 / houseBpm;
            const auto startPosition = (std::int64_t) std::llround (2.5 * beat);

            for (const auto withBarStart : { true, false })
            {
                Feeder f;
                auto position = startPosition;
                f.play (position, 6.0, 1000, [withBarStart] (std::int64_t p) { return barHost (p, houseBpm, withBarStart); });

                // 锚点：此刻（分析器时间 0）减去 2.5 拍的样本数（按宿主 PPQ 算，再取整）。
                const auto ppq = (double) startPosition * houseBpm / (60.0 * rate);
                const auto origin = -(std::int64_t) std::llround (ppq * beat);
                const auto& first = f.ledger.getStored (0);
                const auto& second = f.ledger.getStored (1);
                const auto tag = juce::String (withBarStart ? "with" : "without") + " ppqLastBarStart";

                expect (first.start == 0 && first.end == origin + (std::int64_t) std::llround (samplesPerBar), "first instance: " + tag);
                expect (first.key == 0 && first.nominalLength == (std::int64_t) std::llround (samplesPerBar), "first key / nominal: " + tag);
                expect (second.start == first.end && second.end == origin + (std::int64_t) std::llround (2.0 * samplesPerBar)
                        && second.key == 64, "second bar on the grid: " + tag);
                expect (! first.heard && second.heard, "the partial first bar (1.5 of 4 beats) is not counted as heard: " + tag);
            }
        }

        // 依据：第 2.1 节「段按位置存：同一小节重放时覆盖旧值，不重复计入」。播 6 小节，跳回第 2 小节重放 2 小节（响度换成 −6）：
        // 账本仍是 6 段，第 2、3 小节是新值；4 小节循环 4 遍：只有 4 段。
        void testOverwriteByPosition()
        {
            beginTest ("replaying a bar overwrites it by position; a loop counts once");

            {
                Feeder f;
                std::int64_t position = 0;
                f.play (position, 6.0 * samplesPerBar / rate + 0.3, 777, [] (std::int64_t p) { return barHost (p, houseBpm); });
                expectEquals (f.ledger.getNumStored(), 6);

                f.jump();
                position = (std::int64_t) std::llround (2.0 * samplesPerBar);
                f.loudnessAt = [] (std::int64_t) { return -6.0; };
                f.play (position, 2.0 * samplesPerBar / rate + 0.3, 777, [] (std::int64_t p) { return barHost (p, houseBpm); });
                f.jump();

                // 7 = 第 0 ～ 5 小节 + 第一次跳走时只听了 0.3 s 的第 6 小节；重放的第 2、3 小节就地覆盖，只听了 0.3 s 的第 4 小节不毁掉旧值。
                expectEquals (f.ledger.getNumStored(), 7, "bars 2 and 3 overwritten in place; the 0.3 s re-entry into bar 4 keeps the old bar");
                auto replayedLoud = true, othersQuiet = true;

                for (int i = 0; i < f.ledger.getNumStored(); ++i)
                {
                    const auto& s = f.ledger.getStored (i);

                    if (s.key == 128 || s.key == 192)
                        replayedLoud = replayedLoud && std::abs (s.loudnessLufs - (-6.0f)) < 1.0e-3f;
                    else if (s.key < 128)
                        othersQuiet = othersQuiet && std::abs (s.loudnessLufs - (-12.0f)) < 1.0e-3f;
                }

                expect (replayedLoud, "the replayed bars carry the new loudness");
                expect (othersQuiet, "bars that were not replayed keep theirs");
                expectEquals (f.ledger.getHeardSegments(), 6, "heard bars are distinct bars: 6, not 8");
            }

            {
                Feeder f;
                std::int64_t position = 0;
                const auto loopLength = (std::int64_t) std::llround (4.0 * samplesPerBar);

                for (int pass = 0; pass < 4; ++pass)
                {
                    position = 0;
                    f.play (position, (double) loopLength / rate, 512, [] (std::int64_t p) { return barHost (p, houseBpm); });
                    f.jump();
                }

                expectEquals (f.ledger.getNumStored(), 4, "a 4-bar loop played 4 times is 4 segments");
                expectEquals (f.ledger.getHeardSegments(), 4);
            }
        }

        // 依据：第 2.1 节「拿不到拍点（没有位置、没在播放、BPM 无效）时改成每 2 秒一段」。有位置时按位置分格（跳回同样覆盖），
        // 没位置时按时间顺排（不去重）；BPM 无效（0）、小节长超出 0.5 ～ 6.4 s（30 BPM）同样退回 2 秒段。
        void testFallbackSegments()
        {
            beginTest ("no tempo: 2-second segments - keyed by the host position when there is one, sequential otherwise");

            {
                Feeder f;
                std::int64_t position = 0;
                f.play (position, 9.0, 512, [] (std::int64_t p) { return positionOnlyHost (p); });
                expect (f.ledger.getMode() == SegmentMode::seconds);
                expectEquals (f.ledger.getNumStored(), 4, "four complete 2-second cells");

                auto grid = true;

                for (int i = 0; i < f.ledger.getNumStored(); ++i)
                {
                    const auto& s = f.ledger.getStored (i);
                    grid = grid && s.positioned && s.key == i && s.start == 96000 * i && s.end == 96000 * (i + 1);
                }

                expect (grid, "cells of 96000 samples keyed by position / 96000");

                f.jump();
                position = 96000;
                f.play (position, 4.5, 512, [] (std::int64_t p) { return positionOnlyHost (p); });
                f.jump();
                expectEquals (f.ledger.getNumStored(), 5, "replaying cells 1 and 2 overwrites them (cell 4 is new)");
            }

            {
                Feeder f;
                std::int64_t position = 0;
                f.play (position, 9.0, 512, [] (std::int64_t) { return HostInfo {}; });
                f.jump();
                position = 0;
                f.play (position, 4.5, 512, [] (std::int64_t) { return HostInfo {}; });
                f.jump();
                expect (f.ledger.getMode() == SegmentMode::seconds);
                expect (! f.ledger.getStored (0).positioned);
                expectEquals (f.ledger.getNumStored(), 8, "without a position nothing can be deduplicated");
            }

            for (const auto bpm : { 0.0, 30.0 })
            {
                Feeder f;
                std::int64_t position = 0;
                f.play (position, 3.0, 512, [bpm] (std::int64_t p) { auto h = barHost (p, 124.0); h.bpm = bpm; return h; });
                expect (f.ledger.getMode() == SegmentMode::seconds, "BPM " + juce::String (bpm, 0) + " falls back to 2-second segments");
            }
        }

        // 依据：第 2.1 节「容量 512 段，在 prepare 里分配；满了以后不再收新段」。
        void testCapacity()
        {
            beginTest ("capacity: 512 segments; further segments are not stored and the ledger says so");

            Feeder f;
            std::int64_t position = 0;
            f.play (position, 520.0 * 2.0 + 1.0, 4096, [] (std::int64_t p) { return positionOnlyHost (p); });

            expectEquals (f.ledger.getNumStored(), SegmentLedger::maxSegments);
            expect (f.ledger.isStoreFull(), "the store reports that it is full");
            expectEquals (f.ledger.getHeardSegments(), SegmentLedger::maxSegments);
        }

        // 依据：第 2.3 节两道门。32 小节前奏（−14 LUFS、lowShare 0.2）之后接 16 小节 drop（−8 LUFS、0.5），再接 4 小节响但没有低频的
        // 段（−8 LUFS、0.05）与 4 小节 breakdown（−20 LUFS、0.3）：
        //   - 只听了前奏时前奏小节全部是满编段（门是相对的）；
        //   - 听完之后 S_ref = drop 的响度（第 95 百分位），前奏、breakdown 掉出响度门；高频响段过了响度门、挡在低频门外；满编 = 16 小节；
        //   - drop 的第一小节是新响段（比前奏的 S_ref 高 3 LU 以上且过低频门）；第二小节进来时 drop 只占 1/33 的时长，第 95 百分位还在
        //     前奏，所以它也算；第三小节起 S_ref 已到 drop，不再算。
        void testGates()
        {
            beginTest ("two gates: an intro is full until the drop arrives; then only the drop passes (loudness and low-share gates)");

            Feeder f;
            const auto bar = [] (std::int64_t t) { return (int) ((double) t / samplesPerBar); };
            f.loudnessAt = [bar] (std::int64_t t) { const auto n = bar (t); return n < 32 ? -14.0 : n < 48 ? -8.0 : n < 52 ? -8.0 : -20.0; };
            f.lowShareAt = [bar] (std::int64_t t) { const auto n = bar (t); return n < 32 ? 0.2 : n < 48 ? 0.5 : n < 52 ? 0.05 : 0.3; };
            std::int64_t position = 0;
            const auto host = [] (std::int64_t p) { return barHost (p, houseBpm); };

            f.play (position, 32.0 * samplesPerBar / rate + 0.3, 512, host);
            const auto intro = f.ledger.getSummary();
            expect (intro.valid && intro.heardSegments == 32 && intro.fullSegments == 32, "intro only: all 32 bars are full");
            expectWithinAbsoluteError (intro.referenceLufs, -14.0f, 0.051f);
            expectWithinAbsoluteError (intro.lowShareReference, 0.2f, 0.00051f);

            std::uint64_t cursor = f.ledger.getEventCount();
            f.play (position, 24.0 * samplesPerBar / rate, 512, host);
            const auto all = f.ledger.getSummary();

            logMessage ("  after intro + drop + bass-less + breakdown: S_ref " + juce::String (all.referenceLufs, 2) + " LUFS, low ref "
                        + juce::String (all.lowShareReference, 3) + ", heard " + juce::String (all.heardSegments) + ", full "
                        + juce::String (all.fullSegments));
            expectWithinAbsoluteError (all.referenceLufs, -8.0f, 0.051f);
            expectWithinAbsoluteError (all.lowShareReference, 0.5f, 0.00051f);
            expectEquals (all.fullSegments, 16, "only the 16 drop bars are full");

            auto fullAreDrop = true;

            for (int i = 0; i < f.ledger.getNumStored(); ++i)
            {
                const auto& s = f.ledger.getStored (i);
                const auto n = (int) (s.key / 64);
                fullAreDrop = fullAreDrop && s.full == (n >= 32 && n < 48);
            }

            expect (fullAreDrop, "full flags: drop bars only (intro and breakdown fail the loudness gate, the bass-less section the low gate)");

            std::array<SegmentLedger::Event, SegmentLedger::eventCapacity> events {};
            const auto got = f.ledger.readEvents (cursor, events.data(), (int) events.size());
            expect (got > 0);
            auto surges = 0, firstSurge = -1;

            for (int i = 0; i < std::abs (got); ++i)
                if (events[(size_t) i].surge)
                {
                    ++surges;

                    if (firstSurge < 0)
                        firstSurge = i;
                }

            expectEquals (surges, 2, "new-loud-section events: the first two drop bars (until S_ref has moved up)");
            expectEquals (firstSurge, 0, "the first one is the first drop bar");
            expect (events[1].surge && ! events[2].surge, "from the third drop bar on S_ref is the drop");
            expect (f.ledger.getSurgeCount() == 2u);
        }

        // 依据：SegmentLedger.h 文件头第五节（T-038 评审 F1 返修）。安静的 breakdown（有效音频为 0、不进账本）不得沿用前面 drop 的判定：
        // 播够 0.5 s 就是 quiet、不是满编段；只有段刚开始才退回上一段，而上一段若也安静，退回的就是「安静」，不是很久以前的 drop。
        void testQuietSegmentIsNotInherited()
        {
            beginTest ("a quiet segment (no effective audio) reads quiet and is not full; only a just-started segment falls back to the last finalized one");

            Feeder f;
            const auto bar = [] (std::int64_t t) { return (int) ((double) t / samplesPerBar); };
            f.loudnessAt = [bar] (std::int64_t t) { return bar (t) < 8 ? -14.0 : -8.0; };
            f.lowShareAt = [bar] (std::int64_t t) { return bar (t) < 8 ? 0.2 : 0.5; };
            f.effectiveAt = [bar] (std::int64_t t) { const auto n = bar (t); return n < 24 || n >= 32; };   // 第 24 ～ 31 小节：安静
            std::int64_t position = 0;
            const auto host = [] (std::int64_t p) { return barHost (p, houseBpm); };
            const auto playTo = [&] (double bars, double extraSeconds)
            {
                const auto target = (std::int64_t) std::llround (bars * samplesPerBar + extraSeconds * rate);
                f.play (position, (double) (target - position) / rate, 512, host);
            };

            float loudness = 0.0f, lowShare = 0.0f;

            playTo (24.0, 0.2);   // drop 之后、安静段刚开始：退回上一段（drop 最后一小节）
            expect (f.ledger.getCurrentReading (loudness, lowShare) == SegmentLedger::Reading::measured, "just after the drop: falls back to the last drop bar");
            expectWithinAbsoluteError (loudness, -8.0f, 0.051f);
            expect (f.ledger.isCurrentSegmentFull(), "the bar line right after the drop does not flicker");

            playTo (24.0, 1.0);   // 安静段播了 1 s
            loudness = lowShare = 123.0f;
            expect (f.ledger.getCurrentReading (loudness, lowShare) == SegmentLedger::Reading::quiet, "1 s into the quiet section: quiet");
            expect (loudness == 123.0f && lowShare == 123.0f, "quiet leaves the outputs unchanged");
            expect (! f.ledger.isCurrentSegmentFull(), "a quiet section is not full");

            playTo (28.0, 0.2);   // 安静段中间的小节刚开始：上一段也安静
            expect (f.ledger.getCurrentReading (loudness, lowShare) == SegmentLedger::Reading::quiet, "inside the quiet section a new bar inherits quiet, not the old drop");
            expect (! f.ledger.isCurrentSegmentFull());

            playTo (32.0, 1.0);   // drop 回来
            expect (f.ledger.getCurrentReading (loudness, lowShare) == SegmentLedger::Reading::measured, "the drop comes back: measured");
            expectWithinAbsoluteError (lowShare, 0.5f, 0.00051f);
            expect (f.ledger.isCurrentSegmentFull());
        }

        // 依据：SegmentLedger.h「有效音频 ≥ 标称长度的一半才算已听」；没有有效音频的段不进账本。
        void testHeardNeedsHalfABar()
        {
            beginTest ("a bar counts as heard with effective audio for at least half its length; silent bars are not stored");

            Feeder f;
            const auto bar = [] (std::int64_t t) { return (int) ((double) t / samplesPerBar); };
            f.effectiveAt = [bar] (std::int64_t t)
            {
                const auto n = bar (t);
                const auto into = (double) t - (double) n * samplesPerBar;
                return n == 1 ? into < 0.4 * samplesPerBar : n == 2 ? false : true;   // 第 1 小节只有 40 % 有效，第 2 小节全静音
            };

            std::int64_t position = 0;
            f.play (position, 4.0 * samplesPerBar / rate + 0.3, 512, [] (std::int64_t p) { return barHost (p, houseBpm); });

            expectEquals (f.ledger.getNumStored(), 3, "the silent bar 2 is not stored");
            expect (f.ledger.getStored (0).heard && ! f.ledger.getStored (1).heard && f.ledger.getStored (2).heard);
            expectEquals (f.ledger.getHeardSegments(), 2);
        }

        // 依据：第 2.1 节「恒定速度下，任意分块得到的画像逐位相同」。同一段 30 小节的素材（响度、低频随时间变）按块长 512、1、333、
        // 4096 与两种随机块长喂：账本的每一段与门控结果逐位相同。
        void testBlockSizeIndependence()
        {
            beginTest ("segments and gate results are bit-identical for any host block size at a constant tempo");

            const auto run = [] (int blockSize, juce::int64 seed)
            {
                auto f = std::make_unique<Feeder>();
                f->loudnessAt = [] (std::int64_t t) { return -16.0 + 6.0 * std::sin ((double) t * 1.0e-5); };
                f->lowShareAt = [] (std::int64_t t) { return 0.35 + 0.2 * std::sin ((double) t * 3.0e-6); };
                std::int64_t position = 12345;   // 不从小节线起播
                f->play (position, 30.0 * samplesPerBar / rate, blockSize,
                         [] (std::int64_t p) { return barHost (p, houseBpm); }, seed);
                return f;
            };

            const auto reference = run (512, 1);
            expect (reference->ledger.getNumStored() >= 28 && reference->ledger.getSummary().fullSegments > 0, "the run exercises the gates");

            for (const auto& [blockSize, seed] : { std::pair<int, juce::int64> { 1, 1 }, std::pair<int, juce::int64> { 333, 1 },
                                                   std::pair<int, juce::int64> { 4096, 1 }, std::pair<int, juce::int64> { 0, 5 },
                                                   std::pair<int, juce::int64> { 0, 77 } })
            {
                const auto other = run (blockSize, seed);
                auto same = other->ledger.getNumStored() == reference->ledger.getNumStored()
                         && sameSummary (other->ledger.getSummary(), reference->ledger.getSummary());

                for (int i = 0; same && i < reference->ledger.getNumStored(); ++i)
                    same = sameSegment (other->ledger.getStored (i), reference->ledger.getStored (i));

                expect (same, blockSize > 0 ? "block " + juce::String (blockSize) : "random blocks, seed " + juce::String (seed));
            }
        }

        // 依据：第 2.1 节「只在速度或拍号改变、或遇到不连续点时重新锚定」。播放头抖动 ±1 ms（< 50 ms 容差）：边界与恒定宿主逐位相同；
        // 中途换成 128 BPM：从换速那一块起按新速度推，之前的段不动。
        void testReanchoring()
        {
            beginTest ("a jittery playhead keeps the anchor; a tempo change re-anchors from that block on");

            const auto steady = std::make_unique<Feeder>();
            const auto jittery = std::make_unique<Feeder>();
            std::int64_t a = 0, b = 0;
            int counter = 0;
            steady->play (a, 10.0, 512, [] (std::int64_t p) { return barHost (p, houseBpm); });
            jittery->play (b, 10.0, 512, [&counter] (std::int64_t p)
            {
                auto h = barHost (p, houseBpm);
                const auto k = counter++ % 3;   // 0、+1 ms、−1 ms 轮着来（第一块是准的：锚点两边相同）
                h.ppqPosition += (k == 0 ? 0.0 : (k == 1 ? 0.001 : -0.001)) * houseBpm / 60.0;
                return h;
            });

            auto same = steady->ledger.getNumStored() == jittery->ledger.getNumStored();

            for (int i = 0; same && i < steady->ledger.getNumStored(); ++i)
                same = sameSegment (steady->ledger.getStored (i), jittery->ledger.getStored (i));

            expect (same, "jitter within the tolerance does not move any boundary");

            Feeder f;
            std::int64_t position = 0;
            f.play (position, 4.0 * samplesPerBar / rate, 512, [] (std::int64_t p) { return barHost (p, houseBpm); });
            const auto changeAt = f.now;
            const auto ppqAtChange = (double) position * houseBpm / (60.0 * rate);
            const auto faster = 128.0;
            const auto fastHost = [=] (std::int64_t p)
            {
                auto h = barHost (p, faster);
                h.ppqPosition = ppqAtChange + (double) (p - position) * faster / (60.0 * rate);
                h.ppqLastBarStart = std::floor (h.ppqPosition / 4.0) * 4.0;
                return h;
            };
            auto p2 = position;
            f.play (p2, 4.0, 512, fastHost);

            const auto newBar = 4.0 * rate * 60.0 / faster;
            auto oldBarsKept = true, newBarsOnNewGrid = false;

            for (int i = 0; i < f.ledger.getNumStored(); ++i)
            {
                const auto& s = f.ledger.getStored (i);

                if (s.end <= changeAt)
                    oldBarsKept = oldBarsKept && s.nominalLength == (std::int64_t) std::llround ((double) (i + 1) * samplesPerBar)
                                                                - (std::int64_t) std::llround ((double) i * samplesPerBar);
                else if (s.start > changeAt)
                    newBarsOnNewGrid = newBarsOnNewGrid || std::abs ((double) s.nominalLength - newBar) <= 1.0;
            }

            expect (oldBarsKept, "bars before the change keep their 124 BPM boundaries");
            expect (newBarsOnNewGrid, "bars after the change are 128 BPM bars");
        }

        // 依据：SegmentLedger.h「音频线程只拷贝、清零，不分配、不加锁」。
        void testNoAllocation()
        {
            beginTest ("the audio-thread API does not allocate (segmenting, lookups, finalizing, gating, events, resets)");

            Feeder f;
            std::int64_t position = 0;
            std::array<SegmentLedger::Event, 8> events {};
            std::uint64_t cursor = 0;
            const std::function<HostInfo (std::int64_t)> bars = [] (std::int64_t p) { return barHost (p, houseBpm); };
            const std::function<HostInfo (std::int64_t)> positions = [] (std::int64_t p) { return positionOnlyHost (p); };
            const auto before = allocationCountOnThisThread();

            for (int pass = 0; pass < 3; ++pass)
            {
                f.play (position, 6.0, 512, bars);
                juce::ignoreUnused (f.ledger.readEvents (cursor, events.data(), (int) events.size()), f.ledger.isCurrentSegmentFull(),
                                    f.ledger.getCurrentSegmentSeconds());
                f.jump();
                f.play (position, 3.0, 333, positions);
                f.ledger.reset (f.now);
            }

            // 先读出计数再断言：断言的消息字符串本身会分配，实参求值的先后不定。
            const auto allocations = (int) (allocationCountOnThisThread() - before);
            expectEquals (allocations, 0, "no operator new on the audio thread");
        }

        //==============================================================================
        // T-048 阶段 1 修订（代理 B2，任务书第 4 条；REPORT-C 5.7）：从第 N 小节起播、起点 = round(小节线)。124 BPM 一小节 92903.2258 样本
        // （每小节的小数部分 7/31）：第 33 小节线在起点之后 0.23 个样本、第 3 小节在 0.45 个样本之后——原来锚定把偏移钳在「每小节样本 − 1」
        // 再取整，原点晚一个样本，第一段成了 [0, 1) 的碎段（上一小节的键），这一小节从样本 1 起；现在这条小节线就近取整落在起点上，第一段就是
        // 这一小节、从 0 起，之后的小节线在 0 + round(k × 每小节样本数)。第 4 小节线在起点之前 0.32 个样本：原来就对，照旧。
        // 对照（照旧）：起点在小节线前半个样本及以上（第 33 小节线前 1.23 个样本、第 4 小节线前 0.68 个样本）——那几个样本就近取整属于上一小节，
        // 第一段是上一小节的 [0, 1)，这一小节从样本 1 起。块长 512 / 4096 / 随机都一样。
        void testStartJustBeforeBarLine()
        {
            beginTest ("start just before a bar line (less than half a sample): the first segment is that bar from the start sample, no 1-sample piece");

            struct Case
            {
                std::int64_t position;   // 起播的宿主位置
                int firstBar;            // 第一段是第几小节（从 1 起）
                std::int64_t firstEnd;   // 第一段的 end（分析器时间 = 起播之后的样本数）
                std::int64_t secondEnd;  // 第二段的 end
            };

            const auto line = [] (int bar) { return (double) (bar - 1) * samplesPerBar; };
            const auto bar = (std::int64_t) std::llround (samplesPerBar);
            const std::vector<Case> cases {
                { (std::int64_t) std::llround (line (33)), 33, bar, (std::int64_t) std::llround (2.0 * samplesPerBar) },   // 0.23 个样本之后
                { (std::int64_t) std::llround (line (3)), 3, bar, (std::int64_t) std::llround (2.0 * samplesPerBar) },     // 0.45 个样本之后
                { (std::int64_t) std::llround (line (4)), 4, bar, (std::int64_t) std::llround (2.0 * samplesPerBar) },     // 0.32 个样本之前
                { (std::int64_t) std::llround (line (33)) - 1, 32, 1, 1 + (std::int64_t) std::llround (line (34)) - (std::int64_t) std::llround (line (33)) },
                { (std::int64_t) std::floor (line (4)), 3, 1, 1 + (std::int64_t) std::llround (line (5)) - (std::int64_t) std::llround (line (4)) },
            };

            for (const auto& c : cases)
            {
                for (const auto blockSize : { 512, 4096, 0 })
                {
                    Feeder f;
                    auto position = c.position;
                    f.play (position, 7.0, blockSize, [] (std::int64_t p) { return barHost (p, houseBpm); }, 5);

                    const auto tag = "start " + juce::String (c.position) + ", block " + juce::String (blockSize);
                    const auto& first = f.ledger.getStored (0);
                    const auto& second = f.ledger.getStored (1);
                    expect (f.ledger.getNumStored() >= 3, tag + ": bars stored");
                    expect (first.start == 0 && first.end == c.firstEnd && first.key == 64 * (std::int64_t) (c.firstBar - 1),
                            tag + ": first segment = bar " + juce::String (c.firstBar) + " [0, " + juce::String (c.firstEnd) + "), got ["
                                + juce::String (first.start) + ", " + juce::String (first.end) + ") key " + juce::String (first.key));
                    expect (second.start == first.end && second.end == c.secondEnd && second.key == first.key + 64,
                            tag + ": the next bar follows on the grid");
                }
            }
        }
    };

    static SegmentLedgerTests segmentLedgerTests;
}
